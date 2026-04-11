#include "core/ai_tool_runtime_executor.h"

namespace ai::provider {

AiToolRuntimeExecutor::AiToolRuntimeExecutor(const AiToolRuntimeRegistry& registry)
    : registry_(registry) {}

bool AiToolRuntimeExecutor::invokeHttpWithTools(
    AiProviderClient& client,
    ProviderKind provider,
    const AiInvokeRequest& request,
    AiInvokeResponse& outResponse,
    String& outErrorMessage,
    const AiToolHttpLoopOptions& options) const {
  outErrorMessage = String();
  outResponse = AiInvokeResponse{};

  AiInvokeRequest working = request;
  if (working.enableToolCalls && working.toolCount == 0 && registry_.size() > 0) {
    if (!registry_.appendToolDefinitionsToRequest(working, outErrorMessage)) {
      return false;
    }
  }

  const size_t maxRounds = options.maxRounds == 0 ? 1 : options.maxRounds;

  for (size_t round = 0; round < maxRounds; ++round) {
    AiInvokeResponse response;
    const bool ok = client.invoke(provider, working, response);
    outResponse = response;

    if (response.toolCallCount == 0) {
      if (!ok && outErrorMessage.length() == 0) {
        outErrorMessage = response.errorMessage.length() > 0 ? response.errorMessage
                                                             : String("Invoke failed");
      }
      return ok;
    }

    AiInvokeRequest next = working;
    String seedError;
    if (!ensureSeedMessages(next, seedError)) {
      outErrorMessage = seedError;
      return false;
    }

    for (size_t i = 0; i < response.toolCallCount; ++i) {
      const AiToolCall& call = response.toolCalls[i];

      String toolResult;
      String toolError;
      if (!registry_.onCall(call, toolResult, toolError)) {
        if (!options.continueOnToolError) {
          outErrorMessage = toolError.length() > 0 ? toolError : String("Tool onCall failed");
          return false;
        }
        toolResult = toJsonErrorObject(toolError.length() > 0 ? toolError : String("onCall failed"));
      }

      AiChatMessage toolMessage;
      toolMessage.role = AiMessageRole::Tool;
      toolMessage.content = toolResult;
      toolMessage.toolCallId = call.id;
      toolMessage.toolName = call.name;
      if (!next.addMessage(toolMessage)) {
        outErrorMessage = "Unable to append tool message to request";
        return false;
      }
    }

    next.enableToolCalls = true;
    working = next;
  }

  outErrorMessage = "Maximum HTTP tool loop rounds reached";
  return false;
}

bool AiToolRuntimeExecutor::onRealtimeToolCall(const String& sessionId,
                                               const AiRealtimeParsedEvent& event,
                                               AiSessionClient& sessionClient,
                                               String& outErrorMessage) const {
  outErrorMessage = String();

  if (!event.hasToolCall) {
    return true;
  }

  String resultJson;
  if (!registry_.onCall(event.toolCall, resultJson, outErrorMessage)) {
    return false;
  }

  if (!sessionClient.sendToolResult(
          sessionId, event.toolCall.id, event.toolCall.name, resultJson, outErrorMessage)) {
    return false;
  }

  return true;
}

String AiToolRuntimeExecutor::toJsonErrorObject(const String& message) {
  String escaped;
  escaped.reserve(message.length() + 8);
  for (size_t i = 0; i < message.length(); ++i) {
    const char c = message[i];
    if (c == '\\' || c == '"') {
      escaped += '\\';
    }
    escaped += c;
  }

  return String("{\"error\":\"") + escaped + String("\"}");
}

bool AiToolRuntimeExecutor::ensureSeedMessages(AiInvokeRequest& request, String& outErrorMessage) {
  outErrorMessage = String();

  if (request.messageCount > 0) {
    return true;
  }

  if (request.systemPrompt.length() > 0) {
    AiChatMessage system;
    system.role = AiMessageRole::System;
    system.content = request.systemPrompt;
    if (!request.addMessage(system)) {
      outErrorMessage = "Unable to append system prompt message";
      return false;
    }
  }

  if (request.prompt.length() > 0) {
    AiChatMessage user;
    user.role = AiMessageRole::User;
    user.content = request.prompt;
    if (!request.addMessage(user)) {
      outErrorMessage = "Unable to append prompt as user message";
      return false;
    }
  }

  if (request.messageCount == 0) {
    outErrorMessage = "Request has no prompt or messages for tool loop";
    return false;
  }

  return true;
}

}  // namespace ai::provider
