#pragma once

#include "core/ai_provider.h"
#include "providers/openrouter/openrouter_audio_adapter.h"

namespace ai::provider {

class OpenRouterProvider : public IAiProviderAdapter {
 public:
  ProviderKind kind() const override;
  const char* id() const override;

  bool buildHttpRequest(const AiInvokeRequest& request,
                        AiHttpRequest& outHttpRequest,
                        String& outErrorMessage) const override;

  bool parseHttpResponse(const AiHttpResponse& httpResponse,
                         AiInvokeResponse& outResponse) const override;

  const IAiAudioProviderAdapter* asAudioAdapter() const override;

 private:
  OpenRouterAudioAdapter audioAdapter_{};
};

}  // namespace ai::provider
