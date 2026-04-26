#include "core/ai_tool_runtime_executor.h"

#include <SpiJsonDocument.h>
#include <esp_heap_caps.h>
#include <new>

namespace ai::provider {
namespace {

AiInvokeRequest* allocateInvokeRequestCopy(const AiInvokeRequest& source) {
  void* memory = heap_caps_calloc(1, sizeof(AiInvokeRequest), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr) {
    memory = heap_caps_calloc(1, sizeof(AiInvokeRequest), MALLOC_CAP_8BIT);
  }

  if (memory == nullptr) {
    return nullptr;
  }

  return ::new (memory) AiInvokeRequest(source);
}

void releaseInvokeRequest(AiInvokeRequest* request) {
  if (request == nullptr) {
    return;
  }

  request->~AiInvokeRequest();
  heap_caps_free(request);
}

int findJsonStartIndex(const String& text, size_t from) {
  const int objectIndex = text.indexOf('{', from);
  const int arrayIndex = text.indexOf('[', from);
  if (objectIndex < 0) {
    return arrayIndex;
  }
  if (arrayIndex < 0) {
    return objectIndex;
  }
  return objectIndex < arrayIndex ? objectIndex : arrayIndex;
}

int findMatchingJsonEnd(const String& text, size_t start) {
  if (start >= text.length()) {
    return -1;
  }

  const char opener = text[start];
  const char closer = opener == '[' ? ']' : (opener == '{' ? '}' : '\0');
  if (closer == '\0') {
    return -1;
  }

  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (size_t i = start; i < text.length(); ++i) {
    const char c = text[i];
    if (inString) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (c == '\\') {
        escaped = true;
        continue;
      }
      if (c == '"') {
        inString = false;
      }
      continue;
    }

    if (c == '"') {
      inString = true;
      continue;
    }

    if (c == opener) {
      ++depth;
      continue;
    }

    if (c == closer) {
      --depth;
      if (depth == 0) {
        return static_cast<int>(i);
      }
    }
  }

  return -1;
}

bool parseLooseToolCallItem(ArduinoJson::JsonObjectConst item,
                            size_t index,
                            AiInvokeResponse& outResponse) {
  if (item.isNull()) {
    return false;
  }

  const char* name = item["name"] | nullptr;
  if ((name == nullptr || name[0] == '\0') && item["tool"].is<const char*>()) {
    name = item["tool"].as<const char*>();
  }
  if (name == nullptr || name[0] == '\0') {
    return false;
  }

  AiToolCall call;
  call.id = item["id"].is<const char*>() ? item["id"].as<const char*>() : String();
  if (call.id.length() == 0) {
    call.id = String("fallback_tool_") + String(static_cast<unsigned>(index + 1));
  }
  call.type = "function";
  call.name = name;

  ArduinoJson::JsonVariantConst argsVar = item["arguments"];
  if (argsVar.isNull()) {
    argsVar = item["args"];
  }
  if (argsVar.isNull()) {
    call.argumentsJson = "{}";
  } else if (argsVar.is<const char*>()) {
    call.argumentsJson = argsVar.as<const char*>();
    if (call.argumentsJson.length() == 0) {
      call.argumentsJson = "{}";
    }
  } else {
    String argsJson;
    serializeJson(argsVar, argsJson);
    call.argumentsJson = argsJson.length() == 0 ? "{}" : argsJson;
  }

  return outResponse.addToolCall(call);
}

bool appendToolCallsFromLooseJson(const String& jsonPayload, AiInvokeResponse& outResponse) {
  if (jsonPayload.length() == 0) {
    return false;
  }

  SpiJsonDocument doc;
  if (deserializeJson(doc, jsonPayload) != DeserializationError::Ok) {
    return false;
  }

  size_t added = 0;
  if (doc.is<ArduinoJson::JsonObjectConst>()) {
    if (parseLooseToolCallItem(doc.as<ArduinoJson::JsonObjectConst>(), added, outResponse)) {
      ++added;
    }
  } else if (doc.is<ArduinoJson::JsonArrayConst>()) {
    for (ArduinoJson::JsonVariantConst entry : doc.as<ArduinoJson::JsonArrayConst>()) {
      if (!entry.is<ArduinoJson::JsonObjectConst>()) {
        continue;
      }
      if (parseLooseToolCallItem(entry.as<ArduinoJson::JsonObjectConst>(), added, outResponse)) {
        ++added;
      }
    }
  } else {
    return false;
  }

  return added > 0;
}

bool tryAppendToolCallsFromText(const String& text, AiInvokeResponse& outResponse) {
  if (text.length() == 0) {
    return false;
  }

  const int markerIndex = text.indexOf("TOOLCALL");
  if (markerIndex < 0) {
    return false;
  }

  int payloadStart = findJsonStartIndex(text, static_cast<size_t>(markerIndex));
  if (payloadStart < 0) {
    return false;
  }

  int payloadEnd = findMatchingJsonEnd(text, static_cast<size_t>(payloadStart));
  if (payloadEnd < payloadStart) {
    return false;
  }

  const String payload = text.substring(static_cast<size_t>(payloadStart),
                                        static_cast<size_t>(payloadEnd + 1));
  return appendToolCallsFromLooseJson(payload, outResponse);
}

String extractQueryFromToolCall(const String& argumentsJson) {
  SpiJsonDocument doc;
  if (deserializeJson(doc, argumentsJson) != DeserializationError::Ok) {
    return "";
  }
  String query = doc["query"].is<const char*>() ? doc["query"].as<const char*>() : "";
  query.trim();
  return query;
}

}  // namespace

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

  auto* workingPtr = allocateInvokeRequestCopy(request);
  if (workingPtr == nullptr) {
    outErrorMessage = "Insufficient memory for invoke request";
    return false;
  }

  AiInvokeRequest& working = *workingPtr;
  if (working.enableToolCalls && working.toolCount == 0 && registry_.size() > 0) {
    if (!registry_.appendToolDefinitionsToRequest(
            working, outErrorMessage, AiToolRuntimeRegistry::AppendMode::InitialOnly)) {
      releaseInvokeRequest(workingPtr);
      return false;
    }
  }

  ToolLoopPolicyState policyState{};
  for (size_t round = 0; round < AIPROVIDERKIT_TOOL_LOOP_MAX_ROUNDS; ++round) {
    AiInvokeResponse response;
    const bool ok = client.invoke(provider, working, response);

    if (ok && working.enableToolCalls && response.toolCallCount == 0) {
      (void)tryAppendToolCallsFromText(response.text, response);
    }

    outResponse = response;

    if (response.toolCallCount == 0) {
      if (!ok && outErrorMessage.length() == 0) {
        outErrorMessage = response.errorMessage.length() > 0 ? response.errorMessage
                                                             : String("Invoke failed");
      }
      releaseInvokeRequest(workingPtr);
      return ok;
    }

    String seedError;
    if (!ensureSeedMessages(working, seedError)) {
      outErrorMessage = seedError;
      releaseInvokeRequest(workingPtr);
      return false;
    }

    for (size_t i = 0; i < response.toolCallCount; ++i) {
      const AiToolCall& call = response.toolCalls[i];

      String toolResult;
      String toolError;
      if (!checkPrerequisites(call, policyState, toolResult)) {
        toolError = "prerequisite_required";
      } else if (!registry_.onCall(call, toolResult, toolError)) {
        if (!options.continueOnToolError) {
          outErrorMessage = toolError.length() > 0 ? toolError : String("Tool onCall failed");
          releaseInvokeRequest(workingPtr);
          return false;
        }
        toolResult = toJsonErrorObject(toolError.length() > 0 ? toolError : String("onCall failed"));
      } else {
        recordSuccessfulTool(policyState, call.name);
      }

      if (call.name.equalsIgnoreCase("tools.find")) {
        String appendError;
        const String query = extractQueryFromToolCall(call.argumentsJson);
        if (!registry_.appendDiscoveredToolDefinitionsToRequest(query, 5, working, appendError)) {
          outErrorMessage = appendError.length() > 0 ? appendError : String("tool_discovery_append_failed");
          releaseInvokeRequest(workingPtr);
          return false;
        }
      }

      if (!working.addMessage(AiMessageRole::Tool, toolResult, call.id, call.name)) {
        outErrorMessage = "Unable to append tool message to request";
        releaseInvokeRequest(workingPtr);
        return false;
      }
    }

    working.enableToolCalls = true;
  }

  outErrorMessage = "Maximum HTTP tool loop rounds reached";
  releaseInvokeRequest(workingPtr);
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

String AiToolRuntimeExecutor::toJsonErrorObject(const String& code,
                                                const String& message,
                                                const String& nextTool) {
  String out = "{\"error\":{\"code\":\"";
  out += code;
  out += "\",\"message\":\"";
  for (size_t i = 0; i < message.length(); ++i) {
    const char c = message[i];
    if (c == '\\' || c == '"') {
      out += '\\';
    }
    out += c;
  }
  out += "\"";
  if (nextTool.length() > 0) {
    out += ",\"nextTool\":\"";
    out += nextTool;
    out += "\"";
  }
  out += "}}";
  return out;
}

bool AiToolRuntimeExecutor::hasSuccessfulTool(const ToolLoopPolicyState& state, const String& toolName) {
  for (size_t i = 0; i < state.successfulToolCount; ++i) {
    if (state.successfulTools[i].equalsIgnoreCase(toolName)) {
      return true;
    }
  }
  return false;
}

void AiToolRuntimeExecutor::recordSuccessfulTool(ToolLoopPolicyState& state, const String& toolName) {
  if (toolName.length() == 0 || hasSuccessfulTool(state, toolName)) {
    return;
  }
  if (state.successfulToolCount >= state.successfulTools.size()) {
    return;
  }
  state.successfulTools[state.successfulToolCount++] = toolName;
}

bool AiToolRuntimeExecutor::checkPrerequisites(const AiToolCall& call,
                                               const ToolLoopPolicyState& state,
                                               String& outResultJson) const {
  outResultJson = String();
  const String requiresJson = registry_.requiredToolsJsonFor(call.name);
  if (requiresJson.length() == 0) {
    return true;
  }

  SpiJsonDocument doc;
  if (deserializeJson(doc, requiresJson) != DeserializationError::Ok) {
    return true;
  }

  JsonArrayConst requiredTools = doc.as<JsonArrayConst>();
  if (requiredTools.isNull() || requiredTools.size() == 0) {
    return true;
  }

  for (JsonVariantConst item : requiredTools) {
    if (!item.is<const char*>()) {
      continue;
    }
    const String requiredTool = item.as<const char*>();
    if (requiredTool.length() == 0 || hasSuccessfulTool(state, requiredTool)) {
      continue;
    }

    outResultJson = toJsonErrorObject("prerequisite_required",
                                      String("Call ") + requiredTool + " before " + call.name,
                                      requiredTool);
    return false;
  }

  return true;
}

bool AiToolRuntimeExecutor::ensureSeedMessages(AiInvokeRequest& request, String& outErrorMessage) {
  outErrorMessage = String();

  if (request.messageCount > 0) {
    return true;
  }

  if (request.systemPrompt.length() > 0) {
    if (!request.addMessage(AiMessageRole::System, request.systemPrompt)) {
      outErrorMessage = "Unable to append system prompt message";
      return false;
    }
  }

  if (request.prompt.length() > 0) {
    if (!request.addMessage(AiMessageRole::User, request.prompt)) {
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
