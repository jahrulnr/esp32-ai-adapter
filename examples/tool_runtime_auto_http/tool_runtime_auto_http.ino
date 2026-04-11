#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_provider_client.h>
#include <core/ai_tool_runtime.h>
#include <core/ai_tool_runtime_executor.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_transport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiProviderClient g_client(g_transport, g_registry);
AiToolRuntimeRegistry g_toolRegistry;
AiToolRuntimeExecutor g_toolExecutor(g_toolRegistry);

bool hasPlaceholderValue(const char* value) {
  if (value == nullptr) {
    return true;
  }
  return String(value).startsWith("YOUR_");
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiSsid, kWifiPassword);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if ((millis() - startedAt) > kWifiConnectTimeoutMs) {
      Serial.println("WiFi connect timeout");
      return false;
    }
    delay(250);
    Serial.print('.');
  }

  Serial.printf("\nWiFi connected, IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool onGetWeather(const AiToolCall& toolCall,
                  String& outResultJson,
                  String& outErrorMessage,
                  void* userContext) {
  (void)toolCall;
  (void)userContext;

  outErrorMessage = String();
  outResultJson = "{\"city\":\"Jakarta\",\"tempC\":30,\"condition\":\"cloudy\"}";
  return true;
}

void runHttpAutoToolLoop() {
  AiToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get weather by city name";
  tool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";

  String error;
  if (!g_toolRegistry.registerTool(tool, onGetWeather, nullptr, error)) {
    Serial.printf("registerTool failed: %s\n", error.c_str());
    return;
  }

  AiInvokeRequest request;
  request.model = "gpt-4o-mini";
  request.apiKey = kOpenAiApiKey;
  request.baseUrl = "https://api.openai.com/v1";
  request.systemPrompt = "Gunakan tool bila dibutuhkan, lalu jawab singkat.";
  request.prompt = "Berapa perkiraan cuaca Jakarta saat ini?";
  request.enableToolCalls = true;
  request.toolChoice = "auto";

  if (!g_toolRegistry.appendToolDefinitionsToRequest(request, error)) {
    Serial.printf("appendToolDefinitionsToRequest failed: %s\n", error.c_str());
    return;
  }

  AiInvokeResponse response;
  AiToolHttpLoopOptions loopOptions;
  loopOptions.maxRounds = 2;
  loopOptions.continueOnToolError = false;

  const bool ok = g_toolExecutor.invokeHttpWithTools(
      g_client, ProviderKind::OpenAI, request, response, error, loopOptions);

  Serial.printf("invokeHttpWithTools ok=%s status=%u code=%s\n",
                ok ? "true" : "false",
                response.statusCode,
                response.errorCode.c_str());

  if (error.length() > 0) {
    Serial.printf("loop_error=%s\n", error.c_str());
  }

  if (response.text.length() > 0) {
    Serial.println("--- final response ---");
    Serial.println(response.text);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_providers.registerAll(g_registry);

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword) ||
      hasPlaceholderValue(kOpenAiApiKey)) {
    Serial.println("Set WiFi and API key constants before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  runHttpAutoToolLoop();
}

void loop() {
  delay(500);
}
