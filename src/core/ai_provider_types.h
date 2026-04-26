#pragma once

#include <Arduino.h>
#include <FS.h>

#include <array>

namespace ai::provider {

enum class ProviderKind : uint8_t {
  OpenAI = 0,
  Claude,
  OpenRouter,
  Ollama,
  LlamaCpp,
  Unknown,
};

enum class AiMessageRole : uint8_t {
  System = 0,
  Developer,
  User,
  Assistant,
  Tool,
};

enum class AiContentPartType : uint8_t {
  Text = 0,
  ImageUrl,
  InputAudioBase64,
};

enum class AiAudioFormat : uint8_t {
  Unknown = 0,
  Pcm16,
  Wav,
  Mp3,
  Opus,
  Aac,
  Flac,
};

struct AiStreamChunk {
  String textDelta;
  String rawChunk;
  bool done = false;
  String doneReason;
};

using AiStreamChunkCallback = void (*)(const AiStreamChunk& chunk, void* userContext);

struct AiSpeechToTextChunk {
  String textDelta;
  String rawChunk;
  bool done = false;
  String doneReason;
};

using AiSpeechToTextChunkCallback =
    void (*)(const AiSpeechToTextChunk& chunk, void* userContext);

struct AiTextToSpeechChunk {
  String audioBase64Delta;
  String audioMimeType;
  String rawChunk;
  bool done = false;
  String doneReason;
};

using AiTextToSpeechChunkCallback =
    void (*)(const AiTextToSpeechChunk& chunk, void* userContext);

struct AiContentPart {
  AiContentPartType type = AiContentPartType::Text;
  String text;
  String url;
  String mediaType;
  String base64Data;
};

constexpr size_t kAiMaxMessageParts = 8;
struct AiChatMessage {
  AiMessageRole role = AiMessageRole::User;
  String content;
  std::array<AiContentPart, kAiMaxMessageParts> parts;
  size_t partCount = 0;
  String toolCallId;
  String toolName;

  bool addTextPart(const String& value) {
    if (partCount >= parts.size()) {
      return false;
    }
    AiContentPart& part = parts[partCount++];
    part.type = AiContentPartType::Text;
    part.text = value;
    return true;
  }

  bool addImageUrlPart(const String& value) {
    if (partCount >= parts.size()) {
      return false;
    }
    AiContentPart& part = parts[partCount++];
    part.type = AiContentPartType::ImageUrl;
    part.url = value;
    return true;
  }

  bool addInputAudioPart(const String& format, const String& base64) {
    if (partCount >= parts.size()) {
      return false;
    }
    AiContentPart& part = parts[partCount++];
    part.type = AiContentPartType::InputAudioBase64;
    part.mediaType = format;
    part.base64Data = base64;
    return true;
  }
};

struct AiToolDefinition {
  String name;
  String description;
  String inputSchemaJson;
  String domain;
  String workflow;
  String findQuery;
};

struct AiToolCall {
  String id;
  String type;
  String name;
  String argumentsJson;
};

struct AiRequestBodySpoolOptions {
  fs::FS* filesystem = nullptr;
  String filePath;
  bool forceSpool = false;
  bool removeAfterSend = false;
  size_t streamChunkBytes = 1024;
  size_t thresholdBytes = 32 * 1024;
};

constexpr size_t kAiMaxMessages = 16;
constexpr size_t kAiMaxTools = 50;
constexpr size_t kAiMaxToolCalls = 8;

struct AiSpeechToTextRequest {
  String model;
  String baseUrl;
  String apiKey;
  String httpReferer;
  String appTitle;
  String audioBase64;
  String audioMimeType;
  String language;
  String prompt;
  String responseFormat;
  AiRequestBodySpoolOptions bodySpool;
  bool stream = false;
  bool preferPsrAm = true;
  uint32_t timeoutMs = 45000;
  AiSpeechToTextChunkCallback streamCallback = nullptr;
  void* streamUserContext = nullptr;
};

struct AiSpeechToTextResponse {
  bool ok = false;
  uint16_t statusCode = 0;
  String text;
  String language;
  String errorCode;
  String errorMessage;
  String rawResponse;
  int32_t promptTokens = -1;
  int32_t completionTokens = -1;
  int32_t totalTokens = -1;
};

struct AiTextToSpeechRequest {
  String model;
  String baseUrl;
  String apiKey;
  String httpReferer;
  String appTitle;
  String inputText;
  String voice;
  String outputFormat;
  String language;
  String instructions;
  AiRequestBodySpoolOptions bodySpool;
  bool stream = false;
  bool preferPsrAm = true;
  uint32_t timeoutMs = 45000;
  AiTextToSpeechChunkCallback streamCallback = nullptr;
  void* streamUserContext = nullptr;
};

struct AiTextToSpeechResponse {
  bool ok = false;
  uint16_t statusCode = 0;
  String audioBase64;
  String audioMimeType;
  size_t audioBytes = 0;
  String errorCode;
  String errorMessage;
  String rawResponse;
};

struct AiInvokeRequest {
  String model;
  String prompt;
  String systemPrompt;
  String baseUrl;
  String apiKey;
  String requestId;
  String httpReferer;
  String appTitle;
  AiRequestBodySpoolOptions bodySpool;
  bool stream = false;
  uint16_t maxTokens = 512;
  float temperature = 0.7f;
  uint32_t timeoutMs = 45000;

  bool preferPsrAm = true;
  bool enableVision = false;
  bool enableAudio = false;
  bool enableToolCalls = false;
  bool enableRealtime = false;

  String audioOutputFormat;
  String audioOutputVoice;
  String toolChoice;

  std::array<AiChatMessage, kAiMaxMessages> messages;
  size_t messageCount = 0;

  std::array<AiToolDefinition, kAiMaxTools> tools;
  size_t toolCount = 0;

  AiStreamChunkCallback streamCallback = nullptr;
  void* streamUserContext = nullptr;

  void reset() {
    model = "";
    prompt = "";
    systemPrompt = "";
    baseUrl = "";
    apiKey = "";
    requestId = "";
    httpReferer = "";
    appTitle = "";
    bodySpool = AiRequestBodySpoolOptions{};
    stream = false;
    maxTokens = 512;
    temperature = 0.7f;
    timeoutMs = 45000;
    preferPsrAm = true;
    enableVision = false;
    enableAudio = false;
    enableToolCalls = false;
    enableRealtime = false;
    audioOutputFormat = "";
    audioOutputVoice = "";
    toolChoice = "";
    messageCount = 0;
    toolCount = 0;
    streamCallback = nullptr;
    streamUserContext = nullptr;
  }

  bool addMessage(const AiChatMessage& message) {
    if (messageCount >= messages.size()) {
      return false;
    }
    messages[messageCount++] = message;
    return true;
  }

  bool addMessage(AiMessageRole role,
                  const String& content,
                  const String& toolCallId = String(),
                  const String& toolName = String()) {
    if (messageCount >= messages.size()) {
      return false;
    }
    AiChatMessage& message = messages[messageCount++];
    message.role = role;
    message.content = content;
    message.partCount = 0;
    message.toolCallId = toolCallId;
    message.toolName = toolName;
    return true;
  }

  bool addTool(const AiToolDefinition& tool) {
    if (toolCount >= tools.size()) {
      return false;
    }
    tools[toolCount++] = tool;
    return true;
  }
};

struct AiInvokeResponse {
  bool ok = false;
  uint16_t statusCode = 0;
  String text;
  String finishReason;
  String errorCode;
  String errorMessage;
  String rawResponse;

  int32_t promptTokens = -1;
  int32_t completionTokens = -1;
  int32_t totalTokens = -1;

  bool realtimeAccepted = false;

  std::array<AiToolCall, kAiMaxToolCalls> toolCalls;
  size_t toolCallCount = 0;

  bool addToolCall(const AiToolCall& toolCall) {
    if (toolCallCount >= toolCalls.size()) {
      return false;
    }
    toolCalls[toolCallCount++] = toolCall;
    return true;
  }
};

struct AiHttpHeader {
  String key;
  String value;
};

constexpr size_t kAiMaxHeaders = 12;

struct AiHttpRequest {
  String method = "POST";
  String url;
  String body;
  fs::FS* bodyFs = nullptr;
  String bodyFilePath;
  bool removeBodyFileAfterSend = false;
  size_t bodyStreamChunkBytes = 1024;
  uint32_t timeoutMs = 45000;
  bool nonBlockingPreferred = false;
  bool preferPsrAm = true;
  std::array<AiHttpHeader, kAiMaxHeaders> headers;
  size_t headerCount = 0;

  bool addHeader(const String& key, const String& value) {
    if (headerCount >= headers.size()) {
      return false;
    }
    headers[headerCount++] = AiHttpHeader{key, value};
    return true;
  }
};

struct AiHttpResponse {
  int statusCode = 0;
  String body;
  String errorMessage;
  bool partial = false;
  bool streaming = false;
  size_t bodyBytes = 0;
};

enum class AiRealtimeEventKind : uint8_t {
  Unknown = 0,
  SessionOpened,
  SessionClosed,
  TextDelta,
  AudioDelta,
  ToolCallRequested,
  ToolCallCancelled,
  UsageUpdated,
  GenerationDone,
  TurnDone,
  Interrupted,
  ProviderEvent,
  Error,
};

struct AiRealtimeSessionConfig {
  String sessionUrl;
  uint32_t timeoutMs = 60000;
  bool preferPsrAm = true;
  bool enableAudioInput = false;
  bool enableAudioOutput = false;
  bool enableSessionResumption = false;
};

struct AiRealtimeMessage {
  AiRealtimeEventKind kind = AiRealtimeEventKind::Unknown;
  String eventType;
  String payload;
  String text;
  String audioBase64;
  String mimeType;
  String toolCallId;
  String toolName;
  String toolArgumentsJson;
  uint32_t sequence = 0;
  bool done = false;
  String doneReason;
};

struct AiRealtimeParsedEvent {
  AiRealtimeEventKind kind = AiRealtimeEventKind::Unknown;
  String providerEventType;
  String textDelta;
  String audioBase64;
  String audioMimeType;
  String payloadJson;
  AiToolCall toolCall;
  bool hasToolCall = false;
  bool done = false;
  String doneReason;
  int32_t promptTokens = -1;
  int32_t completionTokens = -1;
  int32_t totalTokens = -1;
  String errorCode;
  String errorMessage;

  AiStreamChunk toStreamChunk() const {
    AiStreamChunk chunk;
    chunk.textDelta = textDelta;
    chunk.rawChunk = payloadJson.length() > 0 ? payloadJson : textDelta;
    chunk.done = done;
    chunk.doneReason = doneReason;
    return chunk;
  }
};

}  // namespace ai::provider
