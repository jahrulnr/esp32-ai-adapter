# AiProviderKit

AiProviderKit is a local PlatformIO library that provides a single interface for multiple AI backends while keeping each provider isolated in its own folder.

## Why this exists

- Keep app-level AI calls provider-agnostic.
- Prevent payload/header branching from leaking everywhere.
- Allow adding a new provider without touching the core contract.

## Provider research snapshot (2026-04)

1. OpenAI
- Base URL: https://api.openai.com/v1
- Chat endpoint: POST /chat/completions
- Auth: Authorization: Bearer OPENAI_API_KEY
- Text output path: choices[0].message.content

2. Claude (Anthropic)
- Base URL: https://api.anthropic.com
- Chat endpoint: POST /v1/messages
- Auth: x-api-key + anthropic-version header
- Text output path: content[].text (type == text)

3. OpenRouter
- Base URL: https://openrouter.ai/api/v1
- Chat endpoint: POST /chat/completions (OpenAI-like schema)
- Auth: Authorization: Bearer OPENROUTER_API_KEY
- Optional attribution headers: HTTP-Referer, X-OpenRouter-Title
- Text output path: choices[0].message.content

4. Ollama
- Base URL: http://192.168.x.x:11434
- Chat endpoint: POST /api/chat
- Auth: usually none on local setup
- Text output path: message.content

5. llama.cpp (server)
- Base URL: http://192.168.x.x:8080/v1 (OpenAI-compatible routes)
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

| Provider | STT | TTS | STT Streaming | TTS Streaming |
| --- | --- | --- | --- | --- |
| OpenAI | yes | yes | yes | no |
| OpenRouter (compat) | yes | yes | yes | no |
| Claude | no | no | no | no |
| Ollama | no | no | no | no |
| llama.cpp | no | no | no | no |

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
- Async flow uses callbacks:
	- `AiTransportCallbacks::onChunk` for partial chunks
	- `AiTransportCallbacks::onDone` when response is complete
- For `text/event-stream`, the transport emits parsed `AiStreamChunk` deltas:
	- OpenAI/OpenRouter/llama.cpp style `choices[0].delta.content`
	- Claude style `content_block_delta.delta.text`
	- Ollama JSON-line style `message.content` in stream payloads

## Minimal Async Example

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

1. Create a new folder in src/providers/<provider_name>/.
2. Implement IAiProviderAdapter.
3. Register it via AiProviderRegistry or DefaultProviderBundle.
4. No changes are required in core interfaces.
