#include <Arduino.h>
#include <WiFi.h>

#include <AiProviderKit.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

AiProviderKit g_ai;

String g_sessionId;
bool g_realtimeStarted = false;
bool g_realtimePromptSent = false;

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

void runHttpWithFacade() {
  AiInvokeResponse response;
  String error;
  const bool ok = g_ai.llmWithTools(
      ProviderKind::OpenAI,
      "Tolong cek cuaca Jakarta pakai tool yang tersedia.",
      false,
      response,
      error,
      AiToolHttpLoopOptions{});

  Serial.printf("HTTP facade ok=%s status=%u code=%s\n",
                ok ? "true" : "false",
                response.statusCode,
                response.errorCode.c_str());
  if (error.length() > 0) {
    Serial.printf("HTTP facade error=%s\n", error.c_str());
  }
  if (response.text.length() > 0) {
    Serial.println("--- HTTP final response ---");
    Serial.println(response.text);
  }
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
    Serial.printf("\n[realtime_error] code=%s msg=%s\n",
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

void startRealtimeWithFacade() {
  AiRealtimeSessionConfig sessionConfig;
  sessionConfig.timeoutMs = 60000;

  AiSessionClientCallbacks callbacks;
  callbacks.onState = onRealtimeState;
  callbacks.onEvent = onRealtimeEvent;
  callbacks.onDone = onRealtimeDone;

  String error;
  if (!g_ai.realtimeStartWithTools(
          ProviderKind::OpenAI, sessionConfig, callbacks, g_sessionId, error)) {
    Serial.printf("startRealtimeSessionWithTools failed: %s\n", error.c_str());
    return;
  }

  g_realtimeStarted = true;
  Serial.printf("Realtime facade session accepted: %s\n", g_sessionId.c_str());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_ai.setApiKey(ProviderKind::OpenAI, kOpenAiApiKey);
  g_ai.setLlmModel(ProviderKind::OpenAI, "gpt-4o-mini");
  g_ai.setRealtimeModel(ProviderKind::OpenAI, "gpt-realtime");
  g_ai.defaults().llmSystemPrompt = "Gunakan tool kalau diperlukan, lalu jawab ringkas.";

  AiToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get weather by city name";
  tool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";
  String registerError;
  if (!g_ai.tools().registerTool(tool, onGetWeather, nullptr, registerError)) {
    Serial.printf("registerTool failed: %s\n", registerError.c_str());
    return;
  }

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword) ||
      hasPlaceholderValue(kOpenAiApiKey)) {
    Serial.println("Set WiFi and API key constants before running this example.");
    return;
  }

  if (!connectWifi()) {
    return;
  }

  runHttpWithFacade();
  startRealtimeWithFacade();
}

void loop() {
  if (!g_realtimeStarted) {
    delay(200);
    return;
  }

  g_ai.poll();

  if (!g_realtimePromptSent && g_ai.isRealtimeOpen(g_sessionId)) {
    String error;
    if (!g_ai.realtimeSendText(g_sessionId,
                               "Tolong cek cuaca Jakarta pakai tool yang tersedia, lalu jawab singkat.",
                               error)) {
      Serial.printf("sendText failed: %s\n", error.c_str());
    } else {
      g_realtimePromptSent = true;
      Serial.println("sendText: ok");
    }
  }

  delay(1);
}
