#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <array>

#include "core/ai_audio_client.h"
#include "core/ai_chat_session_store.h"
#include "core/ai_provider_client.h"
#include "core/ai_provider_registry.h"
#include "core/ai_provider_types.h"
#include "core/ai_session_client.h"
#include "core/ai_tool_runtime.h"
#include "core/ai_tool_runtime_facade.h"
#include "providers/default_provider_bundle.h"
#include "transport/custom_http_transport.h"
#include "transport/websocket_session_transport.h"

namespace ai::provider {

#ifndef AIPROVIDERKIT_WORKER_STACK_BYTES
#define AIPROVIDERKIT_WORKER_STACK_BYTES (12U * 1024U)
#endif

#ifndef AIPROVIDERKIT_WORKER_PRIORITY
#define AIPROVIDERKIT_WORKER_PRIORITY 2
#endif

#ifndef AIPROVIDERKIT_WORKER_CORE
#define AIPROVIDERKIT_WORKER_CORE 1
#endif

#ifndef AIPROVIDERKIT_QUEUE_DEPTH
#define AIPROVIDERKIT_QUEUE_DEPTH 4
#endif

#ifndef AIPROVIDERKIT_WORKER_STACK_CAPS
#define AIPROVIDERKIT_WORKER_STACK_CAPS 0
#endif

struct AiProviderKitDefaults {
  uint32_t timeoutMs = 45000;
  bool preferPsrAm = true;

  uint16_t llmMaxTokens = 512;
  float llmTemperature = 0.7f;
  String llmSystemPrompt;

  String toolChoice = "auto";

  AiRequestBodySpoolOptions bodySpool;
};

struct AiProviderKitConversationOptions {
  bool enabled = false;
  uint8_t maxTurns = 10;
  bool autoCompact = false;
  uint8_t compactRetainTurns = 10;
};

struct AiProviderKitConfig {
  AiProviderKitDefaults defaults;
  AiProviderKitConversationOptions conversation;

  IAiChatSessionStore* sessionStore = nullptr;
  fs::FS* sessionStoreFs = nullptr;
  AiChatSessionStoreConfig sessionStoreConfig;

  CustomHttpTransport::Config httpConfig;
  WebSocketSessionTransport::Config wsConfig;
};

struct AiProviderKitWorkerConfig {
  uint32_t stackBytes = AIPROVIDERKIT_WORKER_STACK_BYTES;
  UBaseType_t priority = AIPROVIDERKIT_WORKER_PRIORITY;
  BaseType_t core = AIPROVIDERKIT_WORKER_CORE;
  uint8_t queueDepth = AIPROVIDERKIT_QUEUE_DEPTH;
  UBaseType_t stackCaps = AIPROVIDERKIT_WORKER_STACK_CAPS;
};

struct AiProviderKitLlmRequest {
  ProviderKind providerKind = ProviderKind::Unknown;
  String prompt;
  String model;
  bool hasBaseUrl = false;
  String baseUrl;
  String systemPrompt;
  String toolChoice = "auto";
  char sessionId[24] = {0};
  uint16_t maxTokens = 256;
  uint32_t timeoutMs = 45000U;
  float temperature = 0.7f;
  bool continueOnToolError = false;
  String bootstrapToolQuery;
  String preferredToolChoice;
  uint8_t emptyResponseRetries = 0;
  uint32_t emptyResponseRetryDelayMs = 0;
  fs::FS* spoolFilesystem = nullptr;
  String spoolPath;
  bool spoolRemoveAfterSend = true;
  size_t spoolStreamChunkBytes = 1024;
  size_t spoolThresholdBytes = 24U * 1024U;
};

struct AiProviderKitLlmResult {
  bool ok = false;
  AiInvokeResponse response;
  String errorMessage;
};

struct AiProviderKitSpeechToTextRequest {
  ProviderKind providerKind = ProviderKind::Unknown;
  String audioBase64;
  String mimeType;
  String model;
  bool hasBaseUrl = false;
  String baseUrl;
  uint32_t timeoutMs = 45000U;
  fs::FS* spoolFilesystem = nullptr;
  String spoolPath;
  bool spoolRemoveAfterSend = true;
  size_t spoolStreamChunkBytes = 1024;
  size_t spoolThresholdBytes = 24U * 1024U;
};

struct AiProviderKitSpeechToTextResult {
  bool ok = false;
  AiSpeechToTextResponse response;
  String errorMessage;
};

struct AiProviderKitTextToSpeechRequest {
  ProviderKind providerKind = ProviderKind::Unknown;
  String text;
  String model;
  bool hasBaseUrl = false;
  String baseUrl;
  uint32_t timeoutMs = 45000U;
  fs::FS* spoolFilesystem = nullptr;
  String spoolPath;
  bool spoolRemoveAfterSend = true;
  size_t spoolStreamChunkBytes = 1024;
  size_t spoolThresholdBytes = 24U * 1024U;
};

struct AiProviderKitTextToSpeechResult {
  bool ok = false;
  AiTextToSpeechResponse response;
  String errorMessage;
};

class AiProviderKit {
 public:
  struct ProviderProfile {
    String id;
    String baseUrl;
    String apiKey;

    String llmModel;
    String sttModel;
    String ttsModel;
    String realtimeModel;

    String httpReferer;
    String appTitle;
  };

  AiProviderKit();
  explicit AiProviderKit(const AiProviderKitConfig& config);
  ~AiProviderKit();

  AiProviderKit(const AiProviderKit&) = delete;
  AiProviderKit& operator=(const AiProviderKit&) = delete;

  void begin();

  const AiProviderRegistry& registry() const;
  AiProviderClient* httpClient() const;
  IAiChatSessionStore* sessionStore() const;
  AiToolRuntimeRegistry& tools();
  const AiToolRuntimeRegistry& tools() const;

  AiProviderKitDefaults& defaults();
  const AiProviderKitDefaults& defaults() const;

  void setBaseUrl(ProviderKind kind, const String& baseUrl);
  void setApiKey(ProviderKind kind, const String& apiKey);
  void setHttpReferer(ProviderKind kind, const String& httpReferer);
  void setAppTitle(ProviderKind kind, const String& appTitle);

  void setLlmModel(ProviderKind kind, const String& model);
  void setSttModel(ProviderKind kind, const String& model);
  void setTtsModel(ProviderKind kind, const String& model);
  void setRealtimeModel(ProviderKind kind, const String& model);

  ProviderProfile getProfile(ProviderKind kind) const;

  void setConversationEnabled(bool enabled);
  bool openConversation(const char* sessionId, uint32_t nowMs);
  bool resetConversation(uint32_t nowMs);
  bool compactConversation(uint32_t nowMs);
  String conversationId() const;

  bool llm(ProviderKind kind,
           const String& prompt,
           bool useConversation,
           AiInvokeResponse& out,
           String& outErrorMessage);

  bool llmWithTools(ProviderKind kind,
                    const String& prompt,
                    bool useConversation,
                    AiInvokeResponse& out,
                    String& outErrorMessage,
                    const AiToolHttpLoopOptions& options = AiToolHttpLoopOptions{});

  bool sttBase64(ProviderKind kind,
                 const String& audioBase64,
                 const String& mimeType,
                 AiSpeechToTextResponse& out,
                 String& outErrorMessage);

  bool tts(ProviderKind kind,
           const String& text,
           AiTextToSpeechResponse& out,
           String& outErrorMessage);

  bool beginWorker(const AiProviderKitWorkerConfig& config = AiProviderKitWorkerConfig{});
  void loop(TickType_t waitTicks = 0);
  bool workerBusy() const;
  uint32_t workerStackHighWatermarkWords() const;
  uint32_t queuedJobCount() const;

  bool submitLlmWithToolsBlocking(const AiProviderKitLlmRequest& request,
                                  AiProviderKitLlmResult& result,
                                  uint32_t queueTimeoutMs = 0);
  bool submitSpeechToTextBlocking(const AiProviderKitSpeechToTextRequest& request,
                                  AiProviderKitSpeechToTextResult& result,
                                  uint32_t queueTimeoutMs = 0);
  bool submitTextToSpeechBlocking(const AiProviderKitTextToSpeechRequest& request,
                                  AiProviderKitTextToSpeechResult& result,
                                  uint32_t queueTimeoutMs = 0);

  bool realtimeStartWithTools(ProviderKind kind,
                              const AiRealtimeSessionConfig& sessionConfig,
                              const AiSessionClientCallbacks& callbacks,
                              String& outSessionId,
                              String& outErrorMessage);

  bool realtimeClose(const String& sessionId, const String& reason, String& outErrorMessage);
  bool realtimeSendText(const String& sessionId, const String& text, String& outErrorMessage);
  bool realtimeSendAudioBytesSmart(const String& sessionId,
                                   const uint8_t* audioBytes,
                                   size_t audioBytesLength,
                                   const String& mimeType,
                                   String& outErrorMessage);
  bool realtimeSendToolResult(const String& sessionId,
                              const String& toolCallId,
                              const String& toolName,
                              const String& toolResultJson,
                              String& outErrorMessage);

  bool isRealtimeOpen(const String& sessionId) const;

  void poll();
  bool isBusy() const;
  void cancel();

 private:
  static constexpr size_t kProviderCount = 6;

  static size_t indexFromKind(ProviderKind kind);
  ProviderProfile& profileMutable(ProviderKind kind);
  const ProviderProfile& profileConst(ProviderKind kind) const;

  void applyProviderDefaults();
  void ensureBuilt();

  bool buildInvokeRequest(ProviderKind kind,
                          const String& prompt,
                          bool useConversation,
                          bool enableTools,
                          AiInvokeRequest& out,
                          String& outErrorMessage,
                          uint32_t nowMs);

  bool appendConversationTurnsBefore(const String& prompt, uint32_t nowMs, String& outErrorMessage);
  void appendConversationTurnsAfter(const String& assistantText, uint32_t nowMs);

  bool getConversationStore(IAiChatSessionStore*& outStore) const;

  enum class QueuedJobKind : uint8_t {
    LlmWithTools = 0,
    SpeechToText,
    TextToSpeech,
  };

  struct QueuedJob {
    QueuedJobKind kind = QueuedJobKind::LlmWithTools;
    const void* request = nullptr;
    void* result = nullptr;
    SemaphoreHandle_t done = nullptr;
  };

  bool ensureQueue(uint8_t queueDepth);
  bool submitJobBlocking(QueuedJobKind kind,
                         const void* request,
                         void* result,
                         uint32_t queueTimeoutMs,
                         String& outErrorMessage);
  void processQueuedJob(QueuedJob& job);
  void processQueuedLlm(QueuedJob& job);
  void processQueuedSpeechToText(QueuedJob& job);
  void processQueuedTextToSpeech(QueuedJob& job);
  void configureQueuedBodySpool(fs::FS* filesystem,
                                const String& filePath,
                                bool removeAfterSend,
                                size_t streamChunkBytes,
                                size_t thresholdBytes);

  static void workerThunk(void* context);

  AiProviderKitConfig config_{};
  AiProviderKitDefaults defaults_{};
  AiProviderKitConversationOptions conversation_{};

  bool began_ = false;

  AiProviderRegistry registry_{};
  DefaultProviderBundle providers_{};

  CustomHttpTransport ownedHttpTransport_{};
  WebSocketSessionTransport ownedWsTransport_{};
  AiChatSessionStore ownedSessionStore_{};

  IAiChatSessionStore* sessionStore_ = nullptr;

  alignas(AiProviderClient) uint8_t httpClientStorage_[sizeof(AiProviderClient)];
  alignas(AiAudioClient) uint8_t audioClientStorage_[sizeof(AiAudioClient)];
  alignas(AiSessionClient) uint8_t sessionClientStorage_[sizeof(AiSessionClient)];
  alignas(AiToolRuntimeFacade) uint8_t toolFacadeStorage_[sizeof(AiToolRuntimeFacade)];

  AiProviderClient* httpClient_ = nullptr;
  AiAudioClient* audioClient_ = nullptr;
  AiSessionClient* sessionClient_ = nullptr;
  AiToolRuntimeRegistry toolRegistry_{};
  AiToolRuntimeFacade* toolFacade_ = nullptr;

  std::array<ProviderProfile, kProviderCount> profiles_{};
  String conversationId_{};

  QueueHandle_t queuedJobs_ = nullptr;
  TaskHandle_t workerTask_ = nullptr;
  volatile bool workerBusy_ = false;
  uint8_t queueDepth_ = AIPROVIDERKIT_QUEUE_DEPTH;
};

}  // namespace ai::provider
