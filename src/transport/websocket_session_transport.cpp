#include "transport/websocket_session_transport.h"

#include <algorithm>

namespace ai::provider {

namespace {

String generateSessionId() {
  String id = "ws-";
  id += String(static_cast<unsigned long>(millis()));
  id += "-";
  id += String(static_cast<unsigned long>(esp_random()), HEX);
  return id;
}

void notifyError(const AiSessionCallbacks& callbacks, const String& sessionId, const String& message) {
  if (!callbacks.onError) {
    return;
  }
  callbacks.onError(sessionId, message, callbacks.userContext);
}

void notifyMessage(const AiSessionCallbacks& callbacks,
                   const String& sessionId,
                   const AiRealtimeMessage& message) {
  if (!callbacks.onMessage) {
    return;
  }
  callbacks.onMessage(sessionId, message, callbacks.userContext);
}

}  // namespace
WebSocketSessionTransport::WebSocketSessionTransport() : WebSocketSessionTransport(Config{}) {}


WebSocketSessionTransport::WebSocketSessionTransport(const Config& config) : config_(config) {
  config_.maxSessions = std::min(config_.maxSessions, slots_.size());
  if (config_.maxSessions == 0) {
    config_.maxSessions = 1;
  }
}

AiAsyncSubmitResult WebSocketSessionTransport::openSession(const AiRealtimeSessionConfig& sessionConfig,
                                                           const AiHttpRequest& initRequest,
                                                           const AiSessionCallbacks& callbacks,
                                                           String& outSessionId,
                                                           String& outErrorMessage) {
  outSessionId = String();
  outErrorMessage = String();

  SessionSlot* slot = allocateSlot();
  if (slot == nullptr) {
    outErrorMessage = "No available websocket session slot";
    return AiAsyncSubmitResult::Busy;
  }

  String url = initRequest.url;
  if (url.isEmpty()) {
    url = sessionConfig.sessionUrl;
  }
  if (url.isEmpty()) {
    outErrorMessage = "Realtime session URL is empty";
    return AiAsyncSubmitResult::Failed;
  }

  UrlParts parts;
  if (!parseWsUrl(url, parts, outErrorMessage)) {
    return AiAsyncSubmitResult::Failed;
  }

  slot->active = true;
  slot->sessionId = generateSessionId();
  slot->url = parts;
  slot->callbacks = callbacks;
  slot->openedNotified = false;

  String auth;
  for (size_t i = 0; i < initRequest.headerCount; ++i) {
    const auto& h = initRequest.headers[i];
    if (h.key.equalsIgnoreCase("Authorization")) {
      auth = h.value;
      break;
    }
  }
  if (!auth.isEmpty()) {
    slot->client.setAuthorization(auth.c_str());
  }
  const String extraHeaders = buildExtraHeaders(initRequest);
  if (!extraHeaders.isEmpty()) {
    slot->client.setExtraHeaders(extraHeaders.c_str());
  }

  slot->client.setReconnectInterval(config_.reconnectIntervalMs);
  slot->client.enableHeartbeat(config_.heartbeatPingMs,
                               config_.heartbeatPongTimeoutMs,
                               config_.heartbeatMaxMissedPongs);

  const size_t slotIndex = static_cast<size_t>(slot - &slots_[0]);
  slot->client.onEvent([this, slotIndex](WStype_t type, uint8_t* payload, size_t length) {
    handleClientEvent(slotIndex, type, payload, length);
  });

  if (parts.secure) {
    slot->client.beginSSL(parts.host.c_str(), parts.port, parts.path.c_str());
  } else {
    slot->client.begin(parts.host.c_str(), parts.port, parts.path.c_str());
  }

  outSessionId = slot->sessionId;
  return AiAsyncSubmitResult::Accepted;
}

bool WebSocketSessionTransport::sendMessage(const String& sessionId,
                                            const AiRealtimeMessage& message,
                                            String& outErrorMessage) {
  outErrorMessage = String();
  SessionSlot* slot = findSlot(sessionId);
  if (slot == nullptr || !slot->active) {
    outErrorMessage = "Session not found";
    return false;
  }

  String payload = buildOutgoingPayload(message);
  if (payload.isEmpty()) {
    outErrorMessage = "Realtime message payload is empty";
    return false;
  }

  if (!slot->client.sendTXT(payload)) {
    outErrorMessage = "Failed to send websocket text frame";
    return false;
  }

  return true;
}

bool WebSocketSessionTransport::closeSession(const String& sessionId,
                                             const String& reason,
                                             String& outErrorMessage) {
  outErrorMessage = String();
  SessionSlot* slot = findSlot(sessionId);
  if (slot == nullptr || !slot->active) {
    outErrorMessage = "Session not found";
    return false;
  }
  releaseSlot(slot, reason, true);
  return true;
}

bool WebSocketSessionTransport::isSessionOpen(const String& sessionId) const {
  const SessionSlot* slot = findSlot(sessionId);
  if (slot == nullptr || !slot->active) {
    return false;
  }
  return slot->openedNotified;
}

void WebSocketSessionTransport::pollSessions() {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (!slots_[i].active) {
      continue;
    }
    slots_[i].client.loop();
  }
}

bool WebSocketSessionTransport::parseWsUrl(const String& url,
                                           UrlParts& outParts,
                                           String& outErrorMessage) {
  outParts = UrlParts{};
  outErrorMessage = String();

  String lower = url;
  lower.toLowerCase();

  bool secure = false;
  int schemeLen = 0;
  if (lower.startsWith("wss://")) {
    secure = true;
    schemeLen = 6;
  } else if (lower.startsWith("ws://")) {
    secure = false;
    schemeLen = 5;
  } else {
    outErrorMessage = "Unsupported realtime URL scheme (expected ws:// or wss://)";
    return false;
  }

  const int hostStart = schemeLen;
  int pathStart = url.indexOf('/', hostStart);
  String hostPort = pathStart >= 0 ? url.substring(hostStart, pathStart) : url.substring(hostStart);
  String path = pathStart >= 0 ? url.substring(pathStart) : String("/");

  if (hostPort.isEmpty()) {
    outErrorMessage = "Realtime URL host is empty";
    return false;
  }

  uint16_t port = secure ? 443 : 80;
  int colon = hostPort.lastIndexOf(':');
  if (colon > 0) {
    String hostPart = hostPort.substring(0, colon);
    String portPart = hostPort.substring(colon + 1);
    int parsedPort = portPart.toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      outErrorMessage = "Invalid realtime URL port";
      return false;
    }
    hostPort = hostPart;
    port = static_cast<uint16_t>(parsedPort);
  }

  if (hostPort.isEmpty()) {
    outErrorMessage = "Realtime URL host is empty";
    return false;
  }

  outParts.secure = secure;
  outParts.host = hostPort;
  outParts.port = port;
  outParts.path = path;
  return true;
}

String WebSocketSessionTransport::buildExtraHeaders(const AiHttpRequest& request) {
  String headers;
  for (size_t i = 0; i < request.headerCount; ++i) {
    const auto& h = request.headers[i];
    if (h.key.equalsIgnoreCase("Authorization")) {
      continue;
    }
    if (h.key.isEmpty()) {
      continue;
    }
    headers += h.key;
    headers += ": ";
    headers += h.value;
    headers += "\r\n";
  }
  return headers;
}

String WebSocketSessionTransport::buildOutgoingPayload(const AiRealtimeMessage& message) {
  if (!message.payload.isEmpty()) {
    return message.payload;
  }
  return message.text;
}

WebSocketSessionTransport::SessionSlot* WebSocketSessionTransport::allocateSlot() {
  size_t activeCount = 0;
  for (auto& slot : slots_) {
    if (slot.active) {
      ++activeCount;
    }
  }
  if (activeCount >= config_.maxSessions) {
    return nullptr;
  }

  for (auto& slot : slots_) {
    if (!slot.active) {
      return &slot;
    }
  }
  return nullptr;
}

WebSocketSessionTransport::SessionSlot* WebSocketSessionTransport::findSlot(const String& sessionId) {
  for (auto& slot : slots_) {
    if (slot.active && slot.sessionId == sessionId) {
      return &slot;
    }
  }
  return nullptr;
}

const WebSocketSessionTransport::SessionSlot* WebSocketSessionTransport::findSlot(
    const String& sessionId) const {
  for (const auto& slot : slots_) {
    if (slot.active && slot.sessionId == sessionId) {
      return &slot;
    }
  }
  return nullptr;
}

void WebSocketSessionTransport::releaseSlot(SessionSlot* slot,
                                            const String& reason,
                                            bool notifyClosed) {
  if (slot == nullptr || !slot->active) {
    return;
  }

  const String sessionId = slot->sessionId;
  const AiSessionCallbacks callbacks = slot->callbacks;

  slot->client.disconnect();
  slot->active = false;
  slot->sessionId = String();
  slot->url = UrlParts{};
  slot->callbacks = AiSessionCallbacks{};
  slot->openedNotified = false;

  if (notifyClosed && callbacks.onClosed) {
    callbacks.onClosed(sessionId, reason, callbacks.userContext);
  }
}

void WebSocketSessionTransport::handleClientEvent(size_t slotIndex,
                                                  WStype_t type,
                                                  uint8_t* payload,
                                                  size_t length) {
  if (slotIndex >= slots_.size()) {
    return;
  }
  SessionSlot& slot = slots_[slotIndex];
  if (!slot.active) {
    return;
  }

  switch (type) {
    case WStype_CONNECTED: {
      slot.openedNotified = true;
      if (slot.callbacks.onOpened) {
        slot.callbacks.onOpened(slot.sessionId, slot.callbacks.userContext);
      }
      break;
    }
    case WStype_DISCONNECTED: {
      const String reason = "WebSocket disconnected";
      releaseSlot(&slot, reason, true);
      break;
    }
    case WStype_ERROR: {
      String message;
      if (payload != nullptr && length > 0) {
        message.reserve(length + 1);
        for (size_t i = 0; i < length; ++i) {
          message += static_cast<char>(payload[i]);
        }
      } else {
        message = "WebSocket error";
      }
      notifyError(slot.callbacks, slot.sessionId, message);
      break;
    }
    case WStype_TEXT: {
      AiRealtimeMessage message;
      message.eventType = "ws.text";
      message.kind = AiRealtimeEventKind::ProviderEvent;
      if (payload != nullptr && length > 0) {
        message.payload.reserve(length + 1);
        for (size_t i = 0; i < length; ++i) {
          message.payload += static_cast<char>(payload[i]);
        }
      }
      notifyMessage(slot.callbacks, slot.sessionId, message);
      break;
    }
    case WStype_BIN: {
      AiRealtimeMessage message;
      message.eventType = "ws.binary";
      message.kind = AiRealtimeEventKind::AudioDelta;
      message.payload = "binary_length=" + String(static_cast<unsigned long>(length));
      notifyMessage(slot.callbacks, slot.sessionId, message);
      break;
    }
    default:
      break;
  }
}

}  // namespace ai::provider
