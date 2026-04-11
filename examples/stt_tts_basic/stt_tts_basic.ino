#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_audio_client.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenRouterApiKey = "YOUR_OPENROUTER_API_KEY";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";
constexpr const char* kOpenRouterReferer = "https://example.local";
constexpr const char* kOpenRouterAppTitle = "AiProviderKit STT/TTS Basic";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_transport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiAudioClient g_audioClient(g_transport, g_registry);

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

void runSttAndTtsSample() {
  const bool openRouterReady = !hasPlaceholderValue(kOpenRouterApiKey);
  const bool openAiReady = !hasPlaceholderValue(kOpenAiApiKey);

  if (!openRouterReady && !openAiReady) {
    Serial.println("Set OpenRouter or OpenAI API key constants before running this example.");
    return;
  }

  ProviderKind sttProvider = openRouterReady ? ProviderKind::OpenRouter : ProviderKind::OpenAI;

  AiSpeechToTextRequest sttRequest;
  sttRequest.baseUrl = sttProvider == ProviderKind::OpenRouter ? "https://openrouter.ai/api/v1"
                                                                : "https://api.openai.com/v1";
  sttRequest.apiKey = sttProvider == ProviderKind::OpenRouter ? kOpenRouterApiKey : kOpenAiApiKey;
  sttRequest.model = sttProvider == ProviderKind::OpenRouter ? "openai/gpt-4o-mini-transcribe"
                                                              : "gpt-4o-mini-transcribe";
  sttRequest.httpReferer = kOpenRouterReferer;
  sttRequest.appTitle = kOpenRouterAppTitle;
  sttRequest.audioMimeType = "audio/wav";
  sttRequest.audioBase64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAIA+AAABAAgAZGF0YQAAAAA=";
  sttRequest.prompt = "Transcribe this short audio.";

  AiSpeechToTextResponse sttResponse;
  bool sttOk = g_audioClient.transcribe(sttProvider, sttRequest, sttResponse);

  if (!sttOk && sttProvider == ProviderKind::OpenRouter &&
      sttResponse.errorCode.equalsIgnoreCase("provider_not_supported") && openAiReady) {
    Serial.println("STT fallback: OpenRouter not supported, retrying OpenAI");
    sttProvider = ProviderKind::OpenAI;
    sttRequest.baseUrl = "https://api.openai.com/v1";
    sttRequest.apiKey = kOpenAiApiKey;
    sttRequest.model = "gpt-4o-mini-transcribe";
    sttOk = g_audioClient.transcribe(sttProvider, sttRequest, sttResponse);
  }

  Serial.printf("STT provider=%s ok=%s status=%u code=%s\n",
                sttProvider == ProviderKind::OpenRouter ? "openrouter" : "openai",
                sttOk ? "true" : "false",
                sttResponse.statusCode,
                sttResponse.errorCode.c_str());

  if (sttResponse.text.length() > 0) {
    Serial.printf("STT text: %s\n", sttResponse.text.c_str());
  }
  if (!sttOk) {
    Serial.printf("STT error: %s\n", sttResponse.errorMessage.c_str());
  }

  ProviderKind ttsProvider = openRouterReady ? ProviderKind::OpenRouter : ProviderKind::OpenAI;

  AiTextToSpeechRequest ttsRequest;
  ttsRequest.baseUrl = ttsProvider == ProviderKind::OpenRouter ? "https://openrouter.ai/api/v1"
                                                                : "https://api.openai.com/v1";
  ttsRequest.apiKey = ttsProvider == ProviderKind::OpenRouter ? kOpenRouterApiKey : kOpenAiApiKey;
  ttsRequest.model =
      ttsProvider == ProviderKind::OpenRouter ? "openai/gpt-4o-mini-tts" : "gpt-4o-mini-tts";
  ttsRequest.httpReferer = kOpenRouterReferer;
  ttsRequest.appTitle = kOpenRouterAppTitle;
  ttsRequest.voice = "alloy";
  ttsRequest.outputFormat = "mp3";
  ttsRequest.inputText = sttResponse.text.length() > 0 ? sttResponse.text : "Halo dari AiProviderKit";

  AiTextToSpeechResponse ttsResponse;
  bool ttsOk = g_audioClient.synthesize(ttsProvider, ttsRequest, ttsResponse);

  if (!ttsOk && ttsProvider == ProviderKind::OpenRouter &&
      ttsResponse.errorCode.equalsIgnoreCase("provider_not_supported") && openAiReady) {
    Serial.println("TTS fallback: OpenRouter not supported, retrying OpenAI");
    ttsProvider = ProviderKind::OpenAI;
    ttsRequest.baseUrl = "https://api.openai.com/v1";
    ttsRequest.apiKey = kOpenAiApiKey;
    ttsRequest.model = "gpt-4o-mini-tts";
    ttsOk = g_audioClient.synthesize(ttsProvider, ttsRequest, ttsResponse);
  }

  Serial.printf("TTS provider=%s ok=%s status=%u code=%s bytes=%u\n",
                ttsProvider == ProviderKind::OpenRouter ? "openrouter" : "openai",
                ttsOk ? "true" : "false",
                ttsResponse.statusCode,
                ttsResponse.errorCode.c_str(),
                static_cast<unsigned>(ttsResponse.audioBytes));

  if (ttsOk) {
    const String preview = ttsResponse.audioBase64.substring(0, 64);
    Serial.printf("TTS base64 preview: %s...\n", preview.c_str());
    Serial.printf("TTS mime: %s\n", ttsResponse.audioMimeType.c_str());
  } else {
    Serial.printf("TTS error: %s\n", ttsResponse.errorMessage.c_str());
  }
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

  runSttAndTtsSample();
}

void loop() {
  delay(500);
}
