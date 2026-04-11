#pragma once

#include <array>

#include <FS.h>

#include "core/ai_provider_registry.h"
#include "core/ai_realtime_adapter.h"
#include "transport/ai_session_transport.h"

namespace ai::provider {

using AiRealtimeStateCallback =
    void (*)(const String& sessionId, bool connected, const String& reason, void* userContext);
using AiRealtimeEventCallback =
    void (*)(const String& sessionId, const AiRealtimeParsedEvent& event, void* userContext);
using AiRealtimeDoneCallback =
    void (*)(const String& sessionId, const AiInvokeResponse& response, void* userContext);

struct AiSessionClientCallbacks {
  AiRealtimeStateCallback onState = nullptr;
  AiRealtimeEventCallback onEvent = nullptr;
  AiRealtimeDoneCallback onDone = nullptr;
  void* userContext = nullptr;
};

struct AiAudioChunkingOptions {
  size_t preferredChunkBytes = 8192;
  size_t minChunkBytes = 512;
  size_t lowMemoryThresholdBytes = 96 * 1024;
  size_t safetyHeadroomBytes = 24 * 1024;

  fs::FS* spillFs = nullptr;
  String spillFilePath = "/aipk_audio_spill.bin";
  bool removeSpillFileAfterSend = true;
};

class AiSessionClient {
 public:
  AiSessionClient(IAiSessionTransport& transport, const AiProviderRegistry& registry);

  bool startSession(ProviderKind kind,
                    const AiInvokeRequest& request,
                    const AiRealtimeSessionConfig& sessionConfig,
                    const AiSessionClientCallbacks& callbacks,
                    String& outSessionId,
                    String& outErrorMessage);

  bool startSession(const String& providerId,
                    const AiInvokeRequest& request,
                    const AiRealtimeSessionConfig& sessionConfig,
                    const AiSessionClientCallbacks& callbacks,
                    String& outSessionId,
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
  bool sendAudioBytesSmart(const String& sessionId,
                           const uint8_t* audioBytes,
                           size_t audioBytesLength,
                           const String& mimeType,
                           const AiAudioChunkingOptions& options,
                           String& outErrorMessage);
  bool sendToolResult(const String& sessionId,
                      const String& toolCallId,
                      const String& toolName,
                      const String& toolResultJson,
                      String& outErrorMessage);

  bool closeSession(const String& sessionId,
                    const String& reason,
                    String& outErrorMessage);

  bool isSessionOpen(const String& sessionId) const;
  void poll();

 private:
  struct ActiveSession {
    bool active = false;
    String sessionId;
    const IAiRealtimeProviderAdapter* adapter = nullptr;
    AiSessionClientCallbacks callbacks;
    AiInvokeResponse response;
    bool doneNotified = false;
  };

  static constexpr size_t kMaxActiveSessions = 4;

  bool startSession(const IAiProviderAdapter* adapter,
                    const AiInvokeRequest& request,
                    const AiRealtimeSessionConfig& sessionConfig,
                    const AiSessionClientCallbacks& callbacks,
                    String& outSessionId,
                    String& outErrorMessage);

  ActiveSession* allocateSessionSlot();
  void releaseSessionSlot(ActiveSession* slot);
  ActiveSession* findSession(const String& sessionId);
  const ActiveSession* findSession(const String& sessionId) const;

  static void onTransportOpenedStatic(const String& sessionId, void* userContext);
  static void onTransportMessageStatic(const String& sessionId,
                                       const AiRealtimeMessage& message,
                                       void* userContext);
  static void onTransportClosedStatic(const String& sessionId,
                                      const String& reason,
                                      void* userContext);
  static void onTransportErrorStatic(const String& sessionId,
                                     const String& errorMessage,
                                     void* userContext);

  void onTransportOpened(const String& sessionId);
  void onTransportMessage(const String& sessionId, const AiRealtimeMessage& message);
  void onTransportClosed(const String& sessionId, const String& reason);
  void onTransportError(const String& sessionId, const String& errorMessage);
  void maybeEmitDone(ActiveSession& session);

  IAiSessionTransport& transport_;
  const AiProviderRegistry& registry_;
  std::array<ActiveSession, kMaxActiveSessions> sessions_{};
};

}  // namespace ai::provider
