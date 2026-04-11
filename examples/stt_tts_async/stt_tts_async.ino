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
constexpr const char* kOpenRouterAppTitle = "AiProviderKit STT/TTS Async";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_transport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiAudioClient g_audioClient(g_transport, g_registry);

bool g_flowStarted = false;
bool g_flowDone = false;
bool g_openRouterReady = false;
bool g_openAiReady = false;

ProviderKind g_sttProvider = ProviderKind::Unknown;
ProviderKind g_ttsProvider = ProviderKind::Unknown;
String g_lastTranscript;

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

void onTtsDone(const AiTextToSpeechResponse& response, void* userContext) {
  auto* audioClient = static_cast<AiAudioClient*>(userContext);

  if (!response.ok && g_ttsProvider == ProviderKind::OpenRouter &&
      response.errorCode.equalsIgnoreCase("provider_not_supported") && g_openAiReady) {
    Serial.println("[async][TTS] fallback: OpenRouter not supported, retrying OpenAI");

    AiTextToSpeechRequest retryRequest;
    retryRequest.baseUrl = "https://api.openai.com/v1";
    retryRequest.apiKey = kOpenAiApiKey;
    retryRequest.model = "gpt-4o-mini-tts";
    retryRequest.voice = "alloy";
    retryRequest.outputFormat = "mp3";
      retryRequest.inputText =
        g_lastTranscript.length() > 0 ? g_lastTranscript : "Fallback TTS from async flow";

    String err;
    g_ttsProvider = ProviderKind::OpenAI;
    if (audioClient->synthesizeAsync(ProviderKind::OpenAI, retryRequest, onTtsDone, userContext, err)) {
      return;
    }
    Serial.printf("[async][TTS] fallback submit failed: %s\n", err.c_str());
  }

  Serial.printf("[async][TTS] provider=%s ok=%s status=%u code=%s bytes=%u\n",
                g_ttsProvider == ProviderKind::OpenRouter ? "openrouter" : "openai",
                response.ok ? "true" : "false",
                response.statusCode,
                response.errorCode.c_str(),
                static_cast<unsigned>(response.audioBytes));

  if (response.ok) {
    Serial.printf("[async][TTS] mime=%s\n", response.audioMimeType.c_str());
    Serial.printf("[async][TTS] base64 preview=%s...\n",
                  response.audioBase64.substring(0, 48).c_str());
  } else {
    Serial.printf("[async][TTS] error=%s\n", response.errorMessage.c_str());
  }

  g_flowDone = true;
}

void onSttDone(const AiSpeechToTextResponse& response, void* userContext) {
  auto* audioClient = static_cast<AiAudioClient*>(userContext);

  Serial.printf("[async][STT] ok=%s status=%u code=%s\n",
                response.ok ? "true" : "false",
                response.statusCode,
                response.errorCode.c_str());

  if (!response.ok && g_sttProvider == ProviderKind::OpenRouter &&
      response.errorCode.equalsIgnoreCase("provider_not_supported") && g_openAiReady) {
    Serial.println("[async][STT] fallback: OpenRouter not supported, retrying OpenAI");

    AiSpeechToTextRequest retryRequest;
    retryRequest.baseUrl = "https://api.openai.com/v1";
    retryRequest.apiKey = kOpenAiApiKey;
    retryRequest.model = "gpt-4o-mini-transcribe";
    retryRequest.audioMimeType = "audio/wav";
    retryRequest.audioBase64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAIA+AAABAAgAZGF0YQAAAAA=";
    retryRequest.prompt = "Transcribe this short audio.";

    String err;
    g_sttProvider = ProviderKind::OpenAI;
    if (audioClient->transcribeAsync(ProviderKind::OpenAI,
                                     retryRequest,
                                     onSttDone,
                                     userContext,
                                     err)) {
      return;
    }
    Serial.printf("[async][STT] fallback submit failed: %s\n", err.c_str());
  }

  if (!response.ok) {
    Serial.printf("[async][STT] error=%s\n", response.errorMessage.c_str());
    g_flowDone = true;
    return;
  }

  Serial.printf("[async][STT] text=%s\n", response.text.c_str());
  g_lastTranscript = response.text;

  AiTextToSpeechRequest ttsRequest;
  ttsRequest.baseUrl = g_openRouterReady ? "https://openrouter.ai/api/v1"
                                         : "https://api.openai.com/v1";
  ttsRequest.apiKey = g_openRouterReady ? kOpenRouterApiKey : kOpenAiApiKey;
  ttsRequest.model = g_openRouterReady ? "openai/gpt-4o-mini-tts" : "gpt-4o-mini-tts";
  ttsRequest.httpReferer = kOpenRouterReferer;
  ttsRequest.appTitle = kOpenRouterAppTitle;
  ttsRequest.voice = "alloy";
  ttsRequest.outputFormat = "mp3";
  ttsRequest.inputText = response.text.length() > 0 ? response.text : "Halo dari mode async";

  String err;
  g_ttsProvider = g_openRouterReady ? ProviderKind::OpenRouter : ProviderKind::OpenAI;

  if (!audioClient->synthesizeAsync(g_ttsProvider, ttsRequest, onTtsDone, userContext, err)) {
    Serial.printf("[async][TTS] submit failed: %s\n", err.c_str());
    g_flowDone = true;
  }
}

void startAsyncFlow() {
  if (!g_openRouterReady && !g_openAiReady) {
    Serial.println("Set OpenRouter or OpenAI API key constants before running this example.");
    g_flowDone = true;
    return;
  }

  AiSpeechToTextRequest sttRequest;
  sttRequest.baseUrl = g_openRouterReady ? "https://openrouter.ai/api/v1"
                                         : "https://api.openai.com/v1";
  sttRequest.apiKey = g_openRouterReady ? kOpenRouterApiKey : kOpenAiApiKey;
  sttRequest.model =
      g_openRouterReady ? "openai/gpt-4o-mini-transcribe" : "gpt-4o-mini-transcribe";
  sttRequest.httpReferer = kOpenRouterReferer;
  sttRequest.appTitle = kOpenRouterAppTitle;
  sttRequest.audioMimeType = "audio/wav";
  sttRequest.audioBase64 = "UklGRiQAAABXQVZFZm10IBAAAAABAAEAgD4AAIA+AAABAAgAZGF0YQAAAAA=";
  sttRequest.prompt = "Transcribe this short audio.";

  String err;
  g_sttProvider = g_openRouterReady ? ProviderKind::OpenRouter : ProviderKind::OpenAI;

  if (!g_audioClient.transcribeAsync(g_sttProvider, sttRequest, onSttDone, &g_audioClient, err)) {
    Serial.printf("[async][STT] submit failed: %s\n", err.c_str());
    g_flowDone = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_providers.registerAll(g_registry);
  g_openRouterReady = !hasPlaceholderValue(kOpenRouterApiKey);
  g_openAiReady = !hasPlaceholderValue(kOpenAiApiKey);

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword)) {
    Serial.println("Set WiFi constants before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  startAsyncFlow();
  g_flowStarted = true;
}

void loop() {
  if (!g_flowStarted || g_flowDone) {
    delay(200);
    return;
  }

  g_audioClient.poll();
  delay(1);
}
