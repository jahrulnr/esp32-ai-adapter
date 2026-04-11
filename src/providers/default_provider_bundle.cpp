#include "providers/default_provider_bundle.h"

namespace ai::provider {

bool DefaultProviderBundle::registerAll(AiProviderRegistry& registry) const {
  bool ok = true;
  ok = registry.add(openAi_) && ok;
  ok = registry.add(claude_) && ok;
  ok = registry.add(openRouter_) && ok;
  ok = registry.add(ollama_) && ok;
  ok = registry.add(llamaCpp_) && ok;
  return ok;
}

}  // namespace ai::provider
