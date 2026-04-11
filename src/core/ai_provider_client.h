#pragma once

#include "core/ai_provider_registry.h"
#include "core/ai_transport.h"

namespace ai::provider {

using AiInvokeDoneCallback = void (*)(const AiInvokeResponse& response, void* userContext);

class AiProviderClient {
 public:
  AiProviderClient(IAiTransport& transport, const AiProviderRegistry& registry);

  bool invoke(ProviderKind kind,
              const AiInvokeRequest& request,
              AiInvokeResponse& outResponse) const;

  bool invoke(const String& providerId,
              const AiInvokeRequest& request,
              AiInvokeResponse& outResponse) const;

  bool invokeAsync(ProviderKind kind,
                   const AiInvokeRequest& request,
                   AiInvokeDoneCallback doneCallback,
                   void* userContext,
                   String& outErrorMessage);

  bool invokeAsync(const String& providerId,
                   const AiInvokeRequest& request,
                   AiInvokeDoneCallback doneCallback,
                   void* userContext,
                   String& outErrorMessage);

  void poll();
  bool isBusy() const;
  void cancel();

 private:
  bool invoke(const IAiProviderAdapter* adapter,
              const AiInvokeRequest& request,
              AiInvokeResponse& outResponse) const;

  bool invokeAsync(const IAiProviderAdapter* adapter,
                   const AiInvokeRequest& request,
                   AiInvokeDoneCallback doneCallback,
                   void* userContext,
                   String& outErrorMessage);

  static void onTransportChunkStatic(const AiStreamChunk& chunk, void* userContext);
  static void onTransportDoneStatic(const AiHttpResponse& response, void* userContext);

  void onTransportChunk(const AiStreamChunk& chunk);
  void onTransportDone(const AiHttpResponse& response);

  void resetPending();

  IAiTransport& transport_;
  const AiProviderRegistry& registry_;

  struct PendingInvokeContext {
    bool active = false;
    const IAiProviderAdapter* adapter = nullptr;
    AiInvokeDoneCallback doneCallback = nullptr;
    void* doneUserContext = nullptr;
    AiStreamChunkCallback streamCallback = nullptr;
    void* streamUserContext = nullptr;
    bool requestStream = false;
    bool streamSawText = false;
    String streamText;
    String streamDoneReason;
  };

  PendingInvokeContext pending_{};
};

}  // namespace ai::provider
