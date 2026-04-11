#include "core/ai_tool_runtime.h"

namespace ai::provider {

bool AiToolRuntimeRegistry::registerTool(const AiToolDefinition& definition,
                                         AiToolOnCallCallback onCall,
                                         void* userContext,
                                         String& outErrorMessage) {
  outErrorMessage = String();

  if (definition.name.length() == 0) {
    outErrorMessage = "Tool name is required";
    return false;
  }

  if (onCall == nullptr) {
    outErrorMessage = "Tool callback is required";
    return false;
  }

  Entry* existing = findMutableByName(definition.name);
  if (existing != nullptr) {
    existing->definition = definition;
    existing->onCall = onCall;
    existing->userContext = userContext;
    return true;
  }

  for (Entry& entry : entries_) {
    if (entry.used) {
      continue;
    }

    entry.used = true;
    entry.definition = definition;
    entry.onCall = onCall;
    entry.userContext = userContext;
    return true;
  }

  outErrorMessage = "Tool registry is full";
  return false;
}

bool AiToolRuntimeRegistry::unregisterTool(const String& toolName) {
  Entry* entry = findMutableByName(toolName);
  if (entry == nullptr) {
    return false;
  }

  *entry = Entry{};
  return true;
}

bool AiToolRuntimeRegistry::onCall(const AiToolCall& toolCall,
                                   String& outResultJson,
                                   String& outErrorMessage) const {
  outResultJson = String();
  outErrorMessage = String();

  if (toolCall.name.length() == 0) {
    outErrorMessage = "Tool call name is empty";
    return false;
  }

  const Entry* entry = findByName(toolCall.name);
  if (entry == nullptr || entry->onCall == nullptr) {
    outErrorMessage = String("No registered tool handler for: ") + toolCall.name;
    return false;
  }

  return entry->onCall(toolCall, outResultJson, outErrorMessage, entry->userContext);
}

bool AiToolRuntimeRegistry::appendToolDefinitionsToRequest(AiInvokeRequest& request,
                                                           String& outErrorMessage) const {
  outErrorMessage = String();

  for (const Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }

    if (!request.addTool(entry.definition)) {
      outErrorMessage = "Invoke request tool capacity exceeded";
      return false;
    }
  }

  request.enableToolCalls = size() > 0;
  return true;
}

size_t AiToolRuntimeRegistry::size() const {
  size_t count = 0;
  for (const Entry& entry : entries_) {
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

AiToolRuntimeRegistry::Entry* AiToolRuntimeRegistry::findMutableByName(const String& toolName) {
  for (Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }
    if (entry.definition.name.equalsIgnoreCase(toolName)) {
      return &entry;
    }
  }
  return nullptr;
}

const AiToolRuntimeRegistry::Entry* AiToolRuntimeRegistry::findByName(const String& toolName) const {
  for (const Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }
    if (entry.definition.name.equalsIgnoreCase(toolName)) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace ai::provider
