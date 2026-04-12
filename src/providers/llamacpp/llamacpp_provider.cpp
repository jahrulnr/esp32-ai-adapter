#include "providers/llamacpp/llamacpp_provider.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include "core/ai_http_request_body_utils.h"
#include "core/url_utils.h"
#include "providers/openai/openai_compat_utils.h"

namespace ai::provider {

namespace {
constexpr const char* kDefaultBaseUrl = "http://127.0.0.1:8080/v1";
constexpr const char* kChatPath = "/chat/completions";
}

ProviderKind LlamaCppProvider::kind() const {
  return ProviderKind::LlamaCpp;
}

const char* LlamaCppProvider::id() const {
  return "llamacpp";
}

bool LlamaCppProvider::buildHttpRequest(const AiInvokeRequest& request,
                                        AiHttpRequest& outHttpRequest,
                                        String& outErrorMessage) const {
  SpiJsonDocument body;
  if (!buildOpenAiStyleMessages(request, false, body)) {
    outErrorMessage = "Missing required fields: model and prompt";
    return false;
  }

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");

  if (request.apiKey.length() > 0) {
    outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);
  }

  if (!applyJsonBodyToHttpRequest(body, request.bodySpool, outHttpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_body_build_failed";
    }
    return false;
  }

  return true;
}

bool LlamaCppProvider::parseHttpResponse(const AiHttpResponse& httpResponse,
                                         AiInvokeResponse& outResponse) const {
  return parseOpenAiStyleResponse(httpResponse, outResponse);
}

}  // namespace ai::provider
