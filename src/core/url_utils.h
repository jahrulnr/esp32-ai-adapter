#pragma once

#include <Arduino.h>

namespace ai::provider {

String joinUrl(const String& baseUrl, const char* path);

}  // namespace ai::provider
