#include "core/ai_tool_runtime.h"

#include <ArduinoJson.h>
#include <cctype>

namespace ai::provider {
namespace {

const char* kToolFinderName = "tools.find";
const char* kToolMapName = "tools.map";

AiToolDefinition toolMapDefinition() {
  AiToolDefinition definition;
  definition.name = kToolMapName;
  definition.description =
      "List runtime tool domains and compact workflows. Use first when the relevant FieldHub tool domain is unclear.";
  definition.inputSchemaJson = "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{}}";
  return definition;
}

AiToolDefinition toolFinderDefinition() {
  AiToolDefinition definition;
  definition.name = kToolFinderName;
  definition.description =
      "Find runtime tools by intent. Use before calling device/runtime tools when exact tool names are unknown.";
  definition.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"},\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":8}},\"required\":[\"query\"]}";
  return definition;
}

String toolDomainName(const AiToolDefinition& definition) {
  if (definition.domain.length() > 0) {
    return definition.domain;
  }
  const int dotIndex = definition.name.indexOf('.');
  if (dotIndex <= 0) {
    return String();
  }
  return definition.name.substring(0, static_cast<size_t>(dotIndex));
}

String lowerText(const String& value) {
  String out = value;
  out.toLowerCase();
  return out;
}

bool matchesQuery(const AiToolDefinition& definition, const String& query) {
  if (query.length() == 0) {
    return true;
  }
  const String haystack = lowerText(definition.name + " " + definition.description);
  const String needle = lowerText(query);
  if (haystack.indexOf(needle) >= 0) {
    return true;
  }

  size_t tokenStart = 0;
  while (tokenStart < needle.length()) {
    while (tokenStart < needle.length() && !std::isalnum(static_cast<unsigned char>(needle[tokenStart]))) {
      ++tokenStart;
    }
    size_t tokenEnd = tokenStart;
    while (tokenEnd < needle.length() && std::isalnum(static_cast<unsigned char>(needle[tokenEnd]))) {
      ++tokenEnd;
    }
    if (tokenEnd > tokenStart) {
      const String token = needle.substring(tokenStart, tokenEnd);
      if (token.length() >= 3 && haystack.indexOf(token) >= 0) {
        return true;
      }
    }
    tokenStart = tokenEnd + 1;
  }
  return false;
}

bool requestHasToolName(const AiInvokeRequest& request, const String& toolName) {
  for (size_t i = 0; i < request.toolCount; ++i) {
    if (request.tools[i].name.equalsIgnoreCase(toolName)) {
      return true;
    }
  }
  return false;
}

bool jsonArrayHasText(JsonArrayConst values, const String& text) {
  for (JsonVariantConst value : values) {
    if (value.is<const char*>() && text.equalsIgnoreCase(value.as<const char*>())) {
      return true;
    }
  }
  return false;
}

uint8_t parseLimit(const String& argsJson, uint8_t fallback) {
  JsonDocument doc;
  if (deserializeJson(doc, argsJson)) {
    return fallback;
  }
  const uint32_t raw = doc["limit"].is<uint32_t>() ? doc["limit"].as<uint32_t>() : fallback;
  if (raw == 0) {
    return 1;
  }
  if (raw > 8U) {
    return 8;
  }
  return static_cast<uint8_t>(raw);
}

String parseQuery(const String& argsJson) {
  JsonDocument doc;
  if (deserializeJson(doc, argsJson)) {
    return "";
  }
  String query = doc["query"].is<const char*>() ? doc["query"].as<const char*>() : "";
  query.trim();
  return query;
}

}  // namespace

bool AiToolRuntimeRegistry::setMax(size_t maxEntries) {
  if (maxEntries == 0 || maxEntries > kMaxEntries) {
    return false;
  }

  if (maxEntries < activeMaxEntries_) {
    for (size_t i = maxEntries; i < entries_.size(); ++i) {
      entries_[i] = Entry{};
    }
  }

  activeMaxEntries_ = maxEntries;
  return true;
}

size_t AiToolRuntimeRegistry::max() const {
  return activeMaxEntries_;
}

bool AiToolRuntimeRegistry::registerTool(const AiToolDefinition& definition,
                                         AiToolOnCallCallback onCall,
                                         void* userContext,
                                         String& outErrorMessage,
                                         ToolExposure exposure) {
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
    existing->exposure = exposure;
    return true;
  }

  for (size_t i = 0; i < activeMaxEntries_; ++i) {
    Entry& entry = entries_[i];
    if (entry.used) {
      continue;
    }

    entry.used = true;
    entry.definition = definition;
    entry.onCall = onCall;
    entry.userContext = userContext;
    entry.exposure = exposure;
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

  if (toolCall.name.equalsIgnoreCase(kToolMapName)) {
    JsonDocument doc;
    JsonArray domains = doc["domains"].to<JsonArray>();

    JsonArray seen = doc["seenDomains"].to<JsonArray>();
    for (size_t i = 0; i < activeMaxEntries_; ++i) {
      const Entry& source = entries_[i];
      if (!source.used || source.exposure != ToolExposure::Discoverable) {
        continue;
      }
      const String domainName = toolDomainName(source.definition);
      if (domainName.length() == 0 || jsonArrayHasText(seen, domainName)) {
        continue;
      }
      seen.add(domainName);

      JsonObject item = domains.add<JsonObject>();
      item["name"] = domainName;
      if (source.definition.workflow.length() > 0) {
        item["workflow"] = source.definition.workflow;
      }
      if (source.definition.findQuery.length() > 0) {
        item["findQuery"] = source.definition.findQuery;
      }
      JsonArray tools = item["tools"].to<JsonArray>();

      for (size_t j = 0; j < activeMaxEntries_; ++j) {
        const Entry& entry = entries_[j];
        if (!entry.used || entry.exposure != ToolExposure::Discoverable) {
          continue;
        }
        if (!toolDomainName(entry.definition).equalsIgnoreCase(domainName)) {
          continue;
        }
        tools.add(entry.definition.name);
      }
    }

    doc.remove("seenDomains");
    doc["policy"] = "Use skills.find for how-to guidance, then tools.find for concrete tool schemas.";
    serializeJson(doc, outResultJson);
    return true;
  }

  if (toolCall.name.equalsIgnoreCase(kToolFinderName)) {
    const String query = parseQuery(toolCall.argumentsJson);
    const uint8_t limit = parseLimit(toolCall.argumentsJson, 5);
    JsonDocument doc;
    JsonArray tools = doc["tools"].to<JsonArray>();
    size_t emitted = 0;

    for (size_t i = 0; i < activeMaxEntries_ && emitted < limit; ++i) {
      const Entry& entry = entries_[i];
      if (!entry.used || entry.exposure != ToolExposure::Discoverable) {
        continue;
      }
      if (!matchesQuery(entry.definition, query)) {
        continue;
      }
      JsonObject item = tools.add<JsonObject>();
      item["name"] = entry.definition.name;
      item["description"] = entry.definition.description;
      ++emitted;
    }

    if (emitted == 0) {
      for (size_t i = 0; i < activeMaxEntries_ && emitted < limit; ++i) {
        const Entry& entry = entries_[i];
        if (!entry.used || entry.exposure != ToolExposure::Discoverable) {
          continue;
        }
        JsonObject item = tools.add<JsonObject>();
        item["name"] = entry.definition.name;
        item["description"] = entry.definition.description;
        ++emitted;
      }
    }

    doc["query"] = query;
    serializeJson(doc, outResultJson);
    return true;
  }

  const Entry* entry = findByName(toolCall.name);
  if (entry == nullptr || entry->onCall == nullptr) {
    outErrorMessage = String("No registered tool handler for: ") + toolCall.name;
    return false;
  }

  return entry->onCall(toolCall, outResultJson, outErrorMessage, entry->userContext);
}

bool AiToolRuntimeRegistry::appendToolDefinitionsToRequest(AiInvokeRequest& request,
                                                           String& outErrorMessage,
                                                           AppendMode mode) const {
  outErrorMessage = String();

  if (mode == AppendMode::All || mode == AppendMode::InitialOnly) {
    if (!requestHasToolName(request, kToolMapName) && !request.addTool(toolMapDefinition())) {
      outErrorMessage = "Invoke request tool capacity exceeded";
      return false;
    }
    if (!requestHasToolName(request, kToolFinderName) && !request.addTool(toolFinderDefinition())) {
      outErrorMessage = "Invoke request tool capacity exceeded";
      return false;
    }
  }

  for (size_t i = 0; i < activeMaxEntries_; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.used) {
      continue;
    }

    if (mode == AppendMode::InitialOnly && entry.exposure != ToolExposure::Initial) {
      continue;
    }
    if (mode == AppendMode::DiscoverableOnly && entry.exposure != ToolExposure::Discoverable) {
      continue;
    }

    if (requestHasToolName(request, entry.definition.name)) {
      continue;
    }

    if (!request.addTool(entry.definition)) {
      outErrorMessage = "Invoke request tool capacity exceeded";
      return false;
    }
  }

  request.enableToolCalls = request.toolCount > 0;
  return true;
}

bool AiToolRuntimeRegistry::appendDiscoveredToolDefinitionsToRequest(const String& query,
                                                                     size_t maxResults,
                                                                     AiInvokeRequest& request,
                                                                     String& outErrorMessage) const {
  outErrorMessage = String();
  const size_t limit = maxResults == 0 ? 5 : maxResults;
  size_t appended = 0;

  for (size_t i = 0; i < activeMaxEntries_ && appended < limit; ++i) {
    const Entry& entry = entries_[i];
    if (!entry.used || entry.exposure != ToolExposure::Discoverable) {
      continue;
    }
    if (!matchesQuery(entry.definition, query)) {
      continue;
    }
    if (requestHasToolName(request, entry.definition.name)) {
      continue;
    }
    if (!request.addTool(entry.definition)) {
      outErrorMessage = "Invoke request tool capacity exceeded";
      return false;
    }
    ++appended;
  }

  if (appended == 0) {
    for (size_t i = 0; i < activeMaxEntries_ && appended < limit; ++i) {
      const Entry& entry = entries_[i];
      if (!entry.used || entry.exposure != ToolExposure::Discoverable) {
        continue;
      }
      if (requestHasToolName(request, entry.definition.name)) {
        continue;
      }
      if (!request.addTool(entry.definition)) {
        outErrorMessage = "Invoke request tool capacity exceeded";
        return false;
      }
      ++appended;
    }
  }

  request.enableToolCalls = request.toolCount > 0;
  return true;
}

size_t AiToolRuntimeRegistry::size() const {
  size_t count = 0;
  for (size_t i = 0; i < activeMaxEntries_; ++i) {
    const Entry& entry = entries_[i];
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

bool AiToolRuntimeRegistry::hasTool(const String& toolName) const {
  return findByName(toolName) != nullptr;
}

String AiToolRuntimeRegistry::requiredToolsJsonFor(const String& toolName) const {
  const Entry* entry = findByName(toolName);
  if (entry == nullptr) {
    return String();
  }
  return entry->definition.requiresJson;
}

AiToolRuntimeRegistry::Entry* AiToolRuntimeRegistry::findMutableByName(const String& toolName) {
  for (size_t i = 0; i < activeMaxEntries_; ++i) {
    Entry& entry = entries_[i];
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
  for (size_t i = 0; i < activeMaxEntries_; ++i) {
    const Entry& entry = entries_[i];
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
