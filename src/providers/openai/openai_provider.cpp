#include "providers/openai/openai_provider.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include "core/ai_http_request_body_utils.h"
#include "core/url_utils.h"
#include "providers/openai/openai_compat_utils.h"

namespace ai::provider {

namespace {
constexpr const char* kDefaultBaseUrl = "https://api.openai.com/v1";
constexpr const char* kChatPath = "/chat/completions";
}

ProviderKind OpenAiProvider::kind() const {
  return ProviderKind::OpenAI;
}

const char* OpenAiProvider::id() const {
  return "openai";
}

bool OpenAiProvider::buildHttpRequest(const AiInvokeRequest& request,
                                      AiHttpRequest& outHttpRequest,
                                      String& outErrorMessage) const {
  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing OpenAI API key";
    return false;
  }

  SpiJsonDocument body;
  if (!buildOpenAiStyleMessages(request, true, body)) {
    outErrorMessage = "Missing required fields: model and prompt";
    return false;
  }

  if (request.maxTokens > 0) {
    body.remove("max_tokens");
    body["max_completion_tokens"] = request.maxTokens;
  }

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "POST";
  outHttpRequest.url = joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");
  outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);

  if (request.requestId.length() > 0) {
    outHttpRequest.addHeader("X-Client-Request-Id", request.requestId);
  }

  if (!applyJsonBodyToHttpRequest(body, request.bodySpool, outHttpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_body_build_failed";
    }
    return false;
  }

  return true;
}

bool OpenAiProvider::parseHttpResponse(const AiHttpResponse& httpResponse,
                                       AiInvokeResponse& outResponse) const {
  return parseOpenAiStyleResponse(httpResponse, outResponse);
}

const IAiRealtimeProviderAdapter* OpenAiProvider::asRealtimeAdapter() const {
  return &realtimeAdapter_;
}

const IAiAudioProviderAdapter* OpenAiProvider::asAudioAdapter() const {
  return &audioAdapter_;
}

}  // namespace ai::provider
