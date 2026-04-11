#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_provider_client.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kApiKey = "YOUR_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_transport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiProviderClient g_client(g_transport, g_registry);

bool g_callStarted = false;

bool hasPlaceholderValue(const char* value) {
  if (value == nullptr) {
    return true;
  }
  return String(value).startsWith("YOUR_");
}

void onStreamChunk(const AiStreamChunk& chunk, void* userContext) {
  (void)userContext;

  if (chunk.textDelta.length() > 0) {
    Serial.print(chunk.textDelta);
  }

  if (chunk.done) {
    Serial.printf("\n[stream_done] reason=%s\n", chunk.doneReason.c_str());
  }
}

void onInvokeDone(const AiInvokeResponse& response, void* userContext) {
  (void)userContext;

  Serial.printf("\n[done] ok=%s status=%u finish=%s code=%s msg=%s\n",
                response.ok ? "true" : "false",
                response.statusCode,
                response.finishReason.c_str(),
                response.errorCode.c_str(),
                response.errorMessage.c_str());
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.printf("Connecting WiFi SSID: %s\n", kWifiSsid);
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

void startAsyncInvoke() {
  AiInvokeRequest request;
  request.model = "gpt-4o-mini";
  request.prompt = "Berikan 3 ringkasan singkat tentang ESP-NOW.";
  request.systemPrompt = "Jawab ringkas dan jelas.";
  request.apiKey = kApiKey;
  request.baseUrl = "https://api.openai.com/v1";
  request.stream = true;
  request.timeoutMs = 45000;
  request.streamCallback = onStreamChunk;

  String error;
  if (!g_client.invokeAsync(ProviderKind::OpenAI, request, onInvokeDone, nullptr, error)) {
    Serial.printf("invokeAsync failed: %s\n", error.c_str());
    return;
  }

  g_callStarted = true;
  Serial.println("invokeAsync accepted, waiting stream...");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  const bool registryOk = g_providers.registerAll(g_registry);
  Serial.printf("registerAll: %s\n", registryOk ? "ok" : "partial/fail");

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword) ||
      hasPlaceholderValue(kApiKey)) {
    Serial.println("Set kWifiSsid, kWifiPassword, and kApiKey before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  startAsyncInvoke();
}

void loop() {
  if (!g_callStarted) {
    delay(200);
    return;
  }

  g_client.poll();
  delay(1);
}
