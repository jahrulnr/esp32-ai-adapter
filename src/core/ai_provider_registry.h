#pragma once

#include <array>

#include "core/ai_provider.h"
#include "core/ai_audio_provider.h"

namespace ai::provider {

class AiProviderRegistry {
 public:
  bool add(const IAiProviderAdapter& adapter);
  const IAiProviderAdapter* find(ProviderKind kind) const;
  const IAiProviderAdapter* findById(const String& providerId) const;
  const IAiAudioProviderAdapter* findAudio(ProviderKind kind) const;
  const IAiAudioProviderAdapter* findAudioById(const String& providerId) const;

 private:
  static constexpr size_t kMaxProviders = 10;
  std::array<const IAiProviderAdapter*, kMaxProviders> providers_{};
  size_t providerCount_ = 0;
};

}  // namespace ai::provider
