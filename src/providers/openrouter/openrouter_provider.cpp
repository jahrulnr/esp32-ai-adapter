#include "providers/openrouter/openrouter_provider.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include "core/url_utils.h"
#include "providers/openai/openai_compat_utils.h"

namespace ai::provider {

namespace {
constexpr const char* kDefaultBaseUrl = "https://openrouter.ai/api/v1";
constexpr const char* kChatPath = "/chat/completions";
}

ProviderKind OpenRouterProvider::kind() const {
  return ProviderKind::OpenRouter;
}

const char* OpenRouterProvider::id() const {
  return "openrouter";
}

bool OpenRouterProvider::buildHttpRequest(const AiInvokeRequest& request,
                                          AiHttpRequest& outHttpRequest,
                                          String& outErrorMessage) const {
  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing OpenRouter API key";
    return false;
  }

  SpiJsonDocument body;
  if (!buildOpenAiStyleMessages(request, false, body)) {
    outErrorMessage = "Missing required fields: model and prompt";
    return false;
  }

  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");
  outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);

  if (request.httpReferer.length() > 0) {
    outHttpRequest.addHeader("HTTP-Referer", request.httpReferer);
  }
  if (request.appTitle.length() > 0) {
    outHttpRequest.addHeader("X-OpenRouter-Title", request.appTitle);
  }

  serializeJson(body, outHttpRequest.body);
  return true;
}

bool OpenRouterProvider::parseHttpResponse(const AiHttpResponse& httpResponse,
                                           AiInvokeResponse& outResponse) const {
  return parseOpenAiStyleResponse(httpResponse, outResponse);
}

const IAiAudioProviderAdapter* OpenRouterProvider::asAudioAdapter() const {
  return &audioAdapter_;
}

}  // namespace ai::provider
