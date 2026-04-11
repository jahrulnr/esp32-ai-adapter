#pragma once

#include "core/ai_provider_registry.h"
#include "providers/claude/claude_provider.h"
#include "providers/llamacpp/llamacpp_provider.h"
#include "providers/ollama/ollama_provider.h"
#include "providers/openai/openai_provider.h"
#include "providers/openrouter/openrouter_provider.h"

namespace ai::provider {

class DefaultProviderBundle {
 public:
  bool registerAll(AiProviderRegistry& registry) const;

 private:
  OpenAiProvider openAi_;
  ClaudeProvider claude_;
  OpenRouterProvider openRouter_;
  OllamaProvider ollama_;
  LlamaCppProvider llamaCpp_;
};

}  // namespace ai::provider
