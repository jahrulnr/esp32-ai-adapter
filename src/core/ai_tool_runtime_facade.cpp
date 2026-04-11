#include "core/ai_tool_runtime_facade.h"

namespace ai::provider {

AiToolRuntimeFacade::AiToolRuntimeFacade(AiProviderClient& httpClient,
                                         AiSessionClient& sessionClient,
                                         const AiToolRuntimeRegistry& toolRegistry)
    : httpClient_(httpClient),
      sessionClient_(sessionClient),
      toolRegistry_(toolRegistry),
      executor_(toolRegistry) {}

bool AiToolRuntimeFacade::invokeHttpWithTools(ProviderKind provider,
                                              const AiInvokeRequest& request,
                                              AiInvokeResponse& outResponse,
                                              String& outErrorMessage,
                                              const AiToolHttpLoopOptions& options) const {
  return executor_.invokeHttpWithTools(
      httpClient_, provider, request, outResponse, outErrorMessage, options);
}

bool AiToolRuntimeFacade::startRealtimeSessionWithTools(const String& providerId,
                                                        const AiInvokeRequest& request,
                                                        const AiRealtimeSessionConfig& sessionConfig,
                                                        const AiSessionClientCallbacks& callbacks,
                                                        String& outSessionId,
                                                        String& outErrorMessage) {
  AiInvokeRequest working = request;
  if (working.enableToolCalls && working.toolCount == 0 && toolRegistry_.size() > 0) {
    if (!toolRegistry_.appendToolDefinitionsToRequest(working, outErrorMessage)) {
      return false;
    }
  }

  AiSessionClientCallbacks wrapped;
  wrapped.onState = &AiToolRuntimeFacade::onStateStatic;
  wrapped.onEvent = &AiToolRuntimeFacade::onEventStatic;
  wrapped.onDone = &AiToolRuntimeFacade::onDoneStatic;
  wrapped.userContext = this;

  if (!sessionClient_.startSession(
          providerId, working, sessionConfig, wrapped, outSessionId, outErrorMessage)) {
    return false;
  }

  if (!attachBinding(outSessionId, callbacks)) {
    String closeError;
    sessionClient_.closeSession(outSessionId, "binding_failed", closeError);
    outErrorMessage = "Maximum facade realtime bindings reached";
    return false;
  }

  return true;
}

bool AiToolRuntimeFacade::startRealtimeSessionWithTools(const ProviderKind provider,
                                                        const AiInvokeRequest& request,
                                                        const AiRealtimeSessionConfig& sessionConfig,
                                                        const AiSessionClientCallbacks& callbacks,
                                                        String& outSessionId,
                                                        String& outErrorMessage) {
  AiInvokeRequest working = request;
  if (working.enableToolCalls && working.toolCount == 0 && toolRegistry_.size() > 0) {
    if (!toolRegistry_.appendToolDefinitionsToRequest(working, outErrorMessage)) {
      return false;
    }
  }

  AiSessionClientCallbacks wrapped;
  wrapped.onState = &AiToolRuntimeFacade::onStateStatic;
  wrapped.onEvent = &AiToolRuntimeFacade::onEventStatic;
  wrapped.onDone = &AiToolRuntimeFacade::onDoneStatic;
  wrapped.userContext = this;

  if (!sessionClient_.startSession(
          provider, working, sessionConfig, wrapped, outSessionId, outErrorMessage)) {
    return false;
  }

  if (!attachBinding(outSessionId, callbacks)) {
    String closeError;
    sessionClient_.closeSession(outSessionId, "binding_failed", closeError);
    outErrorMessage = "Maximum facade realtime bindings reached";
    return false;
  }

  return true;
}

bool AiToolRuntimeFacade::closeRealtimeSession(const String& sessionId,
                                               const String& reason,
                                               String& outErrorMessage) {
  const bool ok = sessionClient_.closeSession(sessionId, reason, outErrorMessage);
  clearBinding(sessionId);
  return ok;
}

bool AiToolRuntimeFacade::sendText(const String& sessionId,
                                   const String& text,
                                   String& outErrorMessage) {
  return sessionClient_.sendText(sessionId, text, outErrorMessage);
}

bool AiToolRuntimeFacade::sendAudio(const String& sessionId,
                                    const String& audioBase64,
                                    const String& mimeType,
                                    String& outErrorMessage) {
  return sessionClient_.sendAudio(sessionId, audioBase64, mimeType, outErrorMessage);
}

bool AiToolRuntimeFacade::sendAudioBytesSmart(const String& sessionId,
                                              const uint8_t* audioBytes,
                                              const size_t audioBytesLength,
                                              const String& mimeType,
                                              String& outErrorMessage) {
  return sessionClient_.sendAudioBytesSmart(
      sessionId, audioBytes, audioBytesLength, mimeType, outErrorMessage);
}

void AiToolRuntimeFacade::pollRealtime() {
  sessionClient_.poll();
}

bool AiToolRuntimeFacade::isSessionOpen(const String& sessionId) const {
  return sessionClient_.isSessionOpen(sessionId);
}

void AiToolRuntimeFacade::onStateStatic(const String& sessionId,
                                        bool connected,
                                        const String& reason,
                                        void* userContext) {
  auto* self = static_cast<AiToolRuntimeFacade*>(userContext);
  if (self != nullptr) {
    self->onState(sessionId, connected, reason);
  }
}

void AiToolRuntimeFacade::onEventStatic(const String& sessionId,
                                        const AiRealtimeParsedEvent& event,
                                        void* userContext) {
  auto* self = static_cast<AiToolRuntimeFacade*>(userContext);
  if (self != nullptr) {
    self->onEvent(sessionId, event);
  }
}

void AiToolRuntimeFacade::onDoneStatic(const String& sessionId,
                                       const AiInvokeResponse& response,
                                       void* userContext) {
  auto* self = static_cast<AiToolRuntimeFacade*>(userContext);
  if (self != nullptr) {
    self->onDone(sessionId, response);
  }
}

void AiToolRuntimeFacade::onState(const String& sessionId,
                                  bool connected,
                                  const String& reason) {
  const SessionBinding* binding = findBinding(sessionId);
  if (binding != nullptr && binding->callbacks.onState != nullptr) {
    binding->callbacks.onState(sessionId, connected, reason, binding->callbacks.userContext);
  }

  if (!connected) {
    clearBinding(sessionId);
  }
}

void AiToolRuntimeFacade::onEvent(const String& sessionId, const AiRealtimeParsedEvent& event) {
  const SessionBinding* binding = findBinding(sessionId);

  if (event.hasToolCall) {
    String toolError;
    if (!executor_.onRealtimeToolCall(sessionId, event, sessionClient_, toolError)) {
      if (binding != nullptr && binding->callbacks.onEvent != nullptr) {
        AiRealtimeParsedEvent errEvent;
        errEvent.kind = AiRealtimeEventKind::Error;
        errEvent.providerEventType = "tool.runtime.error";
        errEvent.errorCode = "tool_runtime_error";
        errEvent.errorMessage = toolError;
        binding->callbacks.onEvent(sessionId, errEvent, binding->callbacks.userContext);
      }
    }
  }

  if (binding != nullptr && binding->callbacks.onEvent != nullptr) {
    binding->callbacks.onEvent(sessionId, event, binding->callbacks.userContext);
  }
}

void AiToolRuntimeFacade::onDone(const String& sessionId, const AiInvokeResponse& response) {
  const SessionBinding* binding = findBinding(sessionId);
  if (binding != nullptr && binding->callbacks.onDone != nullptr) {
    binding->callbacks.onDone(sessionId, response, binding->callbacks.userContext);
  }
}

bool AiToolRuntimeFacade::attachBinding(const String& sessionId,
                                        const AiSessionClientCallbacks& callbacks) {
  SessionBinding* existing = findBinding(sessionId);
  if (existing != nullptr) {
    existing->callbacks = callbacks;
    return true;
  }

  for (SessionBinding& binding : bindings_) {
    if (binding.used) {
      continue;
    }
    binding.used = true;
    binding.sessionId = sessionId;
    binding.callbacks = callbacks;
    return true;
  }

  return false;
}

AiToolRuntimeFacade::SessionBinding* AiToolRuntimeFacade::findBinding(const String& sessionId) {
  for (SessionBinding& binding : bindings_) {
    if (binding.used && binding.sessionId == sessionId) {
      return &binding;
    }
  }
  return nullptr;
}

const AiToolRuntimeFacade::SessionBinding* AiToolRuntimeFacade::findBinding(
    const String& sessionId) const {
  for (const SessionBinding& binding : bindings_) {
    if (binding.used && binding.sessionId == sessionId) {
      return &binding;
    }
  }
  return nullptr;
}

void AiToolRuntimeFacade::clearBinding(const String& sessionId) {
  SessionBinding* binding = findBinding(sessionId);
  if (binding == nullptr) {
    return;
  }
  *binding = SessionBinding{};
}

}  // namespace ai::provider
