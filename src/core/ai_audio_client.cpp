#include "core/ai_audio_client.h"

#include "core/ai_audio_provider.h"

namespace ai::provider {

AiAudioClient::AiAudioClient(IAiTransport& transport, const AiProviderRegistry& registry)
    : transport_(transport), registry_(registry) {}

bool AiAudioClient::transcribe(ProviderKind kind,
                               const AiSpeechToTextRequest& request,
                               AiSpeechToTextResponse& outResponse) const {
  return transcribe(registry_.findAudio(kind), request, outResponse);
}

bool AiAudioClient::transcribe(const String& providerId,
                               const AiSpeechToTextRequest& request,
                               AiSpeechToTextResponse& outResponse) const {
  return transcribe(registry_.findAudioById(providerId), request, outResponse);
}

bool AiAudioClient::synthesize(ProviderKind kind,
                               const AiTextToSpeechRequest& request,
                               AiTextToSpeechResponse& outResponse) const {
  return synthesize(registry_.findAudio(kind), request, outResponse);
}

bool AiAudioClient::synthesize(const String& providerId,
                               const AiTextToSpeechRequest& request,
                               AiTextToSpeechResponse& outResponse) const {
  return synthesize(registry_.findAudioById(providerId), request, outResponse);
}

bool AiAudioClient::transcribe(const IAiAudioProviderAdapter* adapter,
                               const AiSpeechToTextRequest& request,
                               AiSpeechToTextResponse& outResponse) const {
  if (adapter == nullptr) {
    outResponse.ok = false;
    outResponse.errorCode = "provider_not_found";
    outResponse.errorMessage = "Provider adapter is not registered";
    return false;
  }

  if (!adapter->supportsSpeechToText()) {
    outResponse.ok = false;
    outResponse.errorCode = "provider_not_supported";
    outResponse.errorMessage = "Speech-to-text is not supported by this provider";
    return false;
  }

  AiHttpRequest httpRequest;
  String buildError;
  if (!adapter->buildSpeechToTextRequest(request, httpRequest, buildError)) {
    outResponse.ok = false;
    outResponse.errorCode = "request_build_failed";
    outResponse.errorMessage = buildError;
    return false;
  }

  AiHttpResponse httpResponse;
  if (!transport_.execute(httpRequest, httpResponse)) {
    outResponse.ok = false;
    outResponse.errorCode = "transport_error";
    outResponse.errorMessage = httpResponse.errorMessage;
    outResponse.statusCode =
        static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
    outResponse.rawResponse = httpResponse.body;
    return false;
  }

  const bool parseOk = adapter->parseSpeechToTextResponse(httpResponse, outResponse);
  if (!parseOk && outResponse.errorCode.length() == 0) {
    outResponse.errorCode = "response_parse_failed";
  }
  if (outResponse.rawResponse.length() == 0) {
    outResponse.rawResponse = httpResponse.body;
  }
  return parseOk;
}

bool AiAudioClient::synthesize(const IAiAudioProviderAdapter* adapter,
                               const AiTextToSpeechRequest& request,
                               AiTextToSpeechResponse& outResponse) const {
  if (adapter == nullptr) {
    outResponse.ok = false;
    outResponse.errorCode = "provider_not_found";
    outResponse.errorMessage = "Provider adapter is not registered";
    return false;
  }

  if (!adapter->supportsTextToSpeech()) {
    outResponse.ok = false;
    outResponse.errorCode = "provider_not_supported";
    outResponse.errorMessage = "Text-to-speech is not supported by this provider";
    return false;
  }

  AiHttpRequest httpRequest;
  String buildError;
  if (!adapter->buildTextToSpeechRequest(request, httpRequest, buildError)) {
    outResponse.ok = false;
    outResponse.errorCode = "request_build_failed";
    outResponse.errorMessage = buildError;
    return false;
  }

  AiHttpResponse httpResponse;
  if (!transport_.execute(httpRequest, httpResponse)) {
    outResponse.ok = false;
    outResponse.errorCode = "transport_error";
    outResponse.errorMessage = httpResponse.errorMessage;
    outResponse.statusCode =
        static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
    outResponse.rawResponse = httpResponse.body;
    return false;
  }

  const bool parseOk = adapter->parseTextToSpeechResponse(httpResponse, outResponse);
  if (!parseOk && outResponse.errorCode.length() == 0) {
    outResponse.errorCode = "response_parse_failed";
  }
  if (outResponse.rawResponse.length() == 0) {
    outResponse.rawResponse = httpResponse.body;
  }
  return parseOk;
}

bool AiAudioClient::transcribeAsync(ProviderKind kind,
                                    const AiSpeechToTextRequest& request,
                                    AiSpeechToTextDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  return transcribeAsync(
      registry_.findAudio(kind), request, doneCallback, userContext, outErrorMessage);
}

bool AiAudioClient::transcribeAsync(const String& providerId,
                                    const AiSpeechToTextRequest& request,
                                    AiSpeechToTextDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  return transcribeAsync(
      registry_.findAudioById(providerId), request, doneCallback, userContext, outErrorMessage);
}

bool AiAudioClient::synthesizeAsync(ProviderKind kind,
                                    const AiTextToSpeechRequest& request,
                                    AiTextToSpeechDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  return synthesizeAsync(
      registry_.findAudio(kind), request, doneCallback, userContext, outErrorMessage);
}

bool AiAudioClient::synthesizeAsync(const String& providerId,
                                    const AiTextToSpeechRequest& request,
                                    AiTextToSpeechDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  return synthesizeAsync(
      registry_.findAudioById(providerId), request, doneCallback, userContext, outErrorMessage);
}

bool AiAudioClient::transcribeAsync(const IAiAudioProviderAdapter* adapter,
                                    const AiSpeechToTextRequest& request,
                                    AiSpeechToTextDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  if (pending_.active || transport_.isBusy()) {
    outErrorMessage = "Transport is busy";
    return false;
  }

  if (adapter == nullptr) {
    outErrorMessage = "Provider adapter is not registered";
    return false;
  }

  if (!adapter->supportsSpeechToText()) {
    outErrorMessage = "Speech-to-text is not supported by this provider";
    return false;
  }

  AiHttpRequest httpRequest;
  if (!adapter->buildSpeechToTextRequest(request, httpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_build_failed";
    }
    return false;
  }

  httpRequest.nonBlockingPreferred = true;
  httpRequest.preferPsrAm = request.preferPsrAm;
  httpRequest.timeoutMs = request.timeoutMs;

  AiTransportCallbacks callbacks;
  callbacks.onChunk = &AiAudioClient::onTransportChunkStatic;
  callbacks.onDone = &AiAudioClient::onTransportDoneStatic;
  callbacks.userContext = this;

  pending_.operation = PendingOperation::SpeechToText;
  pending_.active = true;
  pending_.adapter = adapter;
  pending_.sttRequest = request;
  pending_.sttDoneCallback = doneCallback;
  pending_.sttDoneUserContext = userContext;

  const AiAsyncSubmitResult submitResult =
      transport_.executeAsync(httpRequest, callbacks, outErrorMessage);
  if (submitResult == AiAsyncSubmitResult::Accepted) {
    return true;
  }

  if (submitResult == AiAsyncSubmitResult::Busy && outErrorMessage.length() == 0) {
    outErrorMessage = "Transport is busy";
  }
  if (submitResult == AiAsyncSubmitResult::Failed && outErrorMessage.length() == 0) {
    outErrorMessage = "Async submit failed";
  }

  resetPending();
  return false;
}

bool AiAudioClient::synthesizeAsync(const IAiAudioProviderAdapter* adapter,
                                    const AiTextToSpeechRequest& request,
                                    AiTextToSpeechDoneCallback doneCallback,
                                    void* userContext,
                                    String& outErrorMessage) {
  if (pending_.active || transport_.isBusy()) {
    outErrorMessage = "Transport is busy";
    return false;
  }

  if (adapter == nullptr) {
    outErrorMessage = "Provider adapter is not registered";
    return false;
  }

  if (!adapter->supportsTextToSpeech()) {
    outErrorMessage = "Text-to-speech is not supported by this provider";
    return false;
  }

  AiHttpRequest httpRequest;
  if (!adapter->buildTextToSpeechRequest(request, httpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_build_failed";
    }
    return false;
  }

  httpRequest.nonBlockingPreferred = true;
  httpRequest.preferPsrAm = request.preferPsrAm;
  httpRequest.timeoutMs = request.timeoutMs;

  AiTransportCallbacks callbacks;
  callbacks.onChunk = &AiAudioClient::onTransportChunkStatic;
  callbacks.onDone = &AiAudioClient::onTransportDoneStatic;
  callbacks.userContext = this;

  pending_.operation = PendingOperation::TextToSpeech;
  pending_.active = true;
  pending_.adapter = adapter;
  pending_.ttsRequest = request;
  pending_.ttsDoneCallback = doneCallback;
  pending_.ttsDoneUserContext = userContext;

  const AiAsyncSubmitResult submitResult =
      transport_.executeAsync(httpRequest, callbacks, outErrorMessage);
  if (submitResult == AiAsyncSubmitResult::Accepted) {
    return true;
  }

  if (submitResult == AiAsyncSubmitResult::Busy && outErrorMessage.length() == 0) {
    outErrorMessage = "Transport is busy";
  }
  if (submitResult == AiAsyncSubmitResult::Failed && outErrorMessage.length() == 0) {
    outErrorMessage = "Async submit failed";
  }

  resetPending();
  return false;
}

void AiAudioClient::poll() {
  transport_.poll();
}

bool AiAudioClient::isBusy() const {
  return pending_.active || transport_.isBusy();
}

void AiAudioClient::cancel() {
  transport_.cancel();
  resetPending();
}

void AiAudioClient::onTransportChunkStatic(const AiStreamChunk& chunk, void* userContext) {
  auto* self = static_cast<AiAudioClient*>(userContext);
  if (self != nullptr) {
    self->onTransportChunk(chunk);
  }
}

void AiAudioClient::onTransportDoneStatic(const AiHttpResponse& response, void* userContext) {
  auto* self = static_cast<AiAudioClient*>(userContext);
  if (self != nullptr) {
    self->onTransportDone(response);
  }
}

void AiAudioClient::onTransportChunk(const AiStreamChunk& chunk) {
  if (!pending_.active) {
    return;
  }

  if (pending_.operation == PendingOperation::SpeechToText &&
      pending_.sttRequest.streamCallback != nullptr) {
    AiSpeechToTextChunk sttChunk;
    sttChunk.textDelta = chunk.textDelta;
    sttChunk.rawChunk = chunk.rawChunk;
    sttChunk.done = chunk.done;
    sttChunk.doneReason = chunk.doneReason;
    pending_.sttRequest.streamCallback(sttChunk, pending_.sttRequest.streamUserContext);
    return;
  }

  if (pending_.operation == PendingOperation::TextToSpeech &&
      pending_.ttsRequest.streamCallback != nullptr) {
    AiTextToSpeechChunk ttsChunk;
    ttsChunk.audioBase64Delta = chunk.textDelta;
    ttsChunk.audioMimeType = pending_.ttsRequest.outputFormat;
    ttsChunk.rawChunk = chunk.rawChunk;
    ttsChunk.done = chunk.done;
    ttsChunk.doneReason = chunk.doneReason;
    pending_.ttsRequest.streamCallback(ttsChunk, pending_.ttsRequest.streamUserContext);
  }
}

void AiAudioClient::onTransportDone(const AiHttpResponse& response) {
  if (!pending_.active || pending_.adapter == nullptr) {
    resetPending();
    return;
  }

  const IAiAudioProviderAdapter* adapter = pending_.adapter;
  const PendingOperation operation = pending_.operation;

  AiSpeechToTextDoneCallback sttDone = pending_.sttDoneCallback;
  void* sttUser = pending_.sttDoneUserContext;

  AiTextToSpeechDoneCallback ttsDone = pending_.ttsDoneCallback;
  void* ttsUser = pending_.ttsDoneUserContext;

  resetPending();

  if (operation == PendingOperation::SpeechToText) {
    AiSpeechToTextResponse parsed;
    bool parseOk = adapter->parseSpeechToTextResponse(response, parsed);
    if (!parseOk && parsed.errorCode.length() == 0) {
      parsed.errorCode = "response_parse_failed";
    }
    if (parsed.rawResponse.length() == 0) {
      parsed.rawResponse = response.body;
    }
    if (parsed.statusCode == 0 && response.statusCode >= 0) {
      parsed.statusCode = static_cast<uint16_t>(response.statusCode);
    }
    if (sttDone != nullptr) {
      sttDone(parsed, sttUser);
    }
    return;
  }

  if (operation == PendingOperation::TextToSpeech) {
    AiTextToSpeechResponse parsed;
    bool parseOk = adapter->parseTextToSpeechResponse(response, parsed);
    if (!parseOk && parsed.errorCode.length() == 0) {
      parsed.errorCode = "response_parse_failed";
    }
    if (parsed.rawResponse.length() == 0) {
      parsed.rawResponse = response.body;
    }
    if (parsed.statusCode == 0 && response.statusCode >= 0) {
      parsed.statusCode = static_cast<uint16_t>(response.statusCode);
    }
    if (ttsDone != nullptr) {
      ttsDone(parsed, ttsUser);
    }
  }
}

void AiAudioClient::resetPending() {
  pending_ = PendingContext{};
}

}  // namespace ai::provider
