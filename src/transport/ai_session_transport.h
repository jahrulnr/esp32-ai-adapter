#pragma once

#include "core/ai_transport.h"

namespace ai::provider {

using AiSessionOpenedCallback = void (*)(const String& sessionId, void* userContext);
using AiSessionMessageCallback =
    void (*)(const String& sessionId, const AiRealtimeMessage& message, void* userContext);
using AiSessionClosedCallback =
    void (*)(const String& sessionId, const String& reason, void* userContext);
using AiSessionErrorCallback =
    void (*)(const String& sessionId, const String& errorMessage, void* userContext);

struct AiSessionCallbacks {
  AiSessionOpenedCallback onOpened = nullptr;
  AiSessionMessageCallback onMessage = nullptr;
  AiSessionClosedCallback onClosed = nullptr;
  AiSessionErrorCallback onError = nullptr;
  void* userContext = nullptr;
};

class IAiSessionTransport {
 public:
  virtual ~IAiSessionTransport() = default;

  virtual AiAsyncSubmitResult openSession(const AiRealtimeSessionConfig& sessionConfig,
                                          const AiHttpRequest& initRequest,
                                          const AiSessionCallbacks& callbacks,
                                          String& outSessionId,
                                          String& outErrorMessage) = 0;

  virtual bool sendMessage(const String& sessionId,
                           const AiRealtimeMessage& message,
                           String& outErrorMessage) = 0;

  virtual bool closeSession(const String& sessionId,
                            const String& reason,
                            String& outErrorMessage) = 0;

  virtual bool isSessionOpen(const String& sessionId) const = 0;

  virtual void pollSessions() = 0;
};

}  // namespace ai::provider
