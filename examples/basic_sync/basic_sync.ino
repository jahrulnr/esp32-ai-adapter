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

void runBasicSyncCall() {
  AiInvokeRequest request;
  request.model = "gpt-4o-mini";
  request.prompt = "Jelaskan ESP-NOW dalam 3 poin singkat.";
  request.systemPrompt = "Jawab ringkas dalam Bahasa Indonesia.";
  request.apiKey = kOpenAiApiKey;
  request.baseUrl = "https://api.openai.com/v1";
  request.timeoutMs = 45000;

  AiInvokeResponse response;
  const bool ok = g_client.invoke(ProviderKind::OpenAI, request, response);

  Serial.printf("invoke ok=%s status=%u code=%s\n",
                ok ? "true" : "false",
                response.statusCode,
                response.errorCode.c_str());

  if (response.text.length() > 0) {
    Serial.println("--- model response ---");
    Serial.println(response.text);
  }

  if (response.errorMessage.length() > 0) {
    Serial.printf("error=%s\n", response.errorMessage.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  const bool registryOk = g_providers.registerAll(g_registry);
  Serial.printf("registerAll: %s\n", registryOk ? "ok" : "partial/fail");

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword) ||
      hasPlaceholderValue(kOpenAiApiKey)) {
    Serial.println("Set WiFi and API key constants before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  runBasicSyncCall();
}

void loop() {
  delay(500);
}
