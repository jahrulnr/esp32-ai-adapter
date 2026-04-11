#pragma once

#include "core/ai_provider.h"

namespace ai::provider {

class ClaudeProvider : public IAiProviderAdapter {
 public:
  ProviderKind kind() const override;
  const char* id() const override;

  bool buildHttpRequest(const AiInvokeRequest& request,
                        AiHttpRequest& outHttpRequest,
                        String& outErrorMessage) const override;

  bool parseHttpResponse(const AiHttpResponse& httpResponse,
                         AiInvokeResponse& outResponse) const override;
};

}  // namespace ai::provider
