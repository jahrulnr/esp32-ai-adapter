# AiProviderKit

AiProviderKit is a local PlatformIO library that provides a single interface for multiple AI backends while keeping each provider isolated in its own folder.

Public repository name is `esp32-ai-adapter`, while the in-code library identity remains `AiProviderKit`.

## Installation

PlatformIO (`platformio.ini`):

```ini
[env:your_env]
lib_deps =
	https://github.com/jahrulnr/esp32-ai-adapter.git
```

## Quickstart (one-class facade)

The facade entry point is `AiProviderKit`. It hides transport/registry/provider wiring and lets you configure per-provider secrets/models once.

The facade always uses the library-owned custom transports:

- `CustomHttpTransport` for HTTP-backed LLM/STT/TTS calls.
- `WebSocketSessionTransport` for realtime WebSocket sessions.

These are the supported default paths for constrained ESP32 runtime use. Transport tuning is exposed through `AiProviderKitConfig::httpConfig` and `AiProviderKitConfig::wsConfig`.

Minimal LLM call:

```cpp
#include <Arduino.h>
#include <WiFi.h>

#include <AiProviderKit.h>

using namespace ai::provider;

AiProviderKit ai;

void setup() {
  Serial.begin(115200);

  ai.setApiKey(ProviderKind::OpenAI, "YOUR_OPENAI_API_KEY");
  ai.setLlmModel(ProviderKind::OpenAI, "gpt-4o-mini");
  ai.defaults().llmSystemPrompt = "Jawab ringkas dalam Bahasa Indonesia.";

  AiInvokeResponse out;
  String err;
  if (ai.llm(ProviderKind::OpenAI, "Apa itu ESP-NOW?", false, out, err)) {
    Serial.println(out.text);
  } else {
    Serial.printf("llm failed: %s\n", err.c_str());
  }
}

void loop() {
  ai.poll();
  delay(1);
}
```

Facade examples:

- `examples/basic_sync/basic_sync.ino`
- `examples/tool_runtime_facade/tool_runtime_facade.ino`
- `examples/facade_full/facade_full.ino`

## Worker queue

For HTTP-backed AI calls that must not run on a caller stack, `AiProviderKit` can own a FreeRTOS worker and queue. The queued path currently covers LLM-with-tools, speech-to-text, and text-to-speech. Realtime remains a separate lifecycle.

```cpp
AiProviderKitWorkerConfig worker;
worker.stackBytes = 12 * 1024;
worker.priority = 2;
worker.core = 1;
worker.queueDepth = 4;
ai.beginWorker(worker);

AiProviderKitLlmRequest req;
req.providerKind = ProviderKind::OpenAI;
req.prompt = "Status node aktif?";
req.model = "gpt-4o-mini";

AiProviderKitLlmResult out;
ai.submitLlmWithToolsBlocking(req, out, 150);
```

If the app owns task creation, create a task with the desired stack policy and call `ai.loop(portMAX_DELAY)` inside it. Default worker values can also be overridden at build time with `AIPROVIDERKIT_WORKER_STACK_BYTES`, `AIPROVIDERKIT_WORKER_PRIORITY`, `AIPROVIDERKIT_WORKER_CORE`, `AIPROVIDERKIT_WORKER_STACK_CAPS`, and `AIPROVIDERKIT_QUEUE_DEPTH`. When ESP-IDF task caps APIs are available, the built-in worker defaults to an internal 8-bit stack allocation unless `stackCaps` is explicitly set.

## Why this exists

- Keep app-level AI calls provider-agnostic.
- Prevent payload/header branching from leaking everywhere.
- Allow adding a new provider without touching the core contract.

## Provider research snapshot (2026-04)

1. OpenAI

- Base URL: [https://api.openai.com/v1](https://api.openai.com/v1)
- Chat endpoint: POST /chat/completions
- Auth: Authorization: Bearer OPENAI_API_KEY
- Text output path: choices[0].message.content

1. Claude (Anthropic)

- Base URL: [https://api.anthropic.com](https://api.anthropic.com)
- Chat endpoint: POST /v1/messages
- Auth: x-api-key + anthropic-version header
- Text output path: content[].text (type == text)

1. OpenRouter

- Base URL: [https://openrouter.ai/api/v1](https://openrouter.ai/api/v1)
- Chat endpoint: POST /chat/completions (OpenAI-like schema)
- Auth: Authorization: Bearer OPENROUTER_API_KEY
- Optional attribution headers: HTTP-Referer, X-OpenRouter-Title
- Text output path: choices[0].message.content

1. Ollama

- Base URL: [http://192.168.x.x:11434](http://192.168.x.x:11434)
- Chat endpoint: POST /api/chat
- Auth: usually none on local setup
- Text output path: message.content

1. llama.cpp (server)

- Base URL: [http://192.168.x.x:8080/v1](http://192.168.x.x:8080/v1) (OpenAI-compatible routes)
- Chat endpoint: POST /chat/completions
- Auth: optional, depends on --api-key server flag
- Text output path: choices[0].message.content

## Architecture

- core/: neutral contracts (request/response, transport interface, registry, facade client)
- providers/: isolated adapters per vendor
- providers/default_provider_bundle.*: convenience registration for built-in providers

## Core contract

- IAiTransport handles HTTP execution only.
- IAiProviderAdapter translates neutral request <-> provider-specific HTTP payload.
- IAiAudioProviderAdapter translates neutral STT/TTS request <-> provider-specific HTTP payload.
- AiProviderRegistry resolves provider implementation by enum or string id.
- AiProviderClient orchestrates build request -> transport execute -> parse response.
- AiAudioClient orchestrates build request -> transport execute -> parse response for STT/TTS.

Low-level APIs are still available under `src/core/` and `src/transport/` if you need custom orchestration, but the recommended entry point for apps is `AiProviderKit`.

## Chat Session Memory Store

`core/ai_chat_session_store.h` provides reusable conversation memory primitives for embedded runtimes.

Main types:

- `IAiChatSessionStore`: interface for app-layer injection and mocking.
- `AiChatSessionStore`: filesystem-backed implementation (LittleFS/SD compatible).
- `AiChatSessionStoreConfig`: expiry, cleanup cadence, compaction, and size guardrails.

Supported operations:

- Storage and path setup: `setStorage`, `setMemoryPath`, `setSkillsPath`, `begin`
- Session lifecycle: `openChat`, `touch`, `markInFlight`, `deleteChat`/`reset`
- Context reconstruction: `buildContextMessages`
- Retention control: `compactChat`, `cleanupChat`/`runMandatoryCleanup`
- Observability: `status`/`chatDetails`

Behavior notes:

- Session files are stored as JSONL for append-friendly writes.
- Compaction keeps latest turns and inserts a deterministic summary record.
- Cleanup enforces expiry and total-bytes hard limit with orphan file sweep.
- Unknown future role/text fields are ignored safely during line parsing.

Minimal setup:

```cpp
#include <core/ai_chat_session_store.h>

using namespace ai::provider;

AiChatSessionStoreConfig cfg;
cfg.memoryPath = "/cache/chat_sessions";
cfg.expiryMs = 30UL * 60UL * 1000UL;

AiChatSessionStore store(cfg);
store.setStorage(&LittleFS);
store.begin();
store.openChat("session-1", millis());
store.appendTurn("session-1", "user", "Halo", millis());
```

## Skill Store

- `core/ai_skill.h` provides a lightweight in-memory skill store class named `Skill`.
- CRUD-style operations are available:
  - `Add` to insert a new skill item.
  - `List` to retrieve all active skill items.
  - `Get` to fetch one skill by id.
  - `Remove` to delete one skill by id.
  - `Update` to replace an existing skill record.
- Memory behavior can be tuned with `ConfigureMemory`:
  - `preferPsrAm=true` applies ESP32 external-memory malloc preference (`heap_caps_malloc_extmem_enable`).
  - `extmemAlwaysInternalThreshold` controls small-allocation internal-memory bias.
- Persistence methods are available:
  - `SaveToFs` / `LoadFromFs` for JSON file storage (LittleFS/SPIFFS/SD via `fs::FS`).
  - `SaveToNvs` / `LoadFromNvs` for NVS blob storage with strict `maxBytes` guard.

Minimal snippet:

```cpp
#include <core/ai_skill.h>

using namespace ai::provider;

Skill store;
String error;

AiSkillMemoryConfig memConfig;
memConfig.preferPsrAm = true;
memConfig.extmemAlwaysInternalThreshold = 64;
store.ConfigureMemory(memConfig, error);

AiSkillItem item;
item.id = "weather";
item.name = "Weather Analyst";
item.description = "Handle weather lookups";
item.instructions = "Always include unit and city";

store.Add(item, error);

AiSkillList list;
store.List(list);

AiSkillItem one;
store.Get("weather", one);

item.description = "Handle weather lookups and summarize";
store.Update("weather", item, error);

store.SaveToFs(LittleFS, "/skills.json", error);
store.LoadFromFs(LittleFS, "/skills.json", error);

store.SaveToNvs("aikit_sk", "payload", 2048, error);
store.LoadFromNvs("aikit_sk", "payload", 2048, error);

store.Remove("weather");
```

## Speech and Audio (STT/TTS)

- `core/ai_audio_client.h` provides sync + async methods for:
  - speech-to-text (`transcribe` / `transcribeAsync`)
  - text-to-speech (`synthesize` / `synthesizeAsync`)
- `OpenAiProvider` now exposes audio capability through `asAudioAdapter()`.
- `OpenRouterProvider` now exposes audio capability through `asAudioAdapter()` (OpenAI-compatible route).
- Current baseline implementation targets OpenAI chat-audio compatible JSON payloads to keep transport binary-safe.
- Ready-to-run sample is available at `examples/stt_tts_basic/stt_tts_basic.ino`.
- Async chaining sample is available at `examples/stt_tts_async/stt_tts_async.ino`.

Provider audio capability snapshot:


| Provider            | STT | TTS | STT Streaming | TTS Streaming |
| ------------------- | --- | --- | ------------- | ------------- |
| OpenAI              | yes | yes | yes           | no            |
| OpenRouter (compat) | yes | yes | yes           | no            |
| Claude              | no  | no  | no            | no            |
| Ollama              | no  | no  | no            | no            |
| llama.cpp           | no  | no  | no            | no            |


OpenRouter attribution headers can be set on audio requests:

- `AiSpeechToTextRequest::httpReferer`
- `AiSpeechToTextRequest::appTitle`
- `AiTextToSpeechRequest::httpReferer`
- `AiTextToSpeechRequest::appTitle`

Provider caveats:

- OpenRouter audio is compatibility-based and depends on selected upstream model/provider route.
- Some OpenRouter routes may reject audio payloads even when chat text works.
- In examples, OpenRouter is used first and fallback to OpenAI only when error code is `provider_not_supported`.
- Native binary audio endpoints are not used yet; current baseline keeps JSON audio payloads for transport stability.

Minimal sync snippet:

```cpp
#include <core/ai_audio_client.h>

CustomHttpTransport transport;
AiProviderRegistry registry;
DefaultProviderBundle providers;
AiAudioClient audioClient(transport, registry);

providers.registerAll(registry);

AiSpeechToTextRequest stt;
stt.baseUrl = "https://api.openai.com/v1";
stt.apiKey = "YOUR_OPENAI_API_KEY";
stt.model = "gpt-4o-mini-transcribe";
stt.audioMimeType = "audio/wav";
stt.audioBase64 = "<base64-audio>";

AiSpeechToTextResponse sttOut;
audioClient.transcribe(ProviderKind::OpenAI, stt, sttOut);

AiTextToSpeechRequest tts;
tts.baseUrl = "https://api.openai.com/v1";
tts.apiKey = "YOUR_OPENAI_API_KEY";
tts.model = "gpt-4o-mini-tts";
tts.voice = "alloy";
tts.outputFormat = "mp3";
tts.inputText = sttOut.text;

AiTextToSpeechResponse ttsOut;
audioClient.synthesize(ProviderKind::OpenAI, tts, ttsOut);
```

## Transport and Realtime

- `transport/custom_http_transport.*` provides sync + nonblocking HTTP execution.
- Response body buffering prefers PSRAM on ESP32 (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` fallback to normal heap).
- Request payload can be sourced from FS file to reduce RAM pressure:
  - `AiHttpRequest::bodyFs` + `AiHttpRequest::bodyFilePath`
  - transport sets `Content-Length` from file size and writes file content to socket in small chunks
  - `AiHttpRequest::removeBodyFileAfterSend=true` can auto-delete temporary spool file
  - if both `body` and `bodyFs/bodyFilePath` are set, request is rejected (`request_body_conflict`)
- Async flow uses callbacks:
  - `AiTransportCallbacks::onChunk` for partial chunks
  - `AiTransportCallbacks::onDone` when response is complete
- For `text/event-stream`, the transport emits parsed `AiStreamChunk` deltas:
  - OpenAI/OpenRouter/llama.cpp style `choices[0].delta.content`
  - Claude style `content_block_delta.delta.text`
  - Ollama JSON-line style `message.content` in stream payloads

### Spool to FS then upload

Use this pattern when payload is large and you want deterministic memory usage.

```cpp
File tmp = LittleFS.open("/tmp_req.json", FILE_WRITE);
tmp.print("{\"model\":\"gpt-4o-mini\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}");
tmp.close();

AiHttpRequest request;
request.method = "POST";
request.url = "https://api.openai.com/v1/chat/completions";
request.addHeader("Content-Type", "application/json");
request.addHeader("Authorization", String("Bearer ") + apiKey);
request.bodyFs = &LittleFS;
request.bodyFilePath = "/tmp_req.json";
request.bodyStreamChunkBytes = 1024;
request.removeBodyFileAfterSend = true;

AiHttpResponse response;
transport.execute(request, response);
```

### Auto-spool from provider request

Provider builders (`AiInvokeRequest`, `AiSpeechToTextRequest`, `AiTextToSpeechRequest`) now support automatic FS spool via `bodySpool`.

```cpp
AiInvokeRequest req;
req.baseUrl = "https://api.openai.com/v1";
req.apiKey = "YOUR_KEY";
req.model = "gpt-4o-mini";
req.prompt = "Generate a long structured JSON output";

req.bodySpool.filesystem = &LittleFS;
req.bodySpool.filePath = "/tmp/invoke_req.json";
req.bodySpool.thresholdBytes = 16 * 1024;  // spool only when payload is large
req.bodySpool.streamChunkBytes = 1024;
req.bodySpool.removeAfterSend = true;

AiInvokeResponse out;
client.invoke(ProviderKind::OpenAI, req, out);
```

`bodySpool` behavior:

- if `forceSpool=true`, request must have valid `filesystem` and `filePath`
- if payload size >= `thresholdBytes`, JSON body is written to file and sent from FS with `Content-Length`
- if below threshold (and not forced), body stays in-memory as before

## Advanced Custom HTTP Example

Use this only when the facade is not enough and manual transport/client wiring is required. Keep the custom transport; do not swap this path to generic Arduino HTTP clients in ESP32 low-memory builds.

```cpp
#include <core/ai_provider_client.h>
#include <providers/default_provider_bundle.h>
#include <transport/custom_http_transport.h>

using namespace ai::provider;

void onDelta(const AiStreamChunk& chunk, void* user) {
	(void)user;
	if (chunk.textDelta.length() > 0) {
		Serial.print(chunk.textDelta);
	}
	if (chunk.done) {
		Serial.printf("\n[stream done] reason=%s\n", chunk.doneReason.c_str());
	}
}

void onDone(const AiInvokeResponse& response, void* user) {
	(void)user;
	Serial.printf("\nfinal ok=%s status=%u finish=%s\n",
								response.ok ? "true" : "false",
								response.statusCode,
								response.finishReason.c_str());
}

CustomHttpTransport transport;
AiProviderRegistry registry;
DefaultProviderBundle providers;
AiProviderClient client(transport, registry);

void startCall() {
	providers.registerAll(registry);

	AiInvokeRequest req;
	req.baseUrl = "https://api.openai.com/v1";
	req.apiKey = "YOUR_KEY";
	req.model = "gpt-4o-mini";
	req.prompt = "Give me 3 short bullet points about ESP-NOW";
	req.stream = true;
	req.streamCallback = onDelta;

	String err;
	if (!client.invokeAsync(ProviderKind::OpenAI, req, onDone, nullptr, err)) {
		Serial.printf("invokeAsync failed: %s\n", err.c_str());
	}
}

void loopTick() {
	client.poll();
}
```

## Add a new provider

1. Create a new folder in src/providers/.
2. Implement IAiProviderAdapter.
3. Register it via AiProviderRegistry or DefaultProviderBundle.
4. No changes are required in core interfaces.

