#include "providers/claude/claude_provider.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include <cstring>

#include "core/url_utils.h"

namespace ai::provider {

namespace {
constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com";
constexpr const char* kMessagesPath = "/v1/messages";
constexpr const char* kAnthropicVersion = "2023-06-01";

bool parseJsonBody(const String& body, SpiJsonDocument& outDoc, String& outError) {
  const auto err = deserializeJson(outDoc, body);
  if (!err) {
    return true;
  }
  outError = String("JSON parse failed: ") + err.c_str();
  return false;
}

const char* toClaudeRole(AiMessageRole role) {
  switch (role) {
    case AiMessageRole::Assistant:
      return "assistant";
    case AiMessageRole::User:
      return "user";
    case AiMessageRole::Tool:
      return "user";
    default:
      return "user";
  }
}

void writeClaudeFallbackMessages(const AiInvokeRequest& request, ArduinoJson::JsonArray outMessages) {
  ArduinoJson::JsonObject userMessage = outMessages.add<ArduinoJson::JsonObject>();
  userMessage["role"] = "user";
  userMessage["content"] = request.prompt;
}

void writeClaudeMessageContent(const AiChatMessage& message, ArduinoJson::JsonObject outMessage) {
  if (message.partCount == 0) {
    outMessage["content"] = message.content;
    return;
  }

  ArduinoJson::JsonArray content = outMessage["content"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < message.partCount; ++i) {
    const AiContentPart& part = message.parts[i];

    if (part.type == AiContentPartType::Text) {
      ArduinoJson::JsonObject textBlock = content.add<ArduinoJson::JsonObject>();
      textBlock["type"] = "text";
      textBlock["text"] = part.text;
      continue;
    }

    if (part.type == AiContentPartType::ImageUrl) {
      ArduinoJson::JsonObject imageBlock = content.add<ArduinoJson::JsonObject>();
      imageBlock["type"] = "image";
      ArduinoJson::JsonObject source = imageBlock["source"].to<ArduinoJson::JsonObject>();

      if (part.url.startsWith("http://") || part.url.startsWith("https://")) {
        source["type"] = "url";
        source["url"] = part.url;
      } else {
        source["type"] = "base64";
        source["media_type"] = part.mediaType.length() > 0 ? part.mediaType : "image/jpeg";
        source["data"] = part.base64Data.length() > 0 ? part.base64Data : part.url;
      }
    }
  }
}

void writeClaudeMessages(const AiInvokeRequest& request, ArduinoJson::JsonArray outMessages) {
  if (request.messageCount == 0) {
    writeClaudeFallbackMessages(request, outMessages);
    return;
  }

  for (size_t i = 0; i < request.messageCount; ++i) {
    const AiChatMessage& message = request.messages[i];
    ArduinoJson::JsonObject outMessage = outMessages.add<ArduinoJson::JsonObject>();
    outMessage["role"] = toClaudeRole(message.role);
    writeClaudeMessageContent(message, outMessage);
  }
}

void writeClaudeTools(const AiInvokeRequest& request, SpiJsonDocument& outBody) {
  if (!request.enableToolCalls || request.toolCount == 0) {
    return;
  }

  ArduinoJson::JsonArray tools = outBody["tools"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < request.toolCount; ++i) {
    const AiToolDefinition& tool = request.tools[i];
    if (tool.name.length() == 0) {
      continue;
    }

    ArduinoJson::JsonObject item = tools.add<ArduinoJson::JsonObject>();
    item["name"] = tool.name;
    item["description"] = tool.description;

    SpiJsonDocument schemaDoc;
    if (tool.inputSchemaJson.length() > 0 &&
        deserializeJson(schemaDoc, tool.inputSchemaJson) == DeserializationError::Ok) {
      item["input_schema"] = schemaDoc.as<ArduinoJson::JsonVariantConst>();
    } else {
      item["input_schema"]["type"] = "object";
    }
  }

  if (request.toolChoice.length() == 0) {
    return;
  }

  ArduinoJson::JsonObject toolChoice = outBody["tool_choice"].to<ArduinoJson::JsonObject>();
  if (request.toolChoice.equalsIgnoreCase("auto")) {
    toolChoice["type"] = "auto";
    return;
  }
  if (request.toolChoice.equalsIgnoreCase("none")) {
    toolChoice["type"] = "none";
    return;
  }
  if (request.toolChoice.equalsIgnoreCase("any")) {
    toolChoice["type"] = "any";
    return;
  }

  toolChoice["type"] = "tool";
  toolChoice["name"] = request.toolChoice;
}

void parseClaudeUsage(ArduinoJson::JsonObjectConst usageObj, AiInvokeResponse& outResponse) {
  if (usageObj.isNull()) {
    return;
  }
  outResponse.promptTokens = usageObj["input_tokens"] | -1;
  outResponse.completionTokens = usageObj["output_tokens"] | -1;
  if (outResponse.promptTokens >= 0 && outResponse.completionTokens >= 0) {
    outResponse.totalTokens = outResponse.promptTokens + outResponse.completionTokens;
  }
 }

}  // namespace

ProviderKind ClaudeProvider::kind() const {
  return ProviderKind::Claude;
}

const char* ClaudeProvider::id() const {
  return "claude";
}

bool ClaudeProvider::buildHttpRequest(const AiInvokeRequest& request,
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

  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing Claude API key";
    return false;
  }

  SpiJsonDocument body;
  body["model"] = request.model;
  body["max_tokens"] = request.maxTokens > 0 ? request.maxTokens : 512;
  body["stream"] = request.stream;

  if (request.systemPrompt.length() > 0) {
    body["system"] = request.systemPrompt;
  }

  ArduinoJson::JsonArray messages = body["messages"].to<ArduinoJson::JsonArray>();
  writeClaudeMessages(request, messages);
  writeClaudeTools(request, body);

  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl),
              kMessagesPath);
  outHttpRequest.addHeader("Content-Type", "application/json");
  outHttpRequest.addHeader("x-api-key", request.apiKey);
  outHttpRequest.addHeader("anthropic-version", kAnthropicVersion);

  serializeJson(body, outHttpRequest.body);
  return true;
}

bool ClaudeProvider::parseHttpResponse(const AiHttpResponse& httpResponse,
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
    const char* code = doc["error"]["type"] | nullptr;
    const char* message = doc["error"]["message"] | nullptr;
    outResponse.ok = false;
    outResponse.errorCode = code != nullptr ? String(code) : String("upstream_error");
    outResponse.errorMessage =
        message != nullptr ? String(message) : String("Upstream request failed");
    return false;
  }

  outResponse.finishReason = String(doc["stop_reason"] | "");

  ArduinoJson::JsonArrayConst contentBlocks = doc["content"].as<ArduinoJson::JsonArrayConst>();
  if (contentBlocks.isNull() || contentBlocks.size() == 0) {
    outResponse.ok = false;
    outResponse.errorCode = "empty_content";
    outResponse.errorMessage = "content[] is empty";
    return false;
  }

  String text;
  for (ArduinoJson::JsonVariantConst block : contentBlocks) {
    const char* type = block["type"] | "";

    if (std::strcmp(type, "tool_use") == 0) {
      AiToolCall call;
      call.id = String(block["id"] | "");
      call.type = "function";
      call.name = String(block["name"] | "");
      String args;
      serializeJson(block["input"], args);
      call.argumentsJson = args;
      outResponse.addToolCall(call);
      continue;
    }

    if (std::strcmp(type, "text") != 0) {
      continue;
    }
    const char* chunk = block["text"] | nullptr;
    if (chunk == nullptr || chunk[0] == '\0') {
      continue;
    }
    if (text.length() > 0) {
      text += "\n";
    }
    text += chunk;
  }

  parseClaudeUsage(doc["usage"].as<ArduinoJson::JsonObjectConst>(), outResponse);

  outResponse.ok = text.length() > 0 || outResponse.toolCallCount > 0;
  if (!outResponse.ok) {
    outResponse.errorCode = "empty_response";
    outResponse.errorMessage = "No text/tool_use block found in content[]";
    return false;
  }

  outResponse.text = text;
  return true;
}

}  // namespace ai::provider
