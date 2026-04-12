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
  bool setMax(size_t maxEntries);
  size_t max() const;

  bool registerTool(const AiToolDefinition& definition,
                    AiToolOnCallCallback onCall,
                    void* userContext,
                    String& outErrorMessage);

  bool unregisterTool(const String& toolName);

  bool onCall(const AiToolCall& toolCall,
              String& outResultJson,
              String& outErrorMessage) const;

  bool appendToolDefinitionsToRequest(AiInvokeRequest& request,
                                      String& outErrorMessage) const;

  size_t size() const;

 private:
  struct Entry {
    bool used = false;
    AiToolDefinition definition;
    AiToolOnCallCallback onCall = nullptr;
    void* userContext = nullptr;
  };

  static constexpr size_t kMaxEntries = kAiMaxTools;
  size_t activeMaxEntries_ = kAiMaxTools;
  std::array<Entry, kMaxEntries> entries_{};

  Entry* findMutableByName(const String& toolName);
  const Entry* findByName(const String& toolName) const;
};

}  // namespace ai::provider
