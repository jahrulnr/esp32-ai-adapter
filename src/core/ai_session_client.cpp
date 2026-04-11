#include "core/ai_session_client.h"

#include <base64.h>

#include <cstdlib>

#if defined(ESP32)
#include <Esp.h>
#endif

namespace ai::provider {

namespace {

size_t currentFreeHeapBytes() {
#if defined(ESP32)
  return static_cast<size_t>(ESP.getFreeHeap());
#else
  return 256 * 1024;
#endif
}

size_t encodedLengthForBase64(size_t rawLength) {
  return ((rawLength + 2) / 3) * 4;
}

size_t pickChunkSize(size_t requested,
                     size_t minChunk,
                     size_t freeHeap,
                     size_t safetyHeadroom) {
  size_t chunk = requested;
  if (chunk < minChunk) {
    chunk = minChunk;
  }

  while (chunk > minChunk) {
    const size_t encoded = encodedLengthForBase64(chunk);
    if (freeHeap > encoded + safetyHeadroom) {
      break;
    }
    chunk /= 2;
  }

  if (chunk < minChunk) {
    chunk = minChunk;
  }
  return chunk;
}

}  // namespace

AiSessionClient::AiSessionClient(IAiSessionTransport& transport, const AiProviderRegistry& registry)
    : transport_(transport), registry_(registry) {}

bool AiSessionClient::startSession(ProviderKind kind,
                                   const AiInvokeRequest& request,
                                   const AiRealtimeSessionConfig& sessionConfig,
                                   const AiSessionClientCallbacks& callbacks,
                                   String& outSessionId,
                                   String& outErrorMessage) {
  return startSession(
      registry_.find(kind), request, sessionConfig, callbacks, outSessionId, outErrorMessage);
}

bool AiSessionClient::startSession(const String& providerId,
                                   const AiInvokeRequest& request,
                                   const AiRealtimeSessionConfig& sessionConfig,
                                   const AiSessionClientCallbacks& callbacks,
                                   String& outSessionId,
                                   String& outErrorMessage) {
  return startSession(registry_.findById(providerId),
                      request,
                      sessionConfig,
                      callbacks,
                      outSessionId,
                      outErrorMessage);
}

bool AiSessionClient::startSession(const IAiProviderAdapter* adapter,
                                   const AiInvokeRequest& request,
                                   const AiRealtimeSessionConfig& sessionConfig,
                                   const AiSessionClientCallbacks& callbacks,
                                   String& outSessionId,
                                   String& outErrorMessage) {
  if (adapter == nullptr) {
    outErrorMessage = "Provider adapter is not registered";
    return false;
  }

  const IAiRealtimeProviderAdapter* realtimeAdapter = adapter->asRealtimeAdapter();
  if (realtimeAdapter == nullptr || !realtimeAdapter->supportsRealtime()) {
    outErrorMessage = "Realtime session is not supported by this provider";
    return false;
  }

  ActiveSession* slot = allocateSessionSlot();
  if (slot == nullptr) {
    outErrorMessage = "Maximum active realtime sessions reached";
    return false;
  }

  slot->active = true;
  slot->adapter = realtimeAdapter;
  slot->callbacks = callbacks;
  slot->response = AiInvokeResponse{};
  slot->response.realtimeAccepted = false;
  slot->doneNotified = false;

  AiHttpRequest initRequest;
  if (!realtimeAdapter->buildRealtimeSessionRequest(
          request, sessionConfig, initRequest, outErrorMessage)) {
    releaseSessionSlot(slot);
    return false;
  }

  AiSessionCallbacks transportCallbacks;
  transportCallbacks.onOpened = &AiSessionClient::onTransportOpenedStatic;
  transportCallbacks.onMessage = &AiSessionClient::onTransportMessageStatic;
  transportCallbacks.onClosed = &AiSessionClient::onTransportClosedStatic;
  transportCallbacks.onError = &AiSessionClient::onTransportErrorStatic;
  transportCallbacks.userContext = this;

  const AiAsyncSubmitResult openResult = transport_.openSession(
      sessionConfig, initRequest, transportCallbacks, outSessionId, outErrorMessage);
  if (openResult != AiAsyncSubmitResult::Accepted) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = openResult == AiAsyncSubmitResult::Busy ? "Session transport is busy"
                                                                : "Failed to open session";
    }
    releaseSessionSlot(slot);
    return false;
  }

  slot->sessionId = outSessionId;
  return true;
}

bool AiSessionClient::sendText(const String& sessionId,
                               const String& text,
                               String& outErrorMessage) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr || session->adapter == nullptr) {
    outErrorMessage = "Session is not active";
    return false;
  }

  AiRealtimeMessage message;
  if (!session->adapter->buildRealtimeTextMessage(text, message, outErrorMessage)) {
    return false;
  }

  return transport_.sendMessage(sessionId, message, outErrorMessage);
}

bool AiSessionClient::sendAudio(const String& sessionId,
                                const String& audioBase64,
                                const String& mimeType,
                                String& outErrorMessage) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr || session->adapter == nullptr) {
    outErrorMessage = "Session is not active";
    return false;
  }

  AiRealtimeMessage message;
  if (!session->adapter->buildRealtimeAudioMessage(
          audioBase64, mimeType, message, outErrorMessage)) {
    return false;
  }

  return transport_.sendMessage(sessionId, message, outErrorMessage);
}

bool AiSessionClient::sendAudioBytesSmart(const String& sessionId,
                                          const uint8_t* audioBytes,
                                          size_t audioBytesLength,
                                          const String& mimeType,
                                          String& outErrorMessage) {
  return sendAudioBytesSmart(
      sessionId, audioBytes, audioBytesLength, mimeType, AiAudioChunkingOptions{}, outErrorMessage);
}

bool AiSessionClient::sendAudioBytesSmart(const String& sessionId,
                                          const uint8_t* audioBytes,
                                          size_t audioBytesLength,
                                          const String& mimeType,
                                          const AiAudioChunkingOptions& options,
                                          String& outErrorMessage) {
  if (audioBytes == nullptr || audioBytesLength == 0) {
    outErrorMessage = "Audio bytes are empty";
    return false;
  }

  size_t minChunk = options.minChunkBytes;
  if (minChunk == 0) {
    minChunk = 256;
  }

  const size_t freeHeap = currentFreeHeapBytes();
  size_t preferred = options.preferredChunkBytes;
  if (preferred < minChunk) {
    preferred = minChunk;
  }

  if (freeHeap < options.lowMemoryThresholdBytes) {
    preferred = minChunk;
  }

  size_t chunkSize = pickChunkSize(preferred, minChunk, freeHeap, options.safetyHeadroomBytes);
  const bool canEncodeInMemory =
      freeHeap > (encodedLengthForBase64(chunkSize) + options.safetyHeadroomBytes);

  auto sendFromBytes = [&](const uint8_t* bytes, size_t length, size_t step) -> bool {
    size_t offset = 0;
    while (offset < length) {
      size_t part = step;
      const size_t remain = length - offset;
      if (part > remain) {
        part = remain;
      }

      String audioBase64 = base64::encode(bytes + offset, part);
      if (audioBase64.length() == 0) {
        outErrorMessage = "Base64 encode failed (out of memory)";
        return false;
      }

      if (!sendAudio(sessionId, audioBase64, mimeType, outErrorMessage)) {
        return false;
      }
      offset += part;
    }
    return true;
  };

  if (canEncodeInMemory) {
    return sendFromBytes(audioBytes, audioBytesLength, chunkSize);
  }

  if (options.spillFs == nullptr) {
    outErrorMessage = "Insufficient memory for base64 chunking and no spill FS configured";
    return false;
  }

  String spillPath = options.spillFilePath;
  if (spillPath.length() == 0) {
    spillPath = "/aipk_audio_spill.bin";
  }

  File writeFile = options.spillFs->open(spillPath, FILE_WRITE);
  if (!writeFile) {
    outErrorMessage = "Failed to open spill file for writing";
    return false;
  }

  const size_t written = writeFile.write(audioBytes, audioBytesLength);
  writeFile.close();
  if (written != audioBytesLength) {
    if (options.removeSpillFileAfterSend) {
      options.spillFs->remove(spillPath);
    }
    outErrorMessage = "Failed to spill full audio bytes to storage";
    return false;
  }

  File readFile = options.spillFs->open(spillPath, FILE_READ);
  if (!readFile) {
    if (options.removeSpillFileAfterSend) {
      options.spillFs->remove(spillPath);
    }
    outErrorMessage = "Failed to reopen spill file for reading";
    return false;
  }

  size_t fsChunk = pickChunkSize(minChunk, 256, currentFreeHeapBytes(), options.safetyHeadroomBytes);
  uint8_t* buffer = static_cast<uint8_t*>(std::malloc(fsChunk));
  if (buffer == nullptr) {
    readFile.close();
    if (options.removeSpillFileAfterSend) {
      options.spillFs->remove(spillPath);
    }
    outErrorMessage = "Failed to allocate streaming buffer for spill file";
    return false;
  }

  bool ok = true;
  while (ok && readFile.available()) {
    const size_t readLen = readFile.read(buffer, fsChunk);
    if (readLen == 0) {
      break;
    }
    String audioBase64 = base64::encode(buffer, readLen);
    if (audioBase64.length() == 0) {
      outErrorMessage = "Base64 encode failed while reading spill file";
      ok = false;
      break;
    }

    if (!sendAudio(sessionId, audioBase64, mimeType, outErrorMessage)) {
      ok = false;
      break;
    }
  }

  std::free(buffer);
  readFile.close();
  if (options.removeSpillFileAfterSend) {
    options.spillFs->remove(spillPath);
  }
  return ok;
}

bool AiSessionClient::sendToolResult(const String& sessionId,
                                     const String& toolCallId,
                                     const String& toolName,
                                     const String& toolResultJson,
                                     String& outErrorMessage) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr || session->adapter == nullptr) {
    outErrorMessage = "Session is not active";
    return false;
  }

  AiRealtimeMessage message;
  if (!session->adapter->buildRealtimeToolResultMessage(
          toolCallId, toolName, toolResultJson, message, outErrorMessage)) {
    return false;
  }

  return transport_.sendMessage(sessionId, message, outErrorMessage);
}

bool AiSessionClient::closeSession(const String& sessionId,
                                   const String& reason,
                                   String& outErrorMessage) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr) {
    outErrorMessage = "Session is not active";
    return false;
  }

  const bool closed = transport_.closeSession(sessionId, reason, outErrorMessage);
  releaseSessionSlot(session);
  return closed;
}

bool AiSessionClient::isSessionOpen(const String& sessionId) const {
  const ActiveSession* session = findSession(sessionId);
  if (session == nullptr || !session->active) {
    return false;
  }
  return transport_.isSessionOpen(sessionId);
}

void AiSessionClient::poll() {
  transport_.pollSessions();
}

AiSessionClient::ActiveSession* AiSessionClient::allocateSessionSlot() {
  for (ActiveSession& session : sessions_) {
    if (!session.active) {
      return &session;
    }
  }
  return nullptr;
}

void AiSessionClient::releaseSessionSlot(ActiveSession* slot) {
  if (slot == nullptr) {
    return;
  }

  *slot = ActiveSession{};
}

AiSessionClient::ActiveSession* AiSessionClient::findSession(const String& sessionId) {
  for (ActiveSession& session : sessions_) {
    if (!session.active) {
      continue;
    }
    if (session.sessionId == sessionId) {
      return &session;
    }
  }
  return nullptr;
}

const AiSessionClient::ActiveSession* AiSessionClient::findSession(const String& sessionId) const {
  for (const ActiveSession& session : sessions_) {
    if (!session.active) {
      continue;
    }
    if (session.sessionId == sessionId) {
      return &session;
    }
  }
  return nullptr;
}

void AiSessionClient::onTransportOpenedStatic(const String& sessionId, void* userContext) {
  auto* self = static_cast<AiSessionClient*>(userContext);
  if (self != nullptr) {
    self->onTransportOpened(sessionId);
  }
}

void AiSessionClient::onTransportMessageStatic(const String& sessionId,
                                               const AiRealtimeMessage& message,
                                               void* userContext) {
  auto* self = static_cast<AiSessionClient*>(userContext);
  if (self != nullptr) {
    self->onTransportMessage(sessionId, message);
  }
}

void AiSessionClient::onTransportClosedStatic(const String& sessionId,
                                              const String& reason,
                                              void* userContext) {
  auto* self = static_cast<AiSessionClient*>(userContext);
  if (self != nullptr) {
    self->onTransportClosed(sessionId, reason);
  }
}

void AiSessionClient::onTransportErrorStatic(const String& sessionId,
                                             const String& errorMessage,
                                             void* userContext) {
  auto* self = static_cast<AiSessionClient*>(userContext);
  if (self != nullptr) {
    self->onTransportError(sessionId, errorMessage);
  }
}

void AiSessionClient::onTransportOpened(const String& sessionId) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr) {
    return;
  }

  session->response.realtimeAccepted = true;
  if (session->callbacks.onState != nullptr) {
    session->callbacks.onState(sessionId, true, "opened", session->callbacks.userContext);
  }
}

void AiSessionClient::onTransportMessage(const String& sessionId,
                                         const AiRealtimeMessage& message) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr || session->adapter == nullptr) {
    return;
  }

  AiRealtimeParsedEvent event;
  String parseError;
  if (!session->adapter->parseRealtimeMessage(message, event, parseError)) {
    event = AiRealtimeParsedEvent{};
    event.kind = AiRealtimeEventKind::Error;
    event.errorCode = "parse_realtime_message_failed";
    event.errorMessage = parseError.length() > 0 ? parseError : String("Unknown parse error");
  }

  if (event.textDelta.length() > 0) {
    session->response.text += event.textDelta;
  }

  if (event.hasToolCall) {
    session->response.addToolCall(event.toolCall);
  }

  if (event.promptTokens >= 0) {
    session->response.promptTokens = event.promptTokens;
  }
  if (event.completionTokens >= 0) {
    session->response.completionTokens = event.completionTokens;
  }
  if (event.totalTokens >= 0) {
    session->response.totalTokens = event.totalTokens;
  }

  if (event.kind == AiRealtimeEventKind::Error) {
    session->response.ok = false;
    session->response.errorCode =
        event.errorCode.length() > 0 ? event.errorCode : String("realtime_error");
    session->response.errorMessage = event.errorMessage;
    session->response.finishReason = "error";
  }

  if (event.done || event.kind == AiRealtimeEventKind::GenerationDone ||
      event.kind == AiRealtimeEventKind::TurnDone) {
    session->response.finishReason =
        event.doneReason.length() > 0 ? event.doneReason : String("done");
    maybeEmitDone(*session);
  }

  if (session->callbacks.onEvent != nullptr) {
    session->callbacks.onEvent(sessionId, event, session->callbacks.userContext);
  }
}

void AiSessionClient::onTransportClosed(const String& sessionId, const String& reason) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr) {
    return;
  }

  if (!session->doneNotified) {
    if (session->response.errorCode.length() == 0 && session->response.finishReason.length() == 0) {
      session->response.finishReason = reason.length() > 0 ? reason : String("closed");
    }
    maybeEmitDone(*session);
  }

  if (session->callbacks.onState != nullptr) {
    session->callbacks.onState(sessionId,
                               false,
                               reason.length() > 0 ? reason : String("closed"),
                               session->callbacks.userContext);
  }

  releaseSessionSlot(session);
}

void AiSessionClient::onTransportError(const String& sessionId, const String& errorMessage) {
  ActiveSession* session = findSession(sessionId);
  if (session == nullptr) {
    return;
  }

  session->response.ok = false;
  session->response.errorCode = "transport_error";
  session->response.errorMessage = errorMessage;
  session->response.finishReason = "error";
  maybeEmitDone(*session);

  if (session->callbacks.onState != nullptr) {
    session->callbacks.onState(sessionId, false, "error", session->callbacks.userContext);
  }
}

void AiSessionClient::maybeEmitDone(ActiveSession& session) {
  if (session.doneNotified) {
    return;
  }

  if (session.response.errorCode.length() == 0) {
    session.response.ok = true;
  }

  session.doneNotified = true;
  if (session.callbacks.onDone != nullptr) {
    session.callbacks.onDone(session.sessionId, session.response, session.callbacks.userContext);
  }
}

}  // namespace ai::provider
