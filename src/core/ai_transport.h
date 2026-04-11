#pragma once

#include "core/ai_provider_types.h"

namespace ai::provider {

enum class AiAsyncSubmitResult : uint8_t {
  Accepted = 0,
  Busy,
  Failed,
};

using AiTransportDoneCallback = void (*)(const AiHttpResponse& response, void* userContext);

struct AiTransportCallbacks {
  AiStreamChunkCallback onChunk = nullptr;
  AiTransportDoneCallback onDone = nullptr;
  void* userContext = nullptr;
};

class IAiTransport {
 public:
  virtual ~IAiTransport() = default;

  virtual bool execute(const AiHttpRequest& request, AiHttpResponse& outResponse) = 0;

  virtual bool supportsAsync() const {
    return false;
  }

  virtual AiAsyncSubmitResult executeAsync(const AiHttpRequest& request,
                                           const AiTransportCallbacks& callbacks,
                                           String& outErrorMessage) {
    (void)request;
    (void)callbacks;
    outErrorMessage = "Async transport not supported";
    return AiAsyncSubmitResult::Failed;
  }

  virtual void poll() {}

  virtual bool isBusy() const {
    return false;
  }

  virtual void cancel() {}
};

}  // namespace ai::provider
