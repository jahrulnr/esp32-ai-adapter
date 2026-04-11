#pragma once

#include "core/ai_realtime_adapter.h"

namespace ai::provider {

class OpenAiRealtimeAdapter : public IAiRealtimeProviderAdapter {
 public:
  ProviderKind kind() const override;
  const char* id() const override;
  bool supportsRealtime() const override;

  bool buildRealtimeSessionRequest(const AiInvokeRequest& request,
                                   const AiRealtimeSessionConfig& sessionConfig,
                                   AiHttpRequest& outHttpRequest,
                                   String& outErrorMessage) const override;

  bool buildRealtimeTextMessage(const String& text,
                                AiRealtimeMessage& outMessage,
                                String& outErrorMessage) const override;

  bool buildRealtimeAudioMessage(const String& audioBase64,
                                 const String& mimeType,
                                 AiRealtimeMessage& outMessage,
                                 String& outErrorMessage) const override;

  bool buildRealtimeToolResultMessage(const String& toolCallId,
                                      const String& toolName,
                                      const String& toolResultJson,
                                      AiRealtimeMessage& outMessage,
                                      String& outErrorMessage) const override;

  bool parseRealtimeMessage(const AiRealtimeMessage& message,
                            AiRealtimeParsedEvent& outEvent,
                            String& outErrorMessage) const override;
};

}  // namespace ai::provider
