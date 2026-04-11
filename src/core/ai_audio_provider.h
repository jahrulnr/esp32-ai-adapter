#pragma once

#include "core/ai_provider.h"

namespace ai::provider {

class IAiAudioProviderAdapter {
 public:
  virtual ~IAiAudioProviderAdapter() = default;

  virtual bool supportsSpeechToText() const {
    return false;
  }

  virtual bool supportsTextToSpeech() const {
    return false;
  }

  virtual bool supportsSpeechToTextStreaming() const {
    return false;
  }

  virtual bool supportsTextToSpeechStreaming() const {
    return false;
  }

  virtual bool buildSpeechToTextRequest(const AiSpeechToTextRequest& request,
                                        AiHttpRequest& outHttpRequest,
                                        String& outErrorMessage) const = 0;

  virtual bool parseSpeechToTextResponse(const AiHttpResponse& httpResponse,
                                         AiSpeechToTextResponse& outResponse) const = 0;

  virtual bool buildTextToSpeechRequest(const AiTextToSpeechRequest& request,
                                        AiHttpRequest& outHttpRequest,
                                        String& outErrorMessage) const = 0;

  virtual bool parseTextToSpeechResponse(const AiHttpResponse& httpResponse,
                                         AiTextToSpeechResponse& outResponse) const = 0;
};

}  // namespace ai::provider
