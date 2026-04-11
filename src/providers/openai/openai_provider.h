#pragma once

#include "providers/openai/openai_realtime_adapter.h"
#include "providers/openai/openai_audio_adapter.h"
#include "core/ai_provider.h"

namespace ai::provider {

class OpenAiProvider : public IAiProviderAdapter {
 public:
  ProviderKind kind() const override;
  const char* id() const override;

  bool buildHttpRequest(const AiInvokeRequest& request,
                        AiHttpRequest& outHttpRequest,
                        String& outErrorMessage) const override;

  bool parseHttpResponse(const AiHttpResponse& httpResponse,
                         AiInvokeResponse& outResponse) const override;

  const IAiRealtimeProviderAdapter* asRealtimeAdapter() const override;
  const IAiAudioProviderAdapter* asAudioAdapter() const override;

 private:
  OpenAiRealtimeAdapter realtimeAdapter_{};
  OpenAiAudioAdapter audioAdapter_{};
};

}  // namespace ai::provider
