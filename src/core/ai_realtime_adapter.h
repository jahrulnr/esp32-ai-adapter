#pragma once

#include "core/ai_provider.h"

namespace ai::provider {

class IAiRealtimeProviderAdapter : public IAiProviderAdapter {
 public:
  const IAiRealtimeProviderAdapter* asRealtimeAdapter() const override {
    return this;
  }

  bool buildHttpRequest(const AiInvokeRequest& request,
                        AiHttpRequest& outHttpRequest,
                        String& outErrorMessage) const override {
    (void)request;
    (void)outHttpRequest;
    outErrorMessage = "HTTP invoke is not supported by this realtime adapter";
    return false;
  }

  bool parseHttpResponse(const AiHttpResponse& httpResponse,
                         AiInvokeResponse& outResponse) const override {
    (void)httpResponse;
    outResponse.ok = false;
    outResponse.errorCode = "http_not_supported";
    outResponse.errorMessage = "HTTP parse is not supported by this realtime adapter";
    return false;
  }

  virtual bool supportsRealtime() const {
    return false;
  }

  virtual bool buildRealtimeSessionRequest(const AiInvokeRequest& request,
                                           const AiRealtimeSessionConfig& sessionConfig,
                                           AiHttpRequest& outHttpRequest,
                                           String& outErrorMessage) const {
    (void)request;
    outHttpRequest = AiHttpRequest{};
    outHttpRequest.method = "GET";
    outHttpRequest.url = sessionConfig.sessionUrl;
    outErrorMessage = "Realtime session request builder is not implemented";
    return false;
  }

  virtual bool buildRealtimeTextMessage(const String& text,
                                        AiRealtimeMessage& outMessage,
                                        String& outErrorMessage) const {
    if (text.length() == 0) {
      outErrorMessage = "Text message is empty";
      return false;
    }

    outMessage = AiRealtimeMessage{};
    outMessage.kind = AiRealtimeEventKind::ProviderEvent;
    outMessage.eventType = "input.text";
    outMessage.text = text;
    return true;
  }

  virtual bool buildRealtimeAudioMessage(const String& audioBase64,
                                         const String& mimeType,
                                         AiRealtimeMessage& outMessage,
                                         String& outErrorMessage) const {
    if (audioBase64.length() == 0) {
      outErrorMessage = "Audio message is empty";
      return false;
    }

    outMessage = AiRealtimeMessage{};
    outMessage.kind = AiRealtimeEventKind::AudioDelta;
    outMessage.eventType = "input.audio";
    outMessage.audioBase64 = audioBase64;
    outMessage.mimeType = mimeType;
    return true;
  }

  virtual bool buildRealtimeToolResultMessage(const String& toolCallId,
                                              const String& toolName,
                                              const String& toolResultJson,
                                              AiRealtimeMessage& outMessage,
                                              String& outErrorMessage) const {
    if (toolCallId.length() == 0 || toolName.length() == 0) {
      outErrorMessage = "toolCallId and toolName are required";
      return false;
    }

    outMessage = AiRealtimeMessage{};
    outMessage.kind = AiRealtimeEventKind::ProviderEvent;
    outMessage.eventType = "tool.result";
    outMessage.toolCallId = toolCallId;
    outMessage.toolName = toolName;
    outMessage.payload = toolResultJson;
    return true;
  }

  virtual bool parseRealtimeMessage(const AiRealtimeMessage& message,
                                    AiRealtimeParsedEvent& outEvent,
                                    String& outErrorMessage) const {
    (void)outErrorMessage;
    outEvent = AiRealtimeParsedEvent{};
    outEvent.kind = message.kind;
    outEvent.providerEventType = message.eventType;
    outEvent.payloadJson = message.payload;
    outEvent.textDelta = message.text;
    outEvent.audioBase64 = message.audioBase64;
    outEvent.audioMimeType = message.mimeType;
    outEvent.done = message.done;
    outEvent.doneReason = message.doneReason;
    return true;
  }
};

}  // namespace ai::provider
