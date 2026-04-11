#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_provider_client.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";

constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";
constexpr const char* kOpenRouterApiKey = "YOUR_OPENROUTER_API_KEY";

constexpr const char* kOpenAiBaseUrl = "https://api.openai.com/v1";
constexpr const char* kOpenRouterBaseUrl = "https://openrouter.ai/api/v1";
constexpr const char* kOllamaBaseUrl = "http://192.168.1.10:11434";
constexpr const char* kLlamaCppBaseUrl = "http://192.168.1.10:8080/v1";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_transport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiProviderClient g_client(g_transport, g_registry);

struct ProviderAttempt {
  ProviderKind kind;
  const char* label;
  const char* model;
  const char* baseUrl;
  const char* apiKey;
  bool requireApiKey;
};

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

bool runAttempt(const ProviderAttempt& attempt, const String& prompt, AiInvokeResponse& outResponse) {
  if (attempt.requireApiKey && hasPlaceholderValue(attempt.apiKey)) {
    Serial.printf("skip %s (missing API key)\n", attempt.label);
    return false;
  }

  AiInvokeRequest request;
  request.model = attempt.model;
  request.prompt = prompt;
  request.systemPrompt = "Jawab singkat dan jelas.";
  request.baseUrl = attempt.baseUrl;
  request.timeoutMs = 30000;
  if (!hasPlaceholderValue(attempt.apiKey)) {
    request.apiKey = attempt.apiKey;
  }

  Serial.printf("trying provider: %s\n", attempt.label);
  const bool ok = g_client.invoke(attempt.kind, request, outResponse);

  Serial.printf("  result ok=%s status=%u code=%s\n",
                ok ? "true" : "false",
                outResponse.statusCode,
                outResponse.errorCode.c_str());
  return ok;
}

void runMultiModelSample() {
  const ProviderAttempt attempts[] = {
      {ProviderKind::OpenAI, "openai", "gpt-4o-mini", kOpenAiBaseUrl, kOpenAiApiKey, true},
      {ProviderKind::OpenRouter,
       "openrouter",
       "openai/gpt-4o-mini",
       kOpenRouterBaseUrl,
       kOpenRouterApiKey,
       true},
      {ProviderKind::Ollama, "ollama", "llama3.1:8b", kOllamaBaseUrl, "", false},
      {ProviderKind::LlamaCpp, "llamacpp", "gpt-oss", kLlamaCppBaseUrl, "", false},
  };

  const String prompt = "Berikan 2 ide otomasi rumah berbasis ESP32.";

  AiInvokeResponse response;
  for (size_t i = 0; i < (sizeof(attempts) / sizeof(attempts[0])); ++i) {
    if (!runAttempt(attempts[i], prompt, response)) {
      continue;
    }

    Serial.printf("selected provider: %s\n", attempts[i].label);
    Serial.println("--- response ---");
    Serial.println(response.text);
    return;
  }

  Serial.println("No provider returned a successful response.");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_providers.registerAll(g_registry);

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword)) {
    Serial.println("Set WiFi constants before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  runMultiModelSample();
}

void loop() {
  delay(500);
}
