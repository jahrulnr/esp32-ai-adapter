#include "providers/ollama/ollama_provider.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include "core/ai_http_request_body_utils.h"
#include "core/url_utils.h"

namespace ai::provider {

namespace {
constexpr const char* kDefaultBaseUrl = "http://127.0.0.1:11434";
constexpr const char* kChatPath = "/api/chat";

bool parseJsonBody(const String& body, SpiJsonDocument& outDoc, String& outError) {
  const auto err = deserializeJson(outDoc, body);
  if (!err) {
    return true;
  }
  outError = String("JSON parse failed: ") + err.c_str();
  return false;
}

const char* toOllamaRole(AiMessageRole role) {
  switch (role) {
    case AiMessageRole::System:
      return "system";
    case AiMessageRole::Assistant:
      return "assistant";
    case AiMessageRole::Tool:
      return "tool";
    case AiMessageRole::User:
      return "user";
    case AiMessageRole::Developer:
      return "system";
    default:
      return "user";
  }
}

void writeOllamaMessage(const AiChatMessage& message, ArduinoJson::JsonArray outMessages) {
  ArduinoJson::JsonObject out = outMessages.add<ArduinoJson::JsonObject>();
  out["role"] = toOllamaRole(message.role);

  if (message.partCount == 0) {
    out["content"] = message.content;
    if (message.role == AiMessageRole::Tool && message.toolName.length() > 0) {
      out["tool_name"] = message.toolName;
    }
    return;
  }

  String text;
  ArduinoJson::JsonArray images = out["images"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < message.partCount; ++i) {
    const AiContentPart& part = message.parts[i];

    if (part.type == AiContentPartType::Text) {
      if (text.length() > 0) {
        text += "\n";
      }
      text += part.text;
      continue;
    }

    if (part.type == AiContentPartType::ImageUrl) {
      if (part.base64Data.length() > 0) {
        images.add(part.base64Data);
      } else {
        images.add(part.url);
      }
      continue;
    }
  }

  out["content"] = text;
  if (message.role == AiMessageRole::Tool && message.toolName.length() > 0) {
    out["tool_name"] = message.toolName;
  }
}

void writeOllamaTools(const AiInvokeRequest& request, SpiJsonDocument& outBody) {
  if (!request.enableToolCalls || request.toolCount == 0) {
    return;
  }

  ArduinoJson::JsonArray tools = outBody["tools"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < request.toolCount; ++i) {
    const AiToolDefinition& tool = request.tools[i];
    if (tool.name.length() == 0) {
      continue;
    }

    ArduinoJson::JsonObject outTool = tools.add<ArduinoJson::JsonObject>();
    outTool["type"] = "function";
    ArduinoJson::JsonObject function = outTool["function"].to<ArduinoJson::JsonObject>();
    function["name"] = tool.name;
    function["description"] = tool.description;

    SpiJsonDocument schemaDoc;
    if (tool.inputSchemaJson.length() > 0 &&
        deserializeJson(schemaDoc, tool.inputSchemaJson) == DeserializationError::Ok) {
      function["parameters"] = schemaDoc.as<ArduinoJson::JsonVariantConst>();
    } else {
      function["parameters"]["type"] = "object";
    }
  }
}

void parseOllamaToolCalls(ArduinoJson::JsonObjectConst messageObj, AiInvokeResponse& outResponse) {
  if (messageObj.isNull()) {
    return;
  }

  ArduinoJson::JsonArrayConst toolCalls = messageObj["tool_calls"].as<ArduinoJson::JsonArrayConst>();
  if (toolCalls.isNull()) {
    return;
  }

  for (ArduinoJson::JsonVariantConst call : toolCalls) {
    AiToolCall parsed;
    parsed.id = String(call["id"] | "");
    parsed.type = String(call["type"] | "function");
    parsed.name = String(call["function"]["name"] | "");
    String args;
    serializeJson(call["function"]["arguments"], args);
    parsed.argumentsJson = args;
    if (!outResponse.addToolCall(parsed)) {
      break;
    }
  }
}
}  // namespace

ProviderKind OllamaProvider::kind() const {
  return ProviderKind::Ollama;
}

const char* OllamaProvider::id() const {
  return "ollama";
}

bool OllamaProvider::buildHttpRequest(const AiInvokeRequest& request,
                                      AiHttpRequest& outHttpRequest,
                                      String& outErrorMessage) const {
  if (request.model.length() == 0) {
    outErrorMessage = "Missing required field: model";
    return false;
  }

  if (request.prompt.length() == 0 && request.messageCount == 0) {
    outErrorMessage = "Missing required fields: model and prompt";
    return false;
  }

  SpiJsonDocument body;
  body["model"] = request.model;
  body["stream"] = request.stream;

  ArduinoJson::JsonArray messages = body["messages"].to<ArduinoJson::JsonArray>();
  if (request.messageCount > 0) {
    for (size_t i = 0; i < request.messageCount; ++i) {
      writeOllamaMessage(request.messages[i], messages);
    }
  } else {
    if (request.systemPrompt.length() > 0) {
      ArduinoJson::JsonObject systemMsg = messages.add<ArduinoJson::JsonObject>();
      systemMsg["role"] = "system";
      systemMsg["content"] = request.systemPrompt;
    }

    ArduinoJson::JsonObject userMsg = messages.add<ArduinoJson::JsonObject>();
    userMsg["role"] = "user";
    userMsg["content"] = request.prompt;
  }

  writeOllamaTools(request, body);

  ArduinoJson::JsonObject options = body["options"].to<ArduinoJson::JsonObject>();
  options["temperature"] = request.temperature;
  if (request.maxTokens > 0) {
    options["num_predict"] = request.maxTokens;
  }

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");

  if (request.apiKey.length() > 0) {
    outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);
  }

  if (!applyJsonBodyToHttpRequest(body, request.bodySpool, outHttpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_body_build_failed";
    }
    return false;
  }

  return true;
}

bool OllamaProvider::parseHttpResponse(const AiHttpResponse& httpResponse,
                                       AiInvokeResponse& outResponse) const {
  outResponse.statusCode =
      static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
  outResponse.rawResponse = httpResponse.body;

  SpiJsonDocument doc;
  String parseError;
  if (!parseJsonBody(httpResponse.body, doc, parseError)) {
    outResponse.ok = false;
    outResponse.errorCode = "invalid_json";
    outResponse.errorMessage = parseError;
    return false;
  }

  const bool isSuccess = httpResponse.statusCode >= 200 && httpResponse.statusCode < 300;
  if (!isSuccess) {
    const char* message = doc["error"] | nullptr;
    outResponse.ok = false;
    outResponse.errorCode = "upstream_error";
    outResponse.errorMessage =
        message != nullptr ? String(message) : String("Upstream request failed");
    return false;
  }

  parseOllamaToolCalls(doc["message"].as<ArduinoJson::JsonObjectConst>(), outResponse);

  const char* content = doc["message"]["content"] | nullptr;
  if (content == nullptr) {
    content = doc["response"] | nullptr;
  }

  if (content == nullptr && outResponse.toolCallCount == 0) {
    outResponse.ok = false;
    outResponse.errorCode = "empty_content";
    outResponse.errorMessage = "No content field in Ollama response";
    return false;
  }

  outResponse.ok = true;
  outResponse.text = content == nullptr ? "" : String(content);
  outResponse.finishReason = String(doc["done_reason"] | "");
  outResponse.promptTokens = doc["prompt_eval_count"] | -1;
  outResponse.completionTokens = doc["eval_count"] | -1;
  if (outResponse.promptTokens >= 0 && outResponse.completionTokens >= 0) {
    outResponse.totalTokens = outResponse.promptTokens + outResponse.completionTokens;
  }
  return true;
}

}  // namespace ai::provider
