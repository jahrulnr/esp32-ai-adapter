#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>

#include <core/ai_session_client.h>
#include <providers/default_provider_bundle.h>
#include <transport/websocket_session_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
WebSocketSessionTransport g_wsTransport;
AiSessionClient g_sessionClient(g_wsTransport, g_registry);

String g_sessionId;
bool g_sessionStarted = false;
bool g_audioSent = false;

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

void onRealtimeState(const String& sessionId,
                     bool connected,
                     const String& reason,
                     void* userContext) {
  (void)userContext;
  Serial.printf("[state] session=%s connected=%s reason=%s\n",
                sessionId.c_str(),
                connected ? "true" : "false",
                reason.c_str());
}

void onRealtimeEvent(const String& sessionId,
                     const AiRealtimeParsedEvent& event,
                     void* userContext) {
  (void)sessionId;
  (void)userContext;

  if (event.textDelta.length() > 0) {
    Serial.print(event.textDelta);
  }

  if (event.kind == AiRealtimeEventKind::Error) {
    Serial.printf("\n[error] code=%s msg=%s\n",
                  event.errorCode.c_str(),
                  event.errorMessage.c_str());
  }
}

void onRealtimeDone(const String& sessionId,
                    const AiInvokeResponse& response,
                    void* userContext) {
  (void)userContext;
  Serial.printf("\n[done] session=%s ok=%s finish=%s\n",
                sessionId.c_str(),
                response.ok ? "true" : "false",
                response.finishReason.c_str());
}

void startRealtimeSession() {
  AiInvokeRequest request;
  request.model = "gpt-realtime";
  request.apiKey = kOpenAiApiKey;
  request.timeoutMs = 60000;

  AiRealtimeSessionConfig sessionConfig;
  sessionConfig.timeoutMs = 60000;
  sessionConfig.enableAudioInput = true;
  sessionConfig.enableAudioOutput = true;

  AiSessionClientCallbacks callbacks;
  callbacks.onState = onRealtimeState;
  callbacks.onEvent = onRealtimeEvent;
  callbacks.onDone = onRealtimeDone;

  String error;
  if (!g_sessionClient.startSession(
          ProviderKind::OpenAI, request, sessionConfig, callbacks, g_sessionId, error)) {
    Serial.printf("startSession failed: %s\n", error.c_str());
    return;
  }

  g_sessionStarted = true;
  Serial.printf("startSession accepted: %s\n", g_sessionId.c_str());
}

void sendSmartAudioSample() {
  constexpr size_t kAudioBytes = 32000;
  static uint8_t fakePcm[kAudioBytes];

  for (size_t i = 0; i < kAudioBytes; ++i) {
    fakePcm[i] = static_cast<uint8_t>(i & 0x7F);
  }

  AiAudioChunkingOptions opts;
  opts.preferredChunkBytes = 8192;
  opts.minChunkBytes = 512;
  opts.lowMemoryThresholdBytes = 100 * 1024;
  opts.safetyHeadroomBytes = 20 * 1024;
  opts.spillFs = &LittleFS;
  opts.spillFilePath = "/audio_spill.bin";
  opts.removeSpillFileAfterSend = true;

  String error;
  if (!g_sessionClient.sendAudioBytesSmart(
          g_sessionId, fakePcm, kAudioBytes, "audio/pcm", opts, error)) {
    Serial.printf("sendAudioBytesSmart failed: %s\n", error.c_str());
    return;
  }

  Serial.println("sendAudioBytesSmart: ok");
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

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  startRealtimeSession();
}

void loop() {
  if (!g_sessionStarted) {
    delay(200);
    return;
  }

  g_sessionClient.poll();

  if (!g_audioSent && g_sessionClient.isSessionOpen(g_sessionId)) {
    sendSmartAudioSample();
    g_audioSent = true;
  }

  delay(1);
}
