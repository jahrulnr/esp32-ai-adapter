#include "core/ai_provider_kit.h"

#include <cstdlib>
#include <freertos/semphr.h>
#include <new>

#if defined(__has_include)
#if __has_include(<freertos/idf_additions.h>) && __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#define AIPROVIDERKIT_HAS_TASK_CAPS 1
#else
#define AIPROVIDERKIT_HAS_TASK_CAPS 0
#endif
#else
#define AIPROVIDERKIT_HAS_TASK_CAPS 0
#endif

namespace ai::provider {

namespace {

constexpr const char* kDefaultOpenAiBaseUrl = "https://api.openai.com/v1";
constexpr const char* kDefaultClaudeBaseUrl = "https://api.anthropic.com";
constexpr const char* kDefaultOpenRouterBaseUrl = "https://openrouter.ai/api/v1";

template <typename T>
T* allocateLargeObject() {
#if AIPROVIDERKIT_HAS_TASK_CAPS
  void* memory = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr) {
    memory = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_8BIT);
  }
#else
  void* memory = std::calloc(1, sizeof(T));
#endif
  if (memory == nullptr) {
    return nullptr;
  }
  return new (memory) T();
}

template <typename T>
void releaseLargeObject(T* object) {
  if (object == nullptr) {
    return;
  }
  object->~T();
#if AIPROVIDERKIT_HAS_TASK_CAPS
  heap_caps_free(object);
#else
  std::free(object);
#endif
}

}  // namespace

AiProviderKit::AiProviderKit() : AiProviderKit(AiProviderKitConfig{}) {}

AiProviderKit::AiProviderKit(const AiProviderKitConfig& config)
    : config_(config),
      defaults_(config.defaults),
      conversation_(config.conversation),
      ownedHttpTransport_(config.httpConfig),
      ownedWsTransport_(config.wsConfig),
      ownedSessionStore_(config.sessionStoreConfig) {
  applyProviderDefaults();
  ensureBuilt();
}

AiProviderKit::~AiProviderKit() {
  if (workerTask_ != nullptr) {
    vTaskDelete(workerTask_);
    workerTask_ = nullptr;
  }
  if (queuedJobs_ != nullptr) {
    vQueueDelete(queuedJobs_);
    queuedJobs_ = nullptr;
  }
  if (toolFacade_ != nullptr) {
    toolFacade_->~AiToolRuntimeFacade();
    toolFacade_ = nullptr;
  }
  if (sessionClient_ != nullptr) {
    sessionClient_->~AiSessionClient();
    sessionClient_ = nullptr;
  }
  if (audioClient_ != nullptr) {
    audioClient_->~AiAudioClient();
    audioClient_ = nullptr;
  }
  if (httpClient_ != nullptr) {
    httpClient_->~AiProviderClient();
    httpClient_ = nullptr;
  }
}

void AiProviderKit::ensureBuilt() {
  if (httpClient_ != nullptr) {
    return;
  }

  httpClient_ = new (httpClientStorage_) AiProviderClient(ownedHttpTransport_, registry_);
  audioClient_ = new (audioClientStorage_) AiAudioClient(ownedHttpTransport_, registry_);
  sessionClient_ = new (sessionClientStorage_) AiSessionClient(ownedWsTransport_, registry_);
  toolFacade_ = new (toolFacadeStorage_) AiToolRuntimeFacade(*httpClient_, *sessionClient_, toolRegistry_);

  if (config_.sessionStore != nullptr) {
    sessionStore_ = config_.sessionStore;
  } else {
    sessionStore_ = &ownedSessionStore_;
    if (config_.sessionStoreFs != nullptr) {
      sessionStore_->setStorage(config_.sessionStoreFs);
    }
  }
}

void AiProviderKit::applyProviderDefaults() {
  for (ProviderProfile& p : profiles_) {
    p = ProviderProfile{};
  }

  profileMutable(ProviderKind::OpenAI).id = "openai";
  profileMutable(ProviderKind::OpenAI).baseUrl = kDefaultOpenAiBaseUrl;

  profileMutable(ProviderKind::Claude).id = "claude";
  profileMutable(ProviderKind::Claude).baseUrl = kDefaultClaudeBaseUrl;

  profileMutable(ProviderKind::OpenRouter).id = "openrouter";
  profileMutable(ProviderKind::OpenRouter).baseUrl = kDefaultOpenRouterBaseUrl;

  profileMutable(ProviderKind::Ollama).id = "ollama";
  profileMutable(ProviderKind::LlamaCpp).id = "llamacpp";
}

void AiProviderKit::begin() {
  if (began_) {
    return;
  }
  began_ = true;

  providers_.registerAll(registry_);

  IAiChatSessionStore* store = nullptr;
  if (getConversationStore(store)) {
    store->begin();
  }
}

const AiProviderRegistry& AiProviderKit::registry() const {
  return registry_;
}

AiProviderClient* AiProviderKit::httpClient() const {
  return httpClient_;
}

IAiChatSessionStore* AiProviderKit::sessionStore() const {
  return sessionStore_;
}

AiToolRuntimeRegistry& AiProviderKit::tools() {
  return toolRegistry_;
}

const AiToolRuntimeRegistry& AiProviderKit::tools() const {
  return toolRegistry_;
}

AiProviderKitDefaults& AiProviderKit::defaults() {
  return defaults_;
}

const AiProviderKitDefaults& AiProviderKit::defaults() const {
  return defaults_;
}

size_t AiProviderKit::indexFromKind(ProviderKind kind) {
  switch (kind) {
    case ProviderKind::OpenAI:
      return 0;
    case ProviderKind::Claude:
      return 1;
    case ProviderKind::OpenRouter:
      return 2;
    case ProviderKind::Ollama:
      return 3;
    case ProviderKind::LlamaCpp:
      return 4;
    default:
      break;
  }
  return 5;
}

AiProviderKit::ProviderProfile& AiProviderKit::profileMutable(ProviderKind kind) {
  return profiles_[indexFromKind(kind)];
}

const AiProviderKit::ProviderProfile& AiProviderKit::profileConst(ProviderKind kind) const {
  return profiles_[indexFromKind(kind)];
}

void AiProviderKit::setBaseUrl(ProviderKind kind, const String& baseUrl) {
  profileMutable(kind).baseUrl = baseUrl;
}

void AiProviderKit::setApiKey(ProviderKind kind, const String& apiKey) {
  profileMutable(kind).apiKey = apiKey;
}

void AiProviderKit::setHttpReferer(ProviderKind kind, const String& httpReferer) {
  profileMutable(kind).httpReferer = httpReferer;
}

void AiProviderKit::setAppTitle(ProviderKind kind, const String& appTitle) {
  profileMutable(kind).appTitle = appTitle;
}

void AiProviderKit::setLlmModel(ProviderKind kind, const String& model) {
  profileMutable(kind).llmModel = model;
}

void AiProviderKit::setSttModel(ProviderKind kind, const String& model) {
  profileMutable(kind).sttModel = model;
}

void AiProviderKit::setTtsModel(ProviderKind kind, const String& model) {
  profileMutable(kind).ttsModel = model;
}

void AiProviderKit::setRealtimeModel(ProviderKind kind, const String& model) {
  profileMutable(kind).realtimeModel = model;
}

AiProviderKit::ProviderProfile AiProviderKit::getProfile(ProviderKind kind) const {
  return profileConst(kind);
}

void AiProviderKit::setConversationEnabled(bool enabled) {
  conversation_.enabled = enabled;
}

String AiProviderKit::conversationId() const {
  return conversationId_;
}

bool AiProviderKit::getConversationStore(IAiChatSessionStore*& outStore) const {
  outStore = nullptr;
  if (!conversation_.enabled) {
    return false;
  }
  if (sessionStore_ == nullptr) {
    return false;
  }
  outStore = sessionStore_;
  return true;
}

bool AiProviderKit::openConversation(const char* sessionId, uint32_t nowMs) {
  IAiChatSessionStore* store = nullptr;
  if (!getConversationStore(store)) {
    return false;
  }
  if (sessionId == nullptr || sessionId[0] == '\0') {
    return false;
  }
  if (!store->openChat(sessionId, nowMs)) {
    return false;
  }
  conversationId_ = sessionId;
  return true;
}

bool AiProviderKit::resetConversation(uint32_t nowMs) {
  (void)nowMs;
  IAiChatSessionStore* store = nullptr;
  if (!getConversationStore(store)) {
    conversationId_ = String();
    return true;
  }
  if (conversationId_.length() == 0) {
    return true;
  }
  const bool ok = store->reset(conversationId_.c_str());
  conversationId_ = String();
  return ok;
}

bool AiProviderKit::compactConversation(uint32_t nowMs) {
  (void)nowMs;
  IAiChatSessionStore* store = nullptr;
  if (!getConversationStore(store)) {
    return false;
  }
  if (conversationId_.length() == 0) {
    return false;
  }
  return store->compactChat(conversationId_.c_str(), conversation_.compactRetainTurns);
}

bool AiProviderKit::appendConversationTurnsBefore(const String& prompt,
                                                  uint32_t nowMs,
                                                  String& outErrorMessage) {
  IAiChatSessionStore* store = nullptr;
  if (!getConversationStore(store)) {
    return true;
  }
  if (conversationId_.length() == 0) {
    outErrorMessage = "conversation_not_open";
    return false;
  }

  store->touch(conversationId_.c_str(), nowMs);
  if (!store->appendTurn(conversationId_.c_str(), "user", prompt, nowMs)) {
    outErrorMessage = "conversation_append_user_failed";
    return false;
  }
  store->markInFlight(conversationId_.c_str(), +1, nowMs);
  return true;
}

void AiProviderKit::appendConversationTurnsAfter(const String& assistantText, uint32_t nowMs) {
  IAiChatSessionStore* store = nullptr;
  if (!getConversationStore(store)) {
    return;
  }
  if (conversationId_.length() == 0) {
    return;
  }
  store->appendTurn(conversationId_.c_str(), "assistant", assistantText, nowMs);
  store->markInFlight(conversationId_.c_str(), -1, nowMs);

  if (conversation_.autoCompact) {
    store->compactChat(conversationId_.c_str(), conversation_.compactRetainTurns);
  }
  store->cleanupChat(nowMs);
}

bool AiProviderKit::buildInvokeRequest(ProviderKind kind,
                                       const String& prompt,
                                       bool useConversation,
                                       bool enableTools,
                                       AiInvokeRequest& out,
                                       String& outErrorMessage,
                                       uint32_t nowMs) {
  out.reset();

  const ProviderProfile& profile = profileConst(kind);
  if (profile.apiKey.length() == 0 && (kind == ProviderKind::OpenAI || kind == ProviderKind::OpenRouter ||
                                      kind == ProviderKind::Claude)) {
    outErrorMessage = "missing_api_key";
    return false;
  }

  out.baseUrl = profile.baseUrl;
  out.apiKey = profile.apiKey;
  out.httpReferer = profile.httpReferer;
  out.appTitle = profile.appTitle;

  out.timeoutMs = defaults_.timeoutMs;
  out.preferPsrAm = defaults_.preferPsrAm;
  out.maxTokens = defaults_.llmMaxTokens;
  out.temperature = defaults_.llmTemperature;
  out.systemPrompt = defaults_.llmSystemPrompt;
  out.bodySpool = defaults_.bodySpool;

  out.enableToolCalls = enableTools;
  if (enableTools) {
    out.toolChoice = defaults_.toolChoice;
  }

  if (profile.llmModel.length() == 0) {
    outErrorMessage = "missing_llm_model";
    return false;
  }
  out.model = profile.llmModel;

  if (useConversation) {
    IAiChatSessionStore* store = nullptr;
    if (!getConversationStore(store)) {
      outErrorMessage = "conversation_disabled";
      return false;
    }
    if (conversationId_.length() == 0) {
      outErrorMessage = "conversation_not_open";
      return false;
    }

    if (!store->buildContextMessages(conversationId_.c_str(), out, conversation_.maxTurns)) {
      outErrorMessage = "conversation_build_context_failed";
      return false;
    }

    if (!out.addMessage(AiMessageRole::User, prompt)) {
      outErrorMessage = "conversation_request_full";
      return false;
    }

    (void)nowMs;
    return true;
  }

  out.prompt = prompt;
  return true;
}

bool AiProviderKit::llm(ProviderKind kind,
                        const String& prompt,
                        bool useConversation,
                        AiInvokeResponse& out,
                        String& outErrorMessage) {
  begin();

  const uint32_t nowMs = millis();
  AiInvokeRequest* request = allocateLargeObject<AiInvokeRequest>();
  if (request == nullptr) {
    outErrorMessage = "invoke_request_alloc_failed";
    return false;
  }
  if (!buildInvokeRequest(kind, prompt, useConversation, false, *request, outErrorMessage, nowMs)) {
    releaseLargeObject(request);
    return false;
  }

  if (useConversation) {
    if (!appendConversationTurnsBefore(prompt, nowMs, outErrorMessage)) {
      releaseLargeObject(request);
      return false;
    }
  }

  const bool ok = httpClient_->invoke(kind, *request, out);
  releaseLargeObject(request);
  if (useConversation) {
    const String assistantText = out.ok ? out.text : String();
    appendConversationTurnsAfter(assistantText, nowMs);
  }

  if (!ok || !out.ok) {
    if (outErrorMessage.length() == 0 && out.errorMessage.length() > 0) {
      outErrorMessage = out.errorMessage;
    }
    return false;
  }
  return true;
}

bool AiProviderKit::llmWithTools(ProviderKind kind,
                                 const String& prompt,
                                 bool useConversation,
                                 AiInvokeResponse& out,
                                 String& outErrorMessage,
                                 const AiToolHttpLoopOptions& options) {
  begin();

  const uint32_t nowMs = millis();
  AiInvokeRequest* request = allocateLargeObject<AiInvokeRequest>();
  if (request == nullptr) {
    outErrorMessage = "invoke_request_alloc_failed";
    return false;
  }
  if (!buildInvokeRequest(kind, prompt, useConversation, true, *request, outErrorMessage, nowMs)) {
    releaseLargeObject(request);
    return false;
  }

  if (useConversation) {
    if (!appendConversationTurnsBefore(prompt, nowMs, outErrorMessage)) {
      releaseLargeObject(request);
      return false;
    }
  }

  const bool ok = toolFacade_->invokeHttpWithTools(kind, *request, out, outErrorMessage, options);
  releaseLargeObject(request);
  if (useConversation) {
    const String assistantText = out.ok ? out.text : String();
    appendConversationTurnsAfter(assistantText, nowMs);
  }

  if (!ok || !out.ok) {
    if (outErrorMessage.length() == 0 && out.errorMessage.length() > 0) {
      outErrorMessage = out.errorMessage;
    }
    return false;
  }
  return true;
}

bool AiProviderKit::sttBase64(ProviderKind kind,
                              const String& audioBase64,
                              const String& mimeType,
                              AiSpeechToTextResponse& out,
                              String& outErrorMessage) {
  begin();

  const ProviderProfile& profile = profileConst(kind);
  if (profile.sttModel.length() == 0) {
    outErrorMessage = "missing_stt_model";
    return false;
  }

  AiSpeechToTextRequest req;
  req.baseUrl = profile.baseUrl;
  req.apiKey = profile.apiKey;
  req.httpReferer = profile.httpReferer;
  req.appTitle = profile.appTitle;
  req.model = profile.sttModel;
  req.audioBase64 = audioBase64;
  req.audioMimeType = mimeType;
  req.timeoutMs = defaults_.timeoutMs;
  req.preferPsrAm = defaults_.preferPsrAm;
  req.bodySpool = defaults_.bodySpool;

  const bool ok = audioClient_->transcribe(kind, req, out);
  if (!ok || !out.ok) {
    outErrorMessage = out.errorMessage;
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "stt_failed";
    }
    return false;
  }
  return true;
}

bool AiProviderKit::tts(ProviderKind kind,
                        const String& text,
                        AiTextToSpeechResponse& out,
                        String& outErrorMessage) {
  begin();

  const ProviderProfile& profile = profileConst(kind);
  if (profile.ttsModel.length() == 0) {
    outErrorMessage = "missing_tts_model";
    return false;
  }

  AiTextToSpeechRequest req;
  req.baseUrl = profile.baseUrl;
  req.apiKey = profile.apiKey;
  req.httpReferer = profile.httpReferer;
  req.appTitle = profile.appTitle;
  req.model = profile.ttsModel;
  req.inputText = text;
  req.timeoutMs = defaults_.timeoutMs;
  req.preferPsrAm = defaults_.preferPsrAm;
  req.bodySpool = defaults_.bodySpool;

  const bool ok = audioClient_->synthesize(kind, req, out);
  if (!ok || !out.ok) {
    outErrorMessage = out.errorMessage;
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "tts_failed";
    }
    return false;
  }
  return true;
}

bool AiProviderKit::ensureQueue(uint8_t queueDepth) {
  if (queuedJobs_ != nullptr) {
    return true;
  }

  queueDepth_ = queueDepth == 0 ? 1 : queueDepth;
  queuedJobs_ = xQueueCreate(queueDepth_, sizeof(QueuedJob*));
  return queuedJobs_ != nullptr;
}

void AiProviderKit::workerThunk(void* context) {
  auto* self = static_cast<AiProviderKit*>(context);
  if (self == nullptr) {
    vTaskDelete(nullptr);
    return;
  }

  while (true) {
    self->loop(portMAX_DELAY);
  }
}

bool AiProviderKit::beginWorker(const AiProviderKitWorkerConfig& config) {
  if (!ensureQueue(config.queueDepth)) {
    return false;
  }
  if (workerTask_ != nullptr) {
    return true;
  }

#if AIPROVIDERKIT_HAS_TASK_CAPS
  const UBaseType_t stackCaps = config.stackCaps == 0
                                    ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                    : config.stackCaps;
#ifndef CONFIG_FREERTOS_UNICORE
  if (config.core >= 0 && config.core < 2) {
    return xTaskCreatePinnedToCoreWithCaps(workerThunk,
                                           "aiKit",
                                           config.stackBytes,
                                           this,
                                           config.priority,
                                           &workerTask_,
                                           config.core,
                                           stackCaps) == pdPASS;
  }
#endif

  return xTaskCreateWithCaps(workerThunk,
                             "aiKit",
                             config.stackBytes,
                             this,
                             config.priority,
                             &workerTask_,
                             stackCaps) == pdPASS;
#else
#ifndef CONFIG_FREERTOS_UNICORE
  if (config.core >= 0 && config.core < 2) {
    return xTaskCreatePinnedToCore(workerThunk,
                                   "aiKit",
                                   config.stackBytes,
                                   this,
                                   config.priority,
                                   &workerTask_,
                                   config.core) == pdPASS;
  }
#endif

  return xTaskCreate(workerThunk,
                     "aiKit",
                     config.stackBytes,
                     this,
                     config.priority,
                     &workerTask_) == pdPASS;
#endif
}

void AiProviderKit::loop(TickType_t waitTicks) {
  if (queuedJobs_ == nullptr) {
    return;
  }

  QueuedJob* job = nullptr;
  if (xQueueReceive(queuedJobs_, &job, waitTicks) != pdTRUE || job == nullptr) {
    return;
  }

  workerBusy_ = true;
  processQueuedJob(*job);
  workerBusy_ = false;
}

bool AiProviderKit::workerBusy() const {
  return workerBusy_;
}

uint32_t AiProviderKit::workerStackHighWatermarkWords() const {
  if (workerTask_ == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(workerTask_));
}

uint32_t AiProviderKit::queuedJobCount() const {
  if (queuedJobs_ == nullptr) {
    return 0;
  }
  return static_cast<uint32_t>(uxQueueMessagesWaiting(queuedJobs_));
}

bool AiProviderKit::submitJobBlocking(QueuedJobKind kind,
                                      const void* request,
                                      void* result,
                                      uint32_t queueTimeoutMs,
                                      String& outErrorMessage) {
  if (!ensureQueue(queueDepth_)) {
    outErrorMessage = "queue_unavailable";
    return false;
  }

  SemaphoreHandle_t done = xSemaphoreCreateBinary();
  if (done == nullptr) {
    outErrorMessage = "completion_signal_unavailable";
    return false;
  }

  QueuedJob job;
  job.kind = kind;
  job.request = request;
  job.result = result;
  job.done = done;

  QueuedJob* jobPtr = &job;
  if (xQueueSend(queuedJobs_, &jobPtr, pdMS_TO_TICKS(queueTimeoutMs)) != pdTRUE) {
    vSemaphoreDelete(done);
    outErrorMessage = "queue_full";
    return false;
  }

  (void)xSemaphoreTake(done, portMAX_DELAY);
  vSemaphoreDelete(done);
  return true;
}

bool AiProviderKit::submitLlmWithToolsBlocking(const AiProviderKitLlmRequest& request,
                                               AiProviderKitLlmResult& result,
                                               uint32_t queueTimeoutMs) {
  result = AiProviderKitLlmResult{};
  String submitError;
  if (!submitJobBlocking(QueuedJobKind::LlmWithTools, &request, &result, queueTimeoutMs, submitError)) {
    result.errorMessage = submitError;
    return false;
  }
  return result.ok;
}

bool AiProviderKit::submitSpeechToTextBlocking(const AiProviderKitSpeechToTextRequest& request,
                                               AiProviderKitSpeechToTextResult& result,
                                               uint32_t queueTimeoutMs) {
  result = AiProviderKitSpeechToTextResult{};
  String submitError;
  if (!submitJobBlocking(QueuedJobKind::SpeechToText, &request, &result, queueTimeoutMs, submitError)) {
    result.errorMessage = submitError;
    return false;
  }
  return result.ok;
}

bool AiProviderKit::submitTextToSpeechBlocking(const AiProviderKitTextToSpeechRequest& request,
                                               AiProviderKitTextToSpeechResult& result,
                                               uint32_t queueTimeoutMs) {
  result = AiProviderKitTextToSpeechResult{};
  String submitError;
  if (!submitJobBlocking(QueuedJobKind::TextToSpeech, &request, &result, queueTimeoutMs, submitError)) {
    result.errorMessage = submitError;
    return false;
  }
  return result.ok;
}

void AiProviderKit::configureQueuedBodySpool(fs::FS* filesystem,
                                             const String& filePath,
                                             bool removeAfterSend,
                                             size_t streamChunkBytes,
                                             size_t thresholdBytes) {
  if (filesystem == nullptr || filePath.length() == 0) {
    defaults_.bodySpool = AiRequestBodySpoolOptions{};
    return;
  }

  defaults_.bodySpool.filesystem = filesystem;
  defaults_.bodySpool.filePath = filePath;
  defaults_.bodySpool.removeAfterSend = removeAfterSend;
  defaults_.bodySpool.streamChunkBytes = streamChunkBytes;
  defaults_.bodySpool.thresholdBytes = thresholdBytes;
}

void AiProviderKit::processQueuedJob(QueuedJob& job) {
  switch (job.kind) {
    case QueuedJobKind::LlmWithTools:
      processQueuedLlm(job);
      break;
    case QueuedJobKind::SpeechToText:
      processQueuedSpeechToText(job);
      break;
    case QueuedJobKind::TextToSpeech:
      processQueuedTextToSpeech(job);
      break;
  }
}

void AiProviderKit::processQueuedLlm(QueuedJob& job) {
  auto* request = static_cast<const AiProviderKitLlmRequest*>(job.request);
  auto* result = static_cast<AiProviderKitLlmResult*>(job.result);
  if (request == nullptr || result == nullptr) {
    if (job.done != nullptr) {
      xSemaphoreGive(job.done);
    }
    return;
  }

  *result = AiProviderKitLlmResult{};
  const auto savedDefaults = defaults_;
  const auto savedProfile = getProfile(request->providerKind);

  defaults_.llmMaxTokens = request->maxTokens;
  defaults_.timeoutMs = request->timeoutMs;
  defaults_.llmTemperature = request->temperature;
  defaults_.llmSystemPrompt = request->systemPrompt;
  defaults_.toolChoice = request->toolChoice;
  configureQueuedBodySpool(request->spoolFilesystem,
                           request->spoolPath,
                           request->spoolRemoveAfterSend,
                           request->spoolStreamChunkBytes,
                           request->spoolThresholdBytes);

  setConversationEnabled(true);
  openConversation(request->sessionId, millis());
  setLlmModel(request->providerKind, request->model);
  if (request->hasBaseUrl) {
    setBaseUrl(request->providerKind, request->baseUrl);
  }

  AiToolHttpLoopOptions options;
  options.continueOnToolError = request->continueOnToolError;
  options.bootstrapDiscoveryQuery = request->bootstrapToolQuery;
  if (request->preferredToolChoice.length() > 0) {
    defaults_.toolChoice = request->preferredToolChoice;
  }

  String invokeError;
  for (uint8_t attempt = 0; attempt <= request->emptyResponseRetries; ++attempt) {
    result->ok = llmWithTools(request->providerKind,
                              request->prompt,
                              true,
                              result->response,
                              invokeError,
                              options);
    if (result->ok) {
      break;
    }
    if (attempt < request->emptyResponseRetries && request->emptyResponseRetryDelayMs > 0U) {
      vTaskDelay(pdMS_TO_TICKS(request->emptyResponseRetryDelayMs));
    }
  }

  result->errorMessage = invokeError.length() > 0 ? invokeError : result->response.errorMessage;

  defaults_ = savedDefaults;
  setBaseUrl(request->providerKind, savedProfile.baseUrl);
  setApiKey(request->providerKind, savedProfile.apiKey);
  setLlmModel(request->providerKind, savedProfile.llmModel);

  if (job.done != nullptr) {
    xSemaphoreGive(job.done);
  }
}

void AiProviderKit::processQueuedSpeechToText(QueuedJob& job) {
  auto* request = static_cast<const AiProviderKitSpeechToTextRequest*>(job.request);
  auto* result = static_cast<AiProviderKitSpeechToTextResult*>(job.result);
  if (request == nullptr || result == nullptr) {
    if (job.done != nullptr) {
      xSemaphoreGive(job.done);
    }
    return;
  }

  *result = AiProviderKitSpeechToTextResult{};
  const auto savedDefaults = defaults_;
  const auto savedProfile = getProfile(request->providerKind);

  defaults_.timeoutMs = request->timeoutMs;
  configureQueuedBodySpool(request->spoolFilesystem,
                           request->spoolPath,
                           request->spoolRemoveAfterSend,
                           request->spoolStreamChunkBytes,
                           request->spoolThresholdBytes);
  if (request->hasBaseUrl) {
    setBaseUrl(request->providerKind, request->baseUrl);
  }
  if (request->model.length() > 0) {
    setSttModel(request->providerKind, request->model);
  }

  result->ok = sttBase64(request->providerKind,
                         request->audioBase64,
                         request->mimeType,
                         result->response,
                         result->errorMessage);
  if (result->errorMessage.length() == 0) {
    result->errorMessage = result->response.errorMessage;
  }

  defaults_ = savedDefaults;
  setBaseUrl(request->providerKind, savedProfile.baseUrl);
  setApiKey(request->providerKind, savedProfile.apiKey);
  setSttModel(request->providerKind, savedProfile.sttModel);

  if (job.done != nullptr) {
    xSemaphoreGive(job.done);
  }
}

void AiProviderKit::processQueuedTextToSpeech(QueuedJob& job) {
  auto* request = static_cast<const AiProviderKitTextToSpeechRequest*>(job.request);
  auto* result = static_cast<AiProviderKitTextToSpeechResult*>(job.result);
  if (request == nullptr || result == nullptr) {
    if (job.done != nullptr) {
      xSemaphoreGive(job.done);
    }
    return;
  }

  *result = AiProviderKitTextToSpeechResult{};
  const auto savedDefaults = defaults_;
  const auto savedProfile = getProfile(request->providerKind);

  defaults_.timeoutMs = request->timeoutMs;
  configureQueuedBodySpool(request->spoolFilesystem,
                           request->spoolPath,
                           request->spoolRemoveAfterSend,
                           request->spoolStreamChunkBytes,
                           request->spoolThresholdBytes);
  if (request->hasBaseUrl) {
    setBaseUrl(request->providerKind, request->baseUrl);
  }
  if (request->model.length() > 0) {
    setTtsModel(request->providerKind, request->model);
  }

  result->ok = tts(request->providerKind, request->text, result->response, result->errorMessage);
  if (result->errorMessage.length() == 0) {
    result->errorMessage = result->response.errorMessage;
  }

  defaults_ = savedDefaults;
  setBaseUrl(request->providerKind, savedProfile.baseUrl);
  setApiKey(request->providerKind, savedProfile.apiKey);
  setTtsModel(request->providerKind, savedProfile.ttsModel);

  if (job.done != nullptr) {
    xSemaphoreGive(job.done);
  }
}

bool AiProviderKit::realtimeStartWithTools(ProviderKind kind,
                                           const AiRealtimeSessionConfig& sessionConfig,
                                           const AiSessionClientCallbacks& callbacks,
                                           String& outSessionId,
                                           String& outErrorMessage) {
  begin();

  const ProviderProfile& profile = profileConst(kind);
  if (profile.realtimeModel.length() == 0) {
    outErrorMessage = "missing_realtime_model";
    return false;
  }

  AiInvokeRequest* req = allocateLargeObject<AiInvokeRequest>();
  if (req == nullptr) {
    outErrorMessage = "invoke_request_alloc_failed";
    return false;
  }
  req->baseUrl = profile.baseUrl;
  req->apiKey = profile.apiKey;
  req->httpReferer = profile.httpReferer;
  req->appTitle = profile.appTitle;
  req->model = profile.realtimeModel;
  req->timeoutMs = defaults_.timeoutMs;
  req->preferPsrAm = defaults_.preferPsrAm;
  req->enableToolCalls = true;
  req->toolChoice = defaults_.toolChoice;

  const bool ok = toolFacade_->startRealtimeSessionWithTools(
      kind, *req, sessionConfig, callbacks, outSessionId, outErrorMessage);
  releaseLargeObject(req);
  return ok;
}

bool AiProviderKit::realtimeClose(const String& sessionId,
                                  const String& reason,
                                  String& outErrorMessage) {
  begin();
  return toolFacade_->closeRealtimeSession(sessionId, reason, outErrorMessage);
}

bool AiProviderKit::realtimeSendText(const String& sessionId,
                                     const String& text,
                                     String& outErrorMessage) {
  begin();
  return toolFacade_->sendText(sessionId, text, outErrorMessage);
}

bool AiProviderKit::realtimeSendAudioBytesSmart(const String& sessionId,
                                                const uint8_t* audioBytes,
                                                size_t audioBytesLength,
                                                const String& mimeType,
                                                String& outErrorMessage) {
  begin();
  return sessionClient_->sendAudioBytesSmart(sessionId, audioBytes, audioBytesLength, mimeType, outErrorMessage);
}

bool AiProviderKit::realtimeSendToolResult(const String& sessionId,
                                           const String& toolCallId,
                                           const String& toolName,
                                           const String& toolResultJson,
                                           String& outErrorMessage) {
  begin();
  return sessionClient_->sendToolResult(sessionId, toolCallId, toolName, toolResultJson, outErrorMessage);
}

bool AiProviderKit::isRealtimeOpen(const String& sessionId) const {
  if (sessionClient_ == nullptr) {
    return false;
  }
  return sessionClient_->isSessionOpen(sessionId);
}

void AiProviderKit::poll() {
  begin();
  if (httpClient_ != nullptr) {
    httpClient_->poll();
  }
  if (audioClient_ != nullptr) {
    audioClient_->poll();
  }
  if (toolFacade_ != nullptr) {
    toolFacade_->pollRealtime();
  } else if (sessionClient_ != nullptr) {
    sessionClient_->poll();
  }
}

bool AiProviderKit::isBusy() const {
  bool busy = false;
  if (httpClient_ != nullptr) {
    busy = busy || httpClient_->isBusy();
  }
  if (audioClient_ != nullptr) {
    busy = busy || audioClient_->isBusy();
  }
  return busy;
}

void AiProviderKit::cancel() {
  if (httpClient_ != nullptr) {
    httpClient_->cancel();
  }
  if (audioClient_ != nullptr) {
    audioClient_->cancel();
  }
}

}  // namespace ai::provider
