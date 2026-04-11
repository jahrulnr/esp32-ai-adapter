#pragma once

#include "core/ai_provider_types.h"

namespace ai::provider {

class IAiRealtimeProviderAdapter;
class IAiAudioProviderAdapter;

class IAiProviderAdapter {
 public:
  virtual ~IAiProviderAdapter() = default;

  virtual ProviderKind kind() const = 0;
  virtual const char* id() const = 0;

  virtual bool buildHttpRequest(const AiInvokeRequest& request,
                                AiHttpRequest& outHttpRequest,
                                String& outErrorMessage) const = 0;

  virtual bool parseHttpResponse(const AiHttpResponse& httpResponse,
                                 AiInvokeResponse& outResponse) const = 0;

  virtual const IAiRealtimeProviderAdapter* asRealtimeAdapter() const {
    return nullptr;
  }

  virtual const IAiAudioProviderAdapter* asAudioAdapter() const {
    return nullptr;
  }
};

}  // namespace ai::provider
