#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_provider_client.h>
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

void runToolCallSample() {
  AiInvokeRequest request;
  request.model = "gpt-4o-mini";
  request.apiKey = kOpenAiApiKey;
  request.baseUrl = "https://api.openai.com/v1";
  request.prompt = "Cek cuaca Jakarta pakai function tool yang tersedia.";
  request.systemPrompt = "Jika perlu data eksternal, panggil tool yang tersedia.";
  request.enableToolCalls = true;
  request.toolChoice = "auto";

  AiToolDefinition weatherTool;
  weatherTool.name = "get_weather";
  weatherTool.description = "Get current weather by city name";
  weatherTool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";

  if (!request.addTool(weatherTool)) {
    Serial.println("Failed to add tool definition");
    return;
  }

  AiInvokeResponse response;
  const bool ok = g_client.invoke(ProviderKind::OpenAI, request, response);

  Serial.printf("invoke ok=%s status=%u\n", ok ? "true" : "false", response.statusCode);

  if (response.text.length() > 0) {
    Serial.println("assistant_text:");
    Serial.println(response.text);
  }

  Serial.printf("tool_call_count=%u\n", static_cast<unsigned>(response.toolCallCount));
  for (size_t i = 0; i < response.toolCallCount; ++i) {
    const AiToolCall& call = response.toolCalls[i];
    Serial.printf("tool_call[%u] id=%s name=%s args=%s\n",
                  static_cast<unsigned>(i),
                  call.id.c_str(),
                  call.name.c_str(),
                  call.argumentsJson.c_str());
  }

  if (!ok) {
    Serial.printf("error=%s (%s)\n", response.errorCode.c_str(), response.errorMessage.c_str());
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

  runToolCallSample();
}

void loop() {
  delay(500);
}
