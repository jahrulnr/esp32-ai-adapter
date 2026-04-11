#include "providers/openai/openai_realtime_adapter.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

namespace ai::provider {

namespace {

constexpr const char* kDefaultRealtimeModel = "gpt-realtime";
constexpr const char* kDefaultRealtimeUrl = "wss://api.openai.com/v1/realtime";

String buildRealtimeUrl(const AiInvokeRequest& request, const AiRealtimeSessionConfig& sessionConfig) {
  if (sessionConfig.sessionUrl.length() > 0) {
    return sessionConfig.sessionUrl;
  }

  String model = request.model.length() > 0 ? request.model : String(kDefaultRealtimeModel);
  String url = kDefaultRealtimeUrl;
  url += "?model=";
  url += model;
  return url;
}

String jsonToString(const SpiJsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return out;
}

bool parseJson(const String& payload, SpiJsonDocument& outDoc, String& outErrorMessage) {
  const auto err = deserializeJson(outDoc, payload);
  if (!err) {
    return true;
  }
  outErrorMessage = String("Invalid realtime JSON payload: ") + err.c_str();
  return false;
}

void parseUsageObject(ArduinoJson::JsonObjectConst usage, AiRealtimeParsedEvent& outEvent) {
  if (usage.isNull()) {
    return;
  }

  if (!usage["input_tokens"].isNull()) {
    outEvent.promptTokens = usage["input_tokens"] | -1;
  } else {
    outEvent.promptTokens = usage["prompt_tokens"] | -1;
  }

  if (!usage["output_tokens"].isNull()) {
    outEvent.completionTokens = usage["output_tokens"] | -1;
  } else {
    outEvent.completionTokens = usage["completion_tokens"] | -1;
  }

  outEvent.totalTokens = usage["total_tokens"] | -1;
}

String extractErrorCode(ArduinoJson::JsonObjectConst errorObj) {
  if (errorObj.isNull()) {
    return String("provider_error");
  }
  const char* code = errorObj["code"] | nullptr;
  if (code != nullptr && code[0] != '\0') {
    return String(code);
  }
  const char* type = errorObj["type"] | nullptr;
  if (type != nullptr && type[0] != '\0') {
    return String(type);
  }
  return String("provider_error");
}

String extractErrorMessage(ArduinoJson::JsonObjectConst errorObj) {
  if (errorObj.isNull()) {
    return String("Realtime provider error");
  }
  const char* message = errorObj["message"] | nullptr;
  if (message != nullptr && message[0] != '\0') {
    return String(message);
  }
  return String("Realtime provider error");
}

}  // namespace

ProviderKind OpenAiRealtimeAdapter::kind() const {
  return ProviderKind::OpenAI;
}

const char* OpenAiRealtimeAdapter::id() const {
  return "openai.realtime";
}

bool OpenAiRealtimeAdapter::supportsRealtime() const {
  return true;
}

bool OpenAiRealtimeAdapter::buildRealtimeSessionRequest(const AiInvokeRequest& request,
                                                        const AiRealtimeSessionConfig& sessionConfig,
                                                        AiHttpRequest& outHttpRequest,
                                                        String& outErrorMessage) const {
  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing OpenAI API key";
    return false;
  }

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "GET";
  outHttpRequest.url = buildRealtimeUrl(request, sessionConfig);
  outHttpRequest.timeoutMs = sessionConfig.timeoutMs > 0 ? sessionConfig.timeoutMs : request.timeoutMs;
  outHttpRequest.nonBlockingPreferred = true;
  outHttpRequest.preferPsrAm = sessionConfig.preferPsrAm;
  outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);
  outHttpRequest.addHeader("OpenAI-Beta", "realtime=v1");

  if (request.requestId.length() > 0) {
    outHttpRequest.addHeader("X-Client-Request-Id", request.requestId);
  }

  return true;
}

bool OpenAiRealtimeAdapter::buildRealtimeTextMessage(const String& text,
                                                     AiRealtimeMessage& outMessage,
                                                     String& outErrorMessage) const {
  if (text.length() == 0) {
    outErrorMessage = "Text message is empty";
    return false;
  }

  SpiJsonDocument doc;
  doc["type"] = "response.create";

  ArduinoJson::JsonObject response = doc["response"].to<ArduinoJson::JsonObject>();
  ArduinoJson::JsonArray input = response["input"].to<ArduinoJson::JsonArray>();
  ArduinoJson::JsonObject message = input.add<ArduinoJson::JsonObject>();
  message["type"] = "message";
  message["role"] = "user";

  ArduinoJson::JsonArray content = message["content"].to<ArduinoJson::JsonArray>();
  ArduinoJson::JsonObject contentItem = content.add<ArduinoJson::JsonObject>();
  contentItem["type"] = "input_text";
  contentItem["text"] = text;

  outMessage = AiRealtimeMessage{};
  outMessage.kind = AiRealtimeEventKind::ProviderEvent;
  outMessage.eventType = "response.create";
  outMessage.text = text;
  outMessage.payload = jsonToString(doc);
  return true;
}

bool OpenAiRealtimeAdapter::buildRealtimeAudioMessage(const String& audioBase64,
                                                      const String& mimeType,
                                                      AiRealtimeMessage& outMessage,
                                                      String& outErrorMessage) const {
  if (audioBase64.length() == 0) {
    outErrorMessage = "Audio message is empty";
    return false;
  }

  SpiJsonDocument doc;
  doc["type"] = "input_audio_buffer.append";
  doc["audio"] = audioBase64;

  outMessage = AiRealtimeMessage{};
  outMessage.kind = AiRealtimeEventKind::AudioDelta;
  outMessage.eventType = "input_audio_buffer.append";
  outMessage.audioBase64 = audioBase64;
  outMessage.mimeType = mimeType;
  outMessage.payload = jsonToString(doc);
  return true;
}

bool OpenAiRealtimeAdapter::buildRealtimeToolResultMessage(const String& toolCallId,
                                                           const String& toolName,
                                                           const String& toolResultJson,
                                                           AiRealtimeMessage& outMessage,
                                                           String& outErrorMessage) const {
  if (toolCallId.length() == 0 || toolName.length() == 0) {
    outErrorMessage = "toolCallId and toolName are required";
    return false;
  }

  SpiJsonDocument doc;
  doc["type"] = "conversation.item.create";

  ArduinoJson::JsonObject item = doc["item"].to<ArduinoJson::JsonObject>();
  item["type"] = "function_call_output";
  item["call_id"] = toolCallId;
  if (toolResultJson.length() > 0) {
    item["output"] = toolResultJson;
  } else {
    item["output"] = "{}";
  }

  outMessage = AiRealtimeMessage{};
  outMessage.kind = AiRealtimeEventKind::ProviderEvent;
  outMessage.eventType = "conversation.item.create";
  outMessage.toolCallId = toolCallId;
  outMessage.toolName = toolName;
  outMessage.payload = jsonToString(doc);
  return true;
}

bool OpenAiRealtimeAdapter::parseRealtimeMessage(const AiRealtimeMessage& message,
                                                 AiRealtimeParsedEvent& outEvent,
                                                 String& outErrorMessage) const {
  outErrorMessage = String();
  outEvent = AiRealtimeParsedEvent{};

  if (message.eventType == "ws.open") {
    outEvent.kind = AiRealtimeEventKind::SessionOpened;
    outEvent.providerEventType = message.eventType;
    return true;
  }

  if (message.eventType == "ws.closed") {
    outEvent.kind = AiRealtimeEventKind::SessionClosed;
    outEvent.providerEventType = message.eventType;
    outEvent.done = true;
    outEvent.doneReason = "closed";
    return true;
  }

  if (message.eventType == "ws.error") {
    outEvent.kind = AiRealtimeEventKind::Error;
    outEvent.providerEventType = message.eventType;
    outEvent.errorCode = "transport_error";
    outEvent.errorMessage = message.payload.length() > 0 ? message.payload : String("WebSocket error");
    return true;
  }

  if (message.payload.length() == 0) {
    outEvent.kind = message.kind;
    outEvent.providerEventType = message.eventType;
    outEvent.textDelta = message.text;
    outEvent.audioBase64 = message.audioBase64;
    outEvent.audioMimeType = message.mimeType;
    outEvent.done = message.done;
    outEvent.doneReason = message.doneReason;
    return true;
  }

  SpiJsonDocument doc;
  if (!parseJson(message.payload, doc, outErrorMessage)) {
    outEvent.kind = AiRealtimeEventKind::ProviderEvent;
    outEvent.providerEventType = message.eventType;
    outEvent.payloadJson = message.payload;
    outErrorMessage = String();
    return true;
  }

  const String type = String(doc["type"] | "");
  outEvent.providerEventType = type;
  outEvent.payloadJson = message.payload;

  if (type == "error") {
    ArduinoJson::JsonObjectConst errorObj = doc["error"].as<ArduinoJson::JsonObjectConst>();
    outEvent.kind = AiRealtimeEventKind::Error;
    outEvent.errorCode = extractErrorCode(errorObj);
    outEvent.errorMessage = extractErrorMessage(errorObj);
    return true;
  }

  if (type == "session.created" || type == "session.updated") {
    outEvent.kind = AiRealtimeEventKind::SessionOpened;
    return true;
  }

  if (type == "response.output_text.delta") {
    outEvent.kind = AiRealtimeEventKind::TextDelta;
    outEvent.textDelta = String(doc["delta"] | "");
    return true;
  }

  if (type == "response.output_audio.delta") {
    outEvent.kind = AiRealtimeEventKind::AudioDelta;
    outEvent.audioBase64 = String(doc["delta"] | "");
    outEvent.audioMimeType = "audio/pcm";
    return true;
  }

  if (type == "response.function_call_arguments.done") {
    outEvent.kind = AiRealtimeEventKind::ToolCallRequested;
    outEvent.hasToolCall = true;
    outEvent.toolCall.id = String(doc["call_id"] | "");
    outEvent.toolCall.type = "function";
    outEvent.toolCall.name = String(doc["name"] | "");
    outEvent.toolCall.argumentsJson = String(doc["arguments"] | "{}");
    return true;
  }

  if (type == "response.done") {
    outEvent.kind = AiRealtimeEventKind::TurnDone;
    outEvent.done = true;
    ArduinoJson::JsonObjectConst response = doc["response"].as<ArduinoJson::JsonObjectConst>();
    outEvent.doneReason = String(response["status"] | "done");
    parseUsageObject(response["usage"].as<ArduinoJson::JsonObjectConst>(), outEvent);
    return true;
  }

  if (type == "response.created" || type == "conversation.item.created") {
    outEvent.kind = AiRealtimeEventKind::ProviderEvent;
    return true;
  }

  outEvent.kind = AiRealtimeEventKind::ProviderEvent;
  return true;
}

}  // namespace ai::provider
