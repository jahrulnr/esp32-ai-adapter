#include <Arduino.h>
#include <WiFi.h>

#include <LittleFS.h>

#include <AiProviderKit.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

AiProviderKitConfig g_cfg;
AiProviderKit* g_ai = nullptr;

String g_rtSessionId;
bool g_rtStarted = false;
bool g_rtPromptSent = false;

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
  (void)userContext;
  (void)toolCall;
  outErrorMessage = String();
  outResultJson = "{\"city\":\"Jakarta\",\"tempC\":30,\"condition\":\"cloudy\"}";
  return true;
}

void runConversationAndTools() {
  if (g_ai == nullptr) {
    return;
  }

  g_ai->setConversationEnabled(true);
  g_ai->openConversation("demo-1", millis());

  AiToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get weather by city name";
  tool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";
  String toolErr;
  g_ai->tools().registerTool(tool, onGetWeather, nullptr, toolErr);

  AiInvokeResponse out;
  String err;
  if (g_ai->llmWithTools(ProviderKind::OpenAI,
                         "Cek cuaca Jakarta pakai tool, lalu jawab singkat dalam Bahasa Indonesia.",
                         true,
                         out,
                         err)) {
    Serial.println("--- LLM (conversation+tools) ---");
    Serial.println(out.text);
  } else {
    Serial.printf("llmWithTools failed: %s code=%s msg=%s\n",
                  err.c_str(),
                  out.errorCode.c_str(),
                  out.errorMessage.c_str());
  }
}

void onRtState(const String& sessionId, bool connected, const String& reason, void* userContext) {
  (void)userContext;
  Serial.printf("[rt_state] id=%s connected=%s reason=%s\n",
                sessionId.c_str(),
                connected ? "true" : "false",
                reason.c_str());
}

void onRtEvent(const String& sessionId, const AiRealtimeParsedEvent& event, void* userContext) {
  (void)sessionId;
  (void)userContext;
  if (event.textDelta.length() > 0) {
    Serial.print(event.textDelta);
  }
  if (event.kind == AiRealtimeEventKind::Error) {
    Serial.printf("\n[rt_error] code=%s msg=%s\n",
                  event.errorCode.c_str(),
                  event.errorMessage.c_str());
  }
}

void onRtDone(const String& sessionId, const AiInvokeResponse& response, void* userContext) {
  (void)userContext;
  Serial.printf("\n[rt_done] id=%s ok=%s finish=%s\n",
                sessionId.c_str(),
                response.ok ? "true" : "false",
                response.finishReason.c_str());
}

void startRealtime() {
  if (g_ai == nullptr) {
    return;
  }

  AiRealtimeSessionConfig sessionConfig;
  sessionConfig.timeoutMs = 60000;

  AiSessionClientCallbacks cb;
  cb.onState = onRtState;
  cb.onEvent = onRtEvent;
  cb.onDone = onRtDone;

  String err;
  if (!g_ai->realtimeStartWithTools(ProviderKind::OpenAI, sessionConfig, cb, g_rtSessionId, err)) {
    Serial.printf("realtimeStartWithTools failed: %s\n", err.c_str());
    return;
  }
  g_rtStarted = true;
  Serial.printf("realtime accepted: %s\n", g_rtSessionId.c_str());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  if (hasPlaceholderValue(kWifiSsid) || hasPlaceholderValue(kWifiPassword) ||
      hasPlaceholderValue(kOpenAiApiKey)) {
    Serial.println("Set WiFi and API key constants before running this example.");
    return;
  }

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS begin failed");
    return;
  }

  g_cfg.sessionStoreFs = &LittleFS;
  g_cfg.conversation.enabled = true;
  g_cfg.sessionStoreConfig.memoryPath = "/cache/chat_sessions";

  static AiProviderKit ai(g_cfg);
  g_ai = &ai;

  g_ai->setApiKey(ProviderKind::OpenAI, kOpenAiApiKey);
  g_ai->setLlmModel(ProviderKind::OpenAI, "gpt-4o-mini");
  g_ai->setRealtimeModel(ProviderKind::OpenAI, "gpt-realtime");
  g_ai->setSttModel(ProviderKind::OpenAI, "gpt-4o-mini-transcribe");
  g_ai->setTtsModel(ProviderKind::OpenAI, "gpt-4o-mini-tts");
  g_ai->defaults().llmSystemPrompt = "Jawab singkat dan jelas dalam Bahasa Indonesia.";

  if (!connectWifi()) {
    return;
  }

  runConversationAndTools();
  startRealtime();
}

void loop() {
  if (g_ai != nullptr) {
    g_ai->poll();
  }

  if (g_ai != nullptr && g_rtStarted && !g_rtPromptSent && g_ai->isRealtimeOpen(g_rtSessionId)) {
    String err;
    if (g_ai->realtimeSendText(g_rtSessionId, "Halo! Perkenalkan dirimu singkat.", err)) {
      g_rtPromptSent = true;
      Serial.println("\nrealtimeSendText: ok");
    } else {
      Serial.printf("realtimeSendText failed: %s\n", err.c_str());
    }
  }

  delay(1);
}

