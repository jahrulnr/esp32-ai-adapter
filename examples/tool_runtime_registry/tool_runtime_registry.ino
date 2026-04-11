#include <Arduino.h>

#include <core/ai_tool_runtime.h>

using namespace ai::provider;

AiToolRuntimeRegistry g_registry;

bool onGetWeather(const AiToolCall& toolCall,
                  String& outResultJson,
                  String& outErrorMessage,
                  void* userContext) {
  (void)userContext;
  (void)toolCall;

  outErrorMessage = String();
  outResultJson = "{\"city\":\"Jakarta\",\"tempC\":30,\"condition\":\"cloudy\"}";
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  AiToolDefinition weather;
  weather.name = "get_weather";
  weather.description = "Get weather by city";
  weather.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";

  String error;
  if (!g_registry.registerTool(weather, onGetWeather, nullptr, error)) {
    Serial.printf("registerTool failed: %s\n", error.c_str());
    return;
  }

  Serial.printf("registered tools: %u\n", static_cast<unsigned>(g_registry.size()));

  AiToolCall call;
  call.id = "call_1";
  call.type = "function";
  call.name = "get_weather";
  call.argumentsJson = "{\"city\":\"Jakarta\"}";

  String resultJson;
  if (!g_registry.onCall(call, resultJson, error)) {
    Serial.printf("onCall failed: %s\n", error.c_str());
    return;
  }

  Serial.printf("onCall result: %s\n", resultJson.c_str());

  const bool removed = g_registry.unregisterTool("get_weather");
  Serial.printf("unregisterTool: %s\n", removed ? "true" : "false");
  Serial.printf("registered tools: %u\n", static_cast<unsigned>(g_registry.size()));
}

void loop() {
  delay(500);
}
