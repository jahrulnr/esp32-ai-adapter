#pragma once

#include <array>

#include "core/ai_provider_types.h"

namespace ai::provider {

using AiToolOnCallCallback =
    bool (*)(const AiToolCall& toolCall,
             String& outResultJson,
             String& outErrorMessage,
             void* userContext);

class AiToolRuntimeRegistry {
 public:
  enum class ToolExposure : uint8_t {
    Initial = 0,
    Discoverable,
  };

  enum class AppendMode : uint8_t {
    All = 0,
    InitialOnly,
    DiscoverableOnly,
  };

  bool setMax(size_t maxEntries);
  size_t max() const;

  bool registerTool(const AiToolDefinition& definition,
                    AiToolOnCallCallback onCall,
                    void* userContext,
                    String& outErrorMessage,
                    ToolExposure exposure = ToolExposure::Initial);

  bool unregisterTool(const String& toolName);

  bool onCall(const AiToolCall& toolCall,
              String& outResultJson,
              String& outErrorMessage) const;

  bool appendToolDefinitionsToRequest(AiInvokeRequest& request,
                                      String& outErrorMessage,
                                      AppendMode mode = AppendMode::All) const;

  bool appendDiscoveredToolDefinitionsToRequest(const String& query,
                                                size_t maxResults,
                                                AiInvokeRequest& request,
                                                String& outErrorMessage) const;

  size_t size() const;
  String requiredToolsJsonFor(const String& toolName) const;

 private:
  struct Entry {
    bool used = false;
    AiToolDefinition definition;
    AiToolOnCallCallback onCall = nullptr;
    void* userContext = nullptr;
    ToolExposure exposure = ToolExposure::Initial;
  };

  static constexpr size_t kMaxEntries = kAiMaxTools;
  size_t activeMaxEntries_ = kAiMaxTools;
  std::array<Entry, kMaxEntries> entries_{};

  Entry* findMutableByName(const String& toolName);
  const Entry* findByName(const String& toolName) const;
};

}  // namespace ai::provider
