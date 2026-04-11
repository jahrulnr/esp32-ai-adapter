#pragma once

#include "core/ai_audio_provider.h"

namespace ai::provider {

class OpenAiAudioAdapter : public IAiAudioProviderAdapter {
 public:
  bool supportsSpeechToText() const override;
  bool supportsTextToSpeech() const override;
  bool supportsSpeechToTextStreaming() const override;
  bool supportsTextToSpeechStreaming() const override;

  bool buildSpeechToTextRequest(const AiSpeechToTextRequest& request,
                                AiHttpRequest& outHttpRequest,
                                String& outErrorMessage) const override;

  bool parseSpeechToTextResponse(const AiHttpResponse& httpResponse,
                                 AiSpeechToTextResponse& outResponse) const override;

  bool buildTextToSpeechRequest(const AiTextToSpeechRequest& request,
                                AiHttpRequest& outHttpRequest,
                                String& outErrorMessage) const override;

  bool parseTextToSpeechResponse(const AiHttpResponse& httpResponse,
                                 AiTextToSpeechResponse& outResponse) const override;
};

}  // namespace ai::provider