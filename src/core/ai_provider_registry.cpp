#include "core/ai_provider_registry.h"

namespace ai::provider {

bool AiProviderRegistry::add(const IAiProviderAdapter& adapter) {
  if (providerCount_ >= providers_.size()) {
    return false;
  }

  if (find(adapter.kind()) != nullptr || findById(adapter.id()) != nullptr) {
    return false;
  }

  providers_[providerCount_++] = &adapter;
  return true;
}

const IAiProviderAdapter* AiProviderRegistry::find(ProviderKind kind) const {
  for (size_t i = 0; i < providerCount_; ++i) {
    if (providers_[i] != nullptr && providers_[i]->kind() == kind) {
      return providers_[i];
    }
  }
  return nullptr;
}

const IAiProviderAdapter* AiProviderRegistry::findById(const String& providerId) const {
  for (size_t i = 0; i < providerCount_; ++i) {
    if (providers_[i] == nullptr) {
      continue;
    }
    if (providerId.equalsIgnoreCase(providers_[i]->id())) {
      return providers_[i];
    }
  }
  return nullptr;
}

const IAiAudioProviderAdapter* AiProviderRegistry::findAudio(ProviderKind kind) const {
  const IAiProviderAdapter* provider = find(kind);
  if (provider == nullptr) {
    return nullptr;
  }
  return provider->asAudioAdapter();
}

const IAiAudioProviderAdapter* AiProviderRegistry::findAudioById(const String& providerId) const {
  const IAiProviderAdapter* provider = findById(providerId);
  if (provider == nullptr) {
    return nullptr;
  }
  return provider->asAudioAdapter();
}

}  // namespace ai::provider
