#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>

#include <array>

#include "transport/ai_session_transport.h"

namespace ai::provider {

class WebSocketSessionTransport : public IAiSessionTransport {
 public:
  struct Config {
    size_t maxSessions = 2;
    uint32_t reconnectIntervalMs = 1000;
    uint32_t heartbeatPingMs = 15000;
    uint32_t heartbeatPongTimeoutMs = 3000;
    uint8_t heartbeatMaxMissedPongs = 2;
  };

  WebSocketSessionTransport();
  explicit WebSocketSessionTransport(const Config& config);

  AiAsyncSubmitResult openSession(const AiRealtimeSessionConfig& sessionConfig,
                                  const AiHttpRequest& initRequest,
                                  const AiSessionCallbacks& callbacks,
                                  String& outSessionId,
                                  String& outErrorMessage) override;

  bool sendMessage(const String& sessionId,
                   const AiRealtimeMessage& message,
                   String& outErrorMessage) override;

  bool closeSession(const String& sessionId,
                    const String& reason,
                    String& outErrorMessage) override;

  bool isSessionOpen(const String& sessionId) const override;

  void pollSessions() override;

 private:
  struct UrlParts {
    bool secure = false;
    String host;
    uint16_t port = 0;
    String path;
  };

  struct SessionSlot {
    bool active = false;
    String sessionId;
    UrlParts url;
    AiSessionCallbacks callbacks;
    WebSocketsClient client;
    bool openedNotified = false;
  };

  static constexpr size_t kMaxSessionSlots = 4;

  static bool parseWsUrl(const String& url, UrlParts& outParts, String& outErrorMessage);
  static String buildExtraHeaders(const AiHttpRequest& request);
  static String buildOutgoingPayload(const AiRealtimeMessage& message);

  SessionSlot* allocateSlot();
  SessionSlot* findSlot(const String& sessionId);
  const SessionSlot* findSlot(const String& sessionId) const;
  void releaseSlot(SessionSlot* slot, const String& reason, bool notifyClosed);
  void handleClientEvent(size_t slotIndex, WStype_t type, uint8_t* payload, size_t length);

  Config config_;
  std::array<SessionSlot, kMaxSessionSlots> slots_{};
};

}  // namespace ai::provider
