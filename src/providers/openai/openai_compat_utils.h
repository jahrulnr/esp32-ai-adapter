#pragma once

#include <SpiJsonDocument.h>

#include "core/ai_provider_types.h"

namespace ai::provider {

bool buildOpenAiStyleMessages(const AiInvokeRequest& request,
                              bool useDeveloperRole,
                              SpiJsonDocument& outBody);

bool parseOpenAiStyleResponse(const AiHttpResponse& httpResponse,
                              AiInvokeResponse& outResponse);

}  // namespace ai::provider
