#pragma once

#include <array>

#include "core/ai_session_client.h"
#include "core/ai_tool_runtime_executor.h"

namespace ai::provider {

class AiToolRuntimeFacade {
 public:
  AiToolRuntimeFacade(AiProviderClient& httpClient,
                      AiSessionClient& sessionClient,
                      const AiToolRuntimeRegistry& toolRegistry);

  bool invokeHttpWithTools(ProviderKind provider,
                           const AiInvokeRequest& request,
                           AiInvokeResponse& outResponse,
                           String& outErrorMessage,
                           const AiToolHttpLoopOptions& options = AiToolHttpLoopOptions{}) const;

  bool startRealtimeSessionWithTools(const String& providerId,
                                     const AiInvokeRequest& request,
                                     const AiRealtimeSessionConfig& sessionConfig,
                                     const AiSessionClientCallbacks& callbacks,
                                     String& outSessionId,
                                     String& outErrorMessage);

  bool startRealtimeSessionWithTools(ProviderKind provider,
                                     const AiInvokeRequest& request,
                                     const AiRealtimeSessionConfig& sessionConfig,
                                     const AiSessionClientCallbacks& callbacks,
                                     String& outSessionId,
                                     String& outErrorMessage);

  bool closeRealtimeSession(const String& sessionId,
                            const String& reason,
                            String& outErrorMessage);

  bool sendText(const String& sessionId, const String& text, String& outErrorMessage);
  bool sendAudio(const String& sessionId,
                 const String& audioBase64,
                 const String& mimeType,
                 String& outErrorMessage);
  bool sendAudioBytesSmart(const String& sessionId,
                           const uint8_t* audioBytes,
                           size_t audioBytesLength,
                           const String& mimeType,
                           String& outErrorMessage);

  void pollRealtime();
  bool isSessionOpen(const String& sessionId) const;

 private:
  struct SessionBinding {
    bool used = false;
    String sessionId;
    AiSessionClientCallbacks callbacks;
  };

  static constexpr size_t kMaxSessionBindings = 4;

  static void onStateStatic(const String& sessionId,
                            bool connected,
                            const String& reason,
                            void* userContext);
  static void onEventStatic(const String& sessionId,
                            const AiRealtimeParsedEvent& event,
                            void* userContext);
  static void onDoneStatic(const String& sessionId,
                           const AiInvokeResponse& response,
                           void* userContext);

  void onState(const String& sessionId, bool connected, const String& reason);
  void onEvent(const String& sessionId, const AiRealtimeParsedEvent& event);
  void onDone(const String& sessionId, const AiInvokeResponse& response);

  bool attachBinding(const String& sessionId, const AiSessionClientCallbacks& callbacks);
  SessionBinding* findBinding(const String& sessionId);
  const SessionBinding* findBinding(const String& sessionId) const;
  void clearBinding(const String& sessionId);

  AiProviderClient& httpClient_;
  AiSessionClient& sessionClient_;
  const AiToolRuntimeRegistry& toolRegistry_;
  AiToolRuntimeExecutor executor_;
  std::array<SessionBinding, kMaxSessionBindings> bindings_{};
};

}  // namespace ai::provider
