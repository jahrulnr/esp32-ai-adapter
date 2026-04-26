#pragma once

#include "core/ai_provider_client.h"
#include "core/ai_session_client.h"
#include "core/ai_tool_runtime.h"

namespace ai::provider {

#ifndef AIPROVIDERKIT_TOOL_LOOP_MAX_ROUNDS
#define AIPROVIDERKIT_TOOL_LOOP_MAX_ROUNDS 50U
#endif

struct AiToolHttpLoopOptions {
  bool continueOnToolError = false;
};

class AiToolRuntimeExecutor {
 public:
  explicit AiToolRuntimeExecutor(const AiToolRuntimeRegistry& registry);

  bool invokeHttpWithTools(AiProviderClient& client,
                           ProviderKind provider,
                           const AiInvokeRequest& request,
                           AiInvokeResponse& outResponse,
                           String& outErrorMessage,
                           const AiToolHttpLoopOptions& options = AiToolHttpLoopOptions{}) const;

  bool onRealtimeToolCall(const String& sessionId,
                          const AiRealtimeParsedEvent& event,
                          AiSessionClient& sessionClient,
                          String& outErrorMessage) const;

 private:
  static String toJsonErrorObject(const String& message);
  static bool ensureSeedMessages(AiInvokeRequest& request, String& outErrorMessage);

  const AiToolRuntimeRegistry& registry_;
};

}  // namespace ai::provider
