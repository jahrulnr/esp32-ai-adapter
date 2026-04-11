#include <Arduino.h>
#include <WiFi.h>

#include <core/ai_session_client.h>
#include <core/ai_tool_runtime.h>
#include <core/ai_tool_runtime_executor.h>
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
AiToolRuntimeRegistry g_toolRegistry;
AiToolRuntimeExecutor g_toolExecutor(g_toolRegistry);

String g_sessionId;
bool g_started = false;
bool g_promptSent = false;

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
  (void)userContext;

  if (event.textDelta.length() > 0) {
    Serial.print(event.textDelta);
  }

  if (event.hasToolCall) {
    String error;
    if (!g_toolExecutor.onRealtimeToolCall(sessionId, event, g_sessionClient, error)) {
      Serial.printf("\n[tool_auto_error] %s\n", error.c_str());
    } else {
      Serial.printf("\n[tool_auto_ok] name=%s\n", event.toolCall.name.c_str());
    }
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
  Serial.printf("\n[done] session=%s ok=%s finish=%s code=%s\n",
                sessionId.c_str(),
                response.ok ? "true" : "false",
                response.finishReason.c_str(),
                response.errorCode.c_str());
}

void startRealtimeSession() {
  AiToolDefinition tool;
  tool.name = "get_weather";
  tool.description = "Get weather by city name";
  tool.inputSchemaJson =
      "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\"}},\"required\":[\"city\"]}";

  String error;
  if (!g_toolRegistry.registerTool(tool, onGetWeather, nullptr, error)) {
    Serial.printf("registerTool failed: %s\n", error.c_str());
    return;
  }

  AiInvokeRequest request;
  request.model = "gpt-realtime";
  request.apiKey = kOpenAiApiKey;
  request.enableToolCalls = true;
  request.toolChoice = "auto";
  if (!g_toolRegistry.appendToolDefinitionsToRequest(request, error)) {
    Serial.printf("appendToolDefinitionsToRequest failed: %s\n", error.c_str());
    return;
  }

  AiRealtimeSessionConfig sessionConfig;
  sessionConfig.timeoutMs = 60000;

  AiSessionClientCallbacks callbacks;
  callbacks.onState = onRealtimeState;
  callbacks.onEvent = onRealtimeEvent;
  callbacks.onDone = onRealtimeDone;

  if (!g_sessionClient.startSession(
          ProviderKind::OpenAI, request, sessionConfig, callbacks, g_sessionId, error)) {
    Serial.printf("startSession failed: %s\n", error.c_str());
    return;
  }

  g_started = true;
  Serial.printf("startSession accepted: %s\n", g_sessionId.c_str());
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

  startRealtimeSession();
}

void loop() {
  if (!g_started) {
    delay(200);
    return;
  }

  g_sessionClient.poll();

  if (!g_promptSent && g_sessionClient.isSessionOpen(g_sessionId)) {
    String error;
    if (!g_sessionClient.sendText(
            g_sessionId,
            "Tolong cek cuaca Jakarta pakai tool yang tersedia, lalu jawab singkat.",
            error)) {
      Serial.printf("sendText failed: %s\n", error.c_str());
    } else {
      g_promptSent = true;
      Serial.println("sendText: ok");
    }
  }

  delay(1);
}
