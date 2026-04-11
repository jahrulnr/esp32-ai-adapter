#pragma once

#include "core/ai_provider_registry.h"
#include "core/ai_transport.h"

namespace ai::provider {

using AiSpeechToTextDoneCallback =
    void (*)(const AiSpeechToTextResponse& response, void* userContext);
using AiTextToSpeechDoneCallback =
    void (*)(const AiTextToSpeechResponse& response, void* userContext);

class IAiAudioProviderAdapter;

class AiAudioClient {
 public:
  AiAudioClient(IAiTransport& transport, const AiProviderRegistry& registry);

  bool transcribe(ProviderKind kind,
                  const AiSpeechToTextRequest& request,
                  AiSpeechToTextResponse& outResponse) const;

  bool transcribe(const String& providerId,
                  const AiSpeechToTextRequest& request,
                  AiSpeechToTextResponse& outResponse) const;

  bool synthesize(ProviderKind kind,
                  const AiTextToSpeechRequest& request,
                  AiTextToSpeechResponse& outResponse) const;

  bool synthesize(const String& providerId,
                  const AiTextToSpeechRequest& request,
                  AiTextToSpeechResponse& outResponse) const;

  bool transcribeAsync(ProviderKind kind,
                       const AiSpeechToTextRequest& request,
                       AiSpeechToTextDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  bool transcribeAsync(const String& providerId,
                       const AiSpeechToTextRequest& request,
                       AiSpeechToTextDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  bool synthesizeAsync(ProviderKind kind,
                       const AiTextToSpeechRequest& request,
                       AiTextToSpeechDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  bool synthesizeAsync(const String& providerId,
                       const AiTextToSpeechRequest& request,
                       AiTextToSpeechDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  void poll();
  bool isBusy() const;
  void cancel();

 private:
  enum class PendingOperation : uint8_t {
    None = 0,
    SpeechToText,
    TextToSpeech,
  };

  bool transcribe(const IAiAudioProviderAdapter* adapter,
                  const AiSpeechToTextRequest& request,
                  AiSpeechToTextResponse& outResponse) const;

  bool synthesize(const IAiAudioProviderAdapter* adapter,
                  const AiTextToSpeechRequest& request,
                  AiTextToSpeechResponse& outResponse) const;

  bool transcribeAsync(const IAiAudioProviderAdapter* adapter,
                       const AiSpeechToTextRequest& request,
                       AiSpeechToTextDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  bool synthesizeAsync(const IAiAudioProviderAdapter* adapter,
                       const AiTextToSpeechRequest& request,
                       AiTextToSpeechDoneCallback doneCallback,
                       void* userContext,
                       String& outErrorMessage);

  static void onTransportChunkStatic(const AiStreamChunk& chunk, void* userContext);
  static void onTransportDoneStatic(const AiHttpResponse& response, void* userContext);

  void onTransportChunk(const AiStreamChunk& chunk);
  void onTransportDone(const AiHttpResponse& response);

  void resetPending();

  IAiTransport& transport_;
  const AiProviderRegistry& registry_;

  struct PendingContext {
    PendingOperation operation = PendingOperation::None;
    bool active = false;
    const IAiAudioProviderAdapter* adapter = nullptr;

    AiSpeechToTextRequest sttRequest;
    AiSpeechToTextDoneCallback sttDoneCallback = nullptr;
    void* sttDoneUserContext = nullptr;

    AiTextToSpeechRequest ttsRequest;
    AiTextToSpeechDoneCallback ttsDoneCallback = nullptr;
    void* ttsDoneUserContext = nullptr;
  };

  PendingContext pending_{};
};

}  // namespace ai::provider
