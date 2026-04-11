#include "core/ai_provider_client.h"

namespace ai::provider {

AiProviderClient::AiProviderClient(IAiTransport& transport, const AiProviderRegistry& registry)
    : transport_(transport), registry_(registry) {}

bool AiProviderClient::invoke(ProviderKind kind,
                              const AiInvokeRequest& request,
                              AiInvokeResponse& outResponse) const {
  return invoke(registry_.find(kind), request, outResponse);
}

bool AiProviderClient::invoke(const String& providerId,
                              const AiInvokeRequest& request,
                              AiInvokeResponse& outResponse) const {
  return invoke(registry_.findById(providerId), request, outResponse);
}

bool AiProviderClient::invoke(const IAiProviderAdapter* adapter,
                              const AiInvokeRequest& request,
                              AiInvokeResponse& outResponse) const {
  if (adapter == nullptr) {
    outResponse.ok = false;
    outResponse.errorCode = "provider_not_found";
    outResponse.errorMessage = "Provider adapter is not registered";
    return false;
  }

  AiHttpRequest httpRequest;
  String buildError;
  if (!adapter->buildHttpRequest(request, httpRequest, buildError)) {
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
    outResponse.statusCode = static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
    outResponse.rawResponse = httpResponse.body;
    return false;
  }

  const bool parseOk = adapter->parseHttpResponse(httpResponse, outResponse);
  if (!parseOk && outResponse.errorCode.length() == 0) {
    outResponse.errorCode = "response_parse_failed";
  }
  if (outResponse.rawResponse.length() == 0) {
    outResponse.rawResponse = httpResponse.body;
  }
  return parseOk;
}

bool AiProviderClient::invokeAsync(ProviderKind kind,
                                   const AiInvokeRequest& request,
                                   AiInvokeDoneCallback doneCallback,
                                   void* userContext,
                                   String& outErrorMessage) {
  return invokeAsync(registry_.find(kind), request, doneCallback, userContext, outErrorMessage);
}

bool AiProviderClient::invokeAsync(const String& providerId,
                                   const AiInvokeRequest& request,
                                   AiInvokeDoneCallback doneCallback,
                                   void* userContext,
                                   String& outErrorMessage) {
  return invokeAsync(
      registry_.findById(providerId), request, doneCallback, userContext, outErrorMessage);
}

bool AiProviderClient::invokeAsync(const IAiProviderAdapter* adapter,
                                   const AiInvokeRequest& request,
                                   AiInvokeDoneCallback doneCallback,
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

  AiHttpRequest httpRequest;
  String buildError;
  if (!adapter->buildHttpRequest(request, httpRequest, buildError)) {
    outErrorMessage = buildError.length() > 0 ? buildError : String("request_build_failed");
    return false;
  }

  httpRequest.nonBlockingPreferred = true;
  httpRequest.preferPsrAm = request.preferPsrAm;
  httpRequest.timeoutMs = request.timeoutMs;

  AiTransportCallbacks callbacks;
  callbacks.onChunk = &AiProviderClient::onTransportChunkStatic;
  callbacks.onDone = &AiProviderClient::onTransportDoneStatic;
  callbacks.userContext = this;

  pending_.active = true;
  pending_.adapter = adapter;
  pending_.doneCallback = doneCallback;
  pending_.doneUserContext = userContext;
  pending_.streamCallback = request.streamCallback;
  pending_.streamUserContext = request.streamUserContext;
  pending_.requestStream = request.stream;
  pending_.streamSawText = false;
  pending_.streamText = "";
  pending_.streamDoneReason = "";

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

void AiProviderClient::poll() {
  transport_.poll();
}

bool AiProviderClient::isBusy() const {
  return pending_.active || transport_.isBusy();
}

void AiProviderClient::cancel() {
  transport_.cancel();
  resetPending();
}

void AiProviderClient::onTransportChunkStatic(const AiStreamChunk& chunk, void* userContext) {
  auto* self = static_cast<AiProviderClient*>(userContext);
  if (self != nullptr) {
    self->onTransportChunk(chunk);
  }
}

void AiProviderClient::onTransportDoneStatic(const AiHttpResponse& response, void* userContext) {
  auto* self = static_cast<AiProviderClient*>(userContext);
  if (self != nullptr) {
    self->onTransportDone(response);
  }
}

void AiProviderClient::onTransportChunk(const AiStreamChunk& chunk) {
  if (!pending_.active) {
    return;
  }

  if (chunk.textDelta.length() > 0) {
    pending_.streamSawText = true;
    pending_.streamText += chunk.textDelta;
  }

  if (chunk.done && chunk.doneReason.length() > 0) {
    pending_.streamDoneReason = chunk.doneReason;
  }

  if (pending_.streamCallback != nullptr) {
    pending_.streamCallback(chunk, pending_.streamUserContext);
  }
}

void AiProviderClient::onTransportDone(const AiHttpResponse& response) {
  AiInvokeResponse parsed;
  bool parseOk = false;

  if (!pending_.active || pending_.adapter == nullptr) {
    parsed.ok = false;
    parsed.errorCode = "internal_state";
    parsed.errorMessage = "No pending invoke context";
  } else {
    parseOk = pending_.adapter->parseHttpResponse(response, parsed);
    if (!parseOk && parsed.errorCode.length() == 0) {
      parsed.errorCode = "response_parse_failed";
    }
    if (parsed.rawResponse.length() == 0) {
      parsed.rawResponse = response.body;
    }
    if (parsed.statusCode == 0 && response.statusCode >= 0) {
      parsed.statusCode = static_cast<uint16_t>(response.statusCode);
    }

    if (!parseOk && pending_.requestStream && pending_.streamSawText) {
      parsed.ok = response.statusCode >= 200 && response.statusCode < 300;
      parsed.text = pending_.streamText;
      parsed.finishReason = pending_.streamDoneReason.length() > 0
                                ? pending_.streamDoneReason
                                : String("stream_end");
      if (parsed.ok) {
        parsed.errorCode = "";
        parsed.errorMessage = "";
      }
      parseOk = parsed.ok;
    }

    if (parseOk && parsed.text.length() == 0 && pending_.streamSawText) {
      parsed.text = pending_.streamText;
      if (parsed.finishReason.length() == 0 && pending_.streamDoneReason.length() > 0) {
        parsed.finishReason = pending_.streamDoneReason;
      }
    }
  }

  AiInvokeDoneCallback doneCallback = pending_.doneCallback;
  void* doneUserContext = pending_.doneUserContext;
  resetPending();

  if (doneCallback != nullptr) {
    doneCallback(parsed, doneUserContext);
  }
}

void AiProviderClient::resetPending() {
  pending_.active = false;
  pending_.adapter = nullptr;
  pending_.doneCallback = nullptr;
  pending_.doneUserContext = nullptr;
  pending_.streamCallback = nullptr;
  pending_.streamUserContext = nullptr;
  pending_.requestStream = false;
  pending_.streamSawText = false;
  pending_.streamText = "";
  pending_.streamDoneReason = "";
}

}  // namespace ai::provider
