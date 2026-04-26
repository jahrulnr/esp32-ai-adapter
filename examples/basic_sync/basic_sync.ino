#include <Arduino.h>
#include <WiFi.h>

#include <AiProviderKit.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;
AiProviderKit g_ai;

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
  AiInvokeResponse response;
  String error;
  const bool ok = g_ai.llm(
      ProviderKind::OpenAI, "Jelaskan ESP-NOW dalam 3 poin singkat.", false, response, error);

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
  if (error.length() > 0) {
    Serial.printf("facade_error=%s\n", error.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_ai.setApiKey(ProviderKind::OpenAI, kOpenAiApiKey);
  g_ai.setLlmModel(ProviderKind::OpenAI, "gpt-4o-mini");
  g_ai.defaults().llmSystemPrompt = "Jawab ringkas dalam Bahasa Indonesia.";

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
