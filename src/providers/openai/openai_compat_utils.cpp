#include "providers/openai/openai_compat_utils.h"

#include <ArduinoJson.h>

namespace ai::provider {

namespace {

const char* toOpenAiRole(AiMessageRole role) {
  switch (role) {
    case AiMessageRole::System:
      return "system";
    case AiMessageRole::Developer:
      return "developer";
    case AiMessageRole::User:
      return "user";
    case AiMessageRole::Assistant:
      return "assistant";
    case AiMessageRole::Tool:
      return "tool";
    default:
      return "user";
  }
}

bool isBuiltInToolChoice(const String& value) {
  return value.equalsIgnoreCase("none") || value.equalsIgnoreCase("auto") ||
         value.equalsIgnoreCase("required");
}

void writeOpenAiContentParts(const AiChatMessage& message, ArduinoJson::JsonObject outMessage) {
  if (message.partCount == 0) {
    outMessage["content"] = message.content;
    return;
  }

  ArduinoJson::JsonArray content = outMessage["content"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < message.partCount; ++i) {
    const AiContentPart& part = message.parts[i];
    ArduinoJson::JsonObject item = content.add<ArduinoJson::JsonObject>();

    if (part.type == AiContentPartType::Text) {
      item["type"] = "text";
      item["text"] = part.text;
      continue;
    }

    if (part.type == AiContentPartType::ImageUrl) {
      item["type"] = "image_url";
      ArduinoJson::JsonObject imageUrl = item["image_url"].to<ArduinoJson::JsonObject>();
      imageUrl["url"] = part.url;
      continue;
    }

    if (part.type == AiContentPartType::InputAudioBase64) {
      item["type"] = "input_audio";
      ArduinoJson::JsonObject inputAudio = item["input_audio"].to<ArduinoJson::JsonObject>();
      inputAudio["data"] = part.base64Data;
      inputAudio["format"] = part.mediaType.length() > 0 ? part.mediaType : "wav";
      continue;
    }
  }
}

void writeToolSchema(const String& schemaJson, ArduinoJson::JsonObject outFunction) {
  if (schemaJson.length() == 0) {
    outFunction["parameters"]["type"] = "object";
    return;
  }

  SpiJsonDocument schemaDoc;
  const auto parseErr = deserializeJson(schemaDoc, schemaJson);
  if (parseErr) {
    outFunction["parameters"]["type"] = "object";
    return;
  }

  outFunction["parameters"] = schemaDoc.as<ArduinoJson::JsonVariantConst>();
}

void writeTools(const AiInvokeRequest& request, SpiJsonDocument& outBody) {
  if (!request.enableToolCalls || request.toolCount == 0) {
    return;
  }

  ArduinoJson::JsonArray tools = outBody["tools"].to<ArduinoJson::JsonArray>();
  for (size_t i = 0; i < request.toolCount; ++i) {
    const AiToolDefinition& tool = request.tools[i];
    if (tool.name.length() == 0) {
      continue;
    }

    ArduinoJson::JsonObject toolItem = tools.add<ArduinoJson::JsonObject>();
    toolItem["type"] = "function";
    ArduinoJson::JsonObject function = toolItem["function"].to<ArduinoJson::JsonObject>();
    function["name"] = tool.name;
    function["description"] = tool.description;
    writeToolSchema(tool.inputSchemaJson, function);
  }

  if (request.toolChoice.length() == 0) {
    return;
  }

  if (isBuiltInToolChoice(request.toolChoice)) {
    outBody["tool_choice"] = request.toolChoice;
    return;
  }

  ArduinoJson::JsonObject toolChoice = outBody["tool_choice"].to<ArduinoJson::JsonObject>();
  toolChoice["type"] = "function";
  toolChoice["function"]["name"] = request.toolChoice;
}

void writeFallbackPromptMessages(const AiInvokeRequest& request,
                                 bool useDeveloperRole,
                                 ArduinoJson::JsonArray outMessages) {
  if (request.systemPrompt.length() > 0) {
    ArduinoJson::JsonObject systemMsg = outMessages.add<ArduinoJson::JsonObject>();
    systemMsg["role"] = useDeveloperRole ? "developer" : "system";
    systemMsg["content"] = request.systemPrompt;
  }

  ArduinoJson::JsonObject userMsg = outMessages.add<ArduinoJson::JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = request.prompt;
}

void writeMessageList(const AiInvokeRequest& request,
                      bool useDeveloperRole,
                      ArduinoJson::JsonArray outMessages) {
  if (request.messageCount == 0) {
    writeFallbackPromptMessages(request, useDeveloperRole, outMessages);
    return;
  }

  for (size_t i = 0; i < request.messageCount; ++i) {
    const AiChatMessage& message = request.messages[i];
    ArduinoJson::JsonObject item = outMessages.add<ArduinoJson::JsonObject>();
    const AiMessageRole role =
        useDeveloperRole && message.role == AiMessageRole::System ? AiMessageRole::Developer
                                                                   : message.role;
    item["role"] = toOpenAiRole(role);
    writeOpenAiContentParts(message, item);

    if (message.role == AiMessageRole::Tool) {
      if (message.toolCallId.length() > 0) {
        item["tool_call_id"] = message.toolCallId;
      }
      if (message.toolName.length() > 0) {
        item["name"] = message.toolName;
      }
    }
  }
}

void parseUsage(ArduinoJson::JsonObjectConst usageObj, AiInvokeResponse& outResponse) {
  if (usageObj.isNull()) {
    return;
  }

  outResponse.promptTokens = usageObj["prompt_tokens"] | -1;
  outResponse.completionTokens = usageObj["completion_tokens"] | -1;
  outResponse.totalTokens = usageObj["total_tokens"] | -1;
}

void parseToolCalls(ArduinoJson::JsonObjectConst message, AiInvokeResponse& outResponse) {
  if (message.isNull()) {
    return;
  }

  ArduinoJson::JsonArrayConst toolCalls = message["tool_calls"].as<ArduinoJson::JsonArrayConst>();
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

String extractOpenAiContent(ArduinoJson::JsonVariantConst content) {
  if (content.is<const char*>()) {
    return String(content.as<const char*>());
  }

  if (content.is<ArduinoJson::JsonArrayConst>()) {
    String out;
    for (ArduinoJson::JsonVariantConst part : content.as<ArduinoJson::JsonArrayConst>()) {
      const char* text = part["text"] | nullptr;
      if (text == nullptr || text[0] == '\0') {
        continue;
      }
      if (out.length() > 0) {
        out += "\n";
      }
      out += text;
    }
    return out;
  }

  return String();
}

String compactPreview(const String& raw, size_t maxChars) {
  String preview;
  if (maxChars == 0) {
    return preview;
  }

  preview.reserve(maxChars + 1);
  for (size_t i = 0; i < raw.length() && preview.length() < maxChars; ++i) {
    const char ch = raw[i];
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      preview += ' ';
      continue;
    }

    if (static_cast<unsigned char>(ch) < 0x20U || ch == 0x7f) {
      preview += '?';
      continue;
    }

    preview += ch;
  }

  return preview;
}

bool parseJsonBody(const AiHttpResponse& httpResponse, SpiJsonDocument& outDoc, String& outError) {
  const auto err = deserializeJson(outDoc, httpResponse.body);
  if (!err) {
    return true;
  }

  outError = String("JSON parse failed: ") + err.c_str();
  outError += String(" status=") + String(httpResponse.statusCode);
  outError += String(" body_len=") + String(httpResponse.body.length());
  outError += String(" body_bytes=") + String(httpResponse.bodyBytes);

  if (httpResponse.errorMessage.length() > 0) {
    outError += String(" transport=") + httpResponse.errorMessage;
  }

  const String preview = compactPreview(httpResponse.body, 180);
  if (preview.length() > 0) {
    outError += String(" preview=") + preview;
  }

  return false;
}

}  // namespace

bool buildOpenAiStyleMessages(const AiInvokeRequest& request,
                              bool useDeveloperRole,
                              SpiJsonDocument& outBody) {
  if (request.model.length() == 0) {
    return false;
  }

  if (request.prompt.length() == 0 && request.messageCount == 0) {
    return false;
  }

  outBody["model"] = request.model;
  outBody["stream"] = request.stream;

  if (request.maxTokens > 0) {
    outBody["max_tokens"] = request.maxTokens;
  }
  outBody["temperature"] = request.temperature;

  if (request.enableAudio && request.audioOutputFormat.length() > 0) {
    ArduinoJson::JsonArray modalities = outBody["modalities"].to<ArduinoJson::JsonArray>();
    modalities.add("text");
    modalities.add("audio");
    ArduinoJson::JsonObject audio = outBody["audio"].to<ArduinoJson::JsonObject>();
    audio["format"] = request.audioOutputFormat;
    if (request.audioOutputVoice.length() > 0) {
      audio["voice"] = request.audioOutputVoice;
    }
  }

  ArduinoJson::JsonArray messages = outBody["messages"].to<ArduinoJson::JsonArray>();
  writeMessageList(request, useDeveloperRole, messages);
  writeTools(request, outBody);

  return true;
}

bool parseOpenAiStyleResponse(const AiHttpResponse& httpResponse,
                              AiInvokeResponse& outResponse) {
  outResponse.statusCode =
      static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
  outResponse.rawResponse = httpResponse.body;

  SpiJsonDocument doc;
  String parseError;
  if (!parseJsonBody(httpResponse, doc, parseError)) {
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
    outResponse.errorMessage = message != nullptr ? String(message) : String("Upstream request failed");
    return false;
  }

  ArduinoJson::JsonArrayConst choices = doc["choices"].as<ArduinoJson::JsonArrayConst>();
  if (choices.isNull() || choices.size() == 0) {
    outResponse.ok = false;
    outResponse.errorCode = "empty_choices";
    outResponse.errorMessage = "choices[] is empty";
    return false;
  }

  ArduinoJson::JsonObjectConst firstChoice = choices[0].as<ArduinoJson::JsonObjectConst>();
  outResponse.finishReason = String(firstChoice["finish_reason"] | "");

  ArduinoJson::JsonObjectConst message = firstChoice["message"].as<ArduinoJson::JsonObjectConst>();
  parseToolCalls(message, outResponse);
  if (!message.isNull()) {
    outResponse.text = extractOpenAiContent(message["content"]);
  }

  if (outResponse.text.length() == 0) {
    const char* fallbackText = firstChoice["text"] | nullptr;
    if (fallbackText != nullptr) {
      outResponse.text = fallbackText;
    }
  }

  parseUsage(doc["usage"].as<ArduinoJson::JsonObjectConst>(), outResponse);
  outResponse.ok = outResponse.text.length() > 0 || outResponse.toolCallCount > 0;
  if (!outResponse.ok) {
    outResponse.errorCode = "empty_response";
    outResponse.errorMessage = "Response has no text and no tool calls";
    return false;
  }
  return true;
}

}  // namespace ai::provider
