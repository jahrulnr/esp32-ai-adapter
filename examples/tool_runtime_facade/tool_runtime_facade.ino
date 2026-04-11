#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_provider_client.h>
#include <core/ai_session_client.h>
#include <core/ai_tool_runtime.h>
#include <core/ai_tool_runtime_facade.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>
#include <transport/websocket_session_transport.h>

using namespace ai::provider;

constexpr const char* kWifiSsid = "YOUR_WIFI_SSID";
constexpr const char* kWifiPassword = "YOUR_WIFI_PASSWORD";
constexpr const char* kOpenAiApiKey = "YOUR_OPENAI_API_KEY";

constexpr uint32_t kWifiConnectTimeoutMs = 20000;

CustomHttpTransport g_httpTransport;
AiProviderRegistry g_registry;
DefaultProviderBundle g_providers;
AiProviderClient g_httpClient(g_httpTransport, g_registry);
WebSocketSessionTransport g_wsTransport;
AiSessionClient g_sessionClient(g_wsTransport, g_registry);
AiToolRuntimeRegistry g_toolRegistry;
AiToolRuntimeFacade g_facade(g_httpClient, g_sessionClient, g_toolRegistry);

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
  AiInvokeRequest request;
  request.model = "gpt-4o-mini";
  request.apiKey = kOpenAiApiKey;
  request.baseUrl = "https://api.openai.com/v1";
  request.systemPrompt = "Gunakan tool kalau diperlukan, lalu jawab ringkas.";
  request.prompt = "Tolong cek cuaca Jakarta pakai tool yang tersedia.";
  request.enableToolCalls = true;
  request.toolChoice = "auto";

  AiInvokeResponse response;
  String error;
  const bool ok = g_facade.invokeHttpWithTools(
      ProviderKind::OpenAI, request, response, error, AiToolHttpLoopOptions{});

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
  AiInvokeRequest request;
  request.model = "gpt-realtime";
  request.apiKey = kOpenAiApiKey;
  request.enableToolCalls = true;
  request.toolChoice = "auto";

  AiRealtimeSessionConfig sessionConfig;
  sessionConfig.timeoutMs = 60000;

  AiSessionClientCallbacks callbacks;
  callbacks.onState = onRealtimeState;
  callbacks.onEvent = onRealtimeEvent;
  callbacks.onDone = onRealtimeDone;

  String error;
  if (!g_facade.startRealtimeSessionWithTools(
          ProviderKind::OpenAI, request, sessionConfig, callbacks, g_sessionId, error)) {
    Serial.printf("startRealtimeSessionWithTools failed: %s\n", error.c_str());
    return;
  }

  g_realtimeStarted = true;
  Serial.printf("Realtime facade session accepted: %s\n", g_sessionId.c_str());
}

void setup() {
  Serial.begin(115200);
  delay(300);

  g_providers.registerAll(g_registry);

  AiToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get weather by city name";
  tool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";
  String registerError;
  if (!g_toolRegistry.registerTool(tool, onGetWeather, nullptr, registerError)) {
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

  g_facade.pollRealtime();

  if (!g_realtimePromptSent && g_facade.isSessionOpen(g_sessionId)) {
    String error;
    if (!g_facade.sendText(
            g_sessionId,
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
