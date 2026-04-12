#include "providers/openai/openai_audio_adapter.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>

#include "core/ai_http_request_body_utils.h"
#include "core/url_utils.h"
#include "providers/openai/openai_compat_utils.h"

namespace ai::provider {

namespace {

constexpr const char* kDefaultBaseUrl = "https://api.openai.com/v1";
constexpr const char* kChatPath = "/chat/completions";
constexpr const char* kDefaultSttModel = "gpt-4o-mini-transcribe";
constexpr const char* kDefaultTtsModel = "gpt-4o-mini-tts";
constexpr const char* kDefaultVoice = "alloy";

String normalizeInputAudioFormat(const String& mimeType) {
  if (mimeType.length() == 0) {
    return "wav";
  }

  String lower = mimeType;
  lower.toLowerCase();

  if (lower.indexOf("wav") >= 0) {
    return "wav";
  }
  if (lower.indexOf("mp3") >= 0 || lower.indexOf("mpeg") >= 0) {
    return "mp3";
  }
  if (lower.indexOf("flac") >= 0) {
    return "flac";
  }
  if (lower.indexOf("opus") >= 0 || lower.indexOf("ogg") >= 0) {
    return "opus";
  }
  if (lower.indexOf("aac") >= 0) {
    return "aac";
  }

  return "wav";
}

String normalizeOutputAudioFormat(const String& format) {
  if (format.length() == 0) {
    return "mp3";
  }

  String lower = format;
  lower.toLowerCase();

  if (lower.indexOf('/') >= 0) {
    if (lower.indexOf("wav") >= 0) {
      return "wav";
    }
    if (lower.indexOf("mpeg") >= 0 || lower.indexOf("mp3") >= 0) {
      return "mp3";
    }
    if (lower.indexOf("flac") >= 0) {
      return "flac";
    }
    if (lower.indexOf("opus") >= 0 || lower.indexOf("ogg") >= 0) {
      return "opus";
    }
    if (lower.indexOf("aac") >= 0) {
      return "aac";
    }
  }

  if (lower == "wav" || lower == "mp3" || lower == "flac" || lower == "opus" ||
      lower == "aac") {
    return lower;
  }
  return "mp3";
}

String audioFormatToMime(const String& format) {
  if (format.equalsIgnoreCase("wav")) {
    return "audio/wav";
  }
  if (format.equalsIgnoreCase("mp3")) {
    return "audio/mpeg";
  }
  if (format.equalsIgnoreCase("flac")) {
    return "audio/flac";
  }
  if (format.equalsIgnoreCase("opus")) {
    return "audio/opus";
  }
  if (format.equalsIgnoreCase("aac")) {
    return "audio/aac";
  }
  return "audio/mpeg";
}

String parseOpenAiErrorCode(const SpiJsonDocument& doc) {
  const char* code = doc["error"]["code"] | nullptr;
  if (code != nullptr && code[0] != '\0') {
    return String(code);
  }

  const char* type = doc["error"]["type"] | nullptr;
  if (type != nullptr && type[0] != '\0') {
    return String(type);
  }

  return "upstream_error";
}

String parseOpenAiErrorMessage(const SpiJsonDocument& doc) {
  const char* message = doc["error"]["message"] | nullptr;
  if (message != nullptr && message[0] != '\0') {
    return String(message);
  }
  return "Upstream request failed";
}

size_t estimateBase64DecodedSize(const String& base64Data) {
  const size_t len = base64Data.length();
  if (len == 0) {
    return 0;
  }

  size_t padding = 0;
  if (len >= 1 && base64Data[len - 1] == '=') {
    ++padding;
  }
  if (len >= 2 && base64Data[len - 2] == '=') {
    ++padding;
  }

  return ((len / 4) * 3) - padding;
}

String buildSttInstruction(const AiSpeechToTextRequest& request) {
  String instruction;
  if (request.prompt.length() > 0) {
    instruction += request.prompt;
  }

  if (request.language.length() > 0) {
    if (instruction.length() > 0) {
      instruction += "\n";
    }
    instruction += "Target language: ";
    instruction += request.language;
  }

  if (instruction.length() == 0) {
    instruction = "Transcribe this audio accurately.";
  }

  return instruction;
}

}  // namespace

bool OpenAiAudioAdapter::supportsSpeechToText() const {
  return true;
}

bool OpenAiAudioAdapter::supportsTextToSpeech() const {
  return true;
}

bool OpenAiAudioAdapter::supportsSpeechToTextStreaming() const {
  return true;
}

bool OpenAiAudioAdapter::supportsTextToSpeechStreaming() const {
  return false;
}

bool OpenAiAudioAdapter::buildSpeechToTextRequest(const AiSpeechToTextRequest& request,
                                                  AiHttpRequest& outHttpRequest,
                                                  String& outErrorMessage) const {
  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing OpenAI API key";
    return false;
  }
  if (request.audioBase64.length() == 0) {
    outErrorMessage = "audioBase64 is required for speech-to-text";
    return false;
  }

  SpiJsonDocument body;
  body["model"] = request.model.length() > 0 ? request.model : String(kDefaultSttModel);
  body["stream"] = request.stream;
  body["temperature"] = 0;

  ArduinoJson::JsonArray messages = body["messages"].to<ArduinoJson::JsonArray>();
  ArduinoJson::JsonObject userMessage = messages.add<ArduinoJson::JsonObject>();
  userMessage["role"] = "user";

  ArduinoJson::JsonArray content = userMessage["content"].to<ArduinoJson::JsonArray>();

  ArduinoJson::JsonObject audioPart = content.add<ArduinoJson::JsonObject>();
  audioPart["type"] = "input_audio";
  ArduinoJson::JsonObject inputAudio = audioPart["input_audio"].to<ArduinoJson::JsonObject>();
  inputAudio["data"] = request.audioBase64;
  inputAudio["format"] = normalizeInputAudioFormat(request.audioMimeType);

  ArduinoJson::JsonObject textPart = content.add<ArduinoJson::JsonObject>();
  textPart["type"] = "text";
  textPart["text"] = buildSttInstruction(request);

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");
  outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);

  if (!applyJsonBodyToHttpRequest(body, request.bodySpool, outHttpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_body_build_failed";
    }
    return false;
  }

  return true;
}

bool OpenAiAudioAdapter::parseSpeechToTextResponse(const AiHttpResponse& httpResponse,
                                                   AiSpeechToTextResponse& outResponse) const {
  outResponse = AiSpeechToTextResponse{};

  AiInvokeResponse invoke;
  const bool ok = parseOpenAiStyleResponse(httpResponse, invoke);

  outResponse.ok = ok;
  outResponse.statusCode = invoke.statusCode;
  outResponse.text = invoke.text;
  outResponse.errorCode = invoke.errorCode;
  outResponse.errorMessage = invoke.errorMessage;
  outResponse.rawResponse = invoke.rawResponse;
  outResponse.promptTokens = invoke.promptTokens;
  outResponse.completionTokens = invoke.completionTokens;
  outResponse.totalTokens = invoke.totalTokens;

  return ok;
}

bool OpenAiAudioAdapter::buildTextToSpeechRequest(const AiTextToSpeechRequest& request,
                                                  AiHttpRequest& outHttpRequest,
                                                  String& outErrorMessage) const {
  if (request.apiKey.length() == 0) {
    outErrorMessage = "Missing OpenAI API key";
    return false;
  }
  if (request.inputText.length() == 0) {
    outErrorMessage = "inputText is required for text-to-speech";
    return false;
  }

  SpiJsonDocument body;
  body["model"] = request.model.length() > 0 ? request.model : String(kDefaultTtsModel);
  body["stream"] = request.stream;

  ArduinoJson::JsonArray modalities = body["modalities"].to<ArduinoJson::JsonArray>();
  modalities.add("text");
  modalities.add("audio");

  ArduinoJson::JsonObject audio = body["audio"].to<ArduinoJson::JsonObject>();
  audio["format"] = normalizeOutputAudioFormat(request.outputFormat);
  audio["voice"] = request.voice.length() > 0 ? request.voice : String(kDefaultVoice);

  ArduinoJson::JsonArray messages = body["messages"].to<ArduinoJson::JsonArray>();

  if (request.instructions.length() > 0) {
    ArduinoJson::JsonObject developerMessage = messages.add<ArduinoJson::JsonObject>();
    developerMessage["role"] = "developer";
    developerMessage["content"] = request.instructions;
  }

  ArduinoJson::JsonObject userMessage = messages.add<ArduinoJson::JsonObject>();
  userMessage["role"] = "user";
  userMessage["content"] = request.inputText;

  outHttpRequest = AiHttpRequest{};
  outHttpRequest.method = "POST";
  outHttpRequest.url =
      joinUrl(request.baseUrl.length() > 0 ? request.baseUrl : String(kDefaultBaseUrl), kChatPath);
  outHttpRequest.addHeader("Content-Type", "application/json");
  outHttpRequest.addHeader("Authorization", String("Bearer ") + request.apiKey);

  if (!applyJsonBodyToHttpRequest(body, request.bodySpool, outHttpRequest, outErrorMessage)) {
    if (outErrorMessage.length() == 0) {
      outErrorMessage = "request_body_build_failed";
    }
    return false;
  }

  return true;
}

bool OpenAiAudioAdapter::parseTextToSpeechResponse(const AiHttpResponse& httpResponse,
                                                   AiTextToSpeechResponse& outResponse) const {
  outResponse = AiTextToSpeechResponse{};
  outResponse.statusCode =
      static_cast<uint16_t>(httpResponse.statusCode < 0 ? 0 : httpResponse.statusCode);
  outResponse.rawResponse = httpResponse.body;

  SpiJsonDocument doc;
  const auto parseErr = deserializeJson(doc, httpResponse.body);
  if (parseErr) {
    outResponse.ok = false;
    outResponse.errorCode = "invalid_json";
    outResponse.errorMessage = String("JSON parse failed: ") + parseErr.c_str();
    return false;
  }

  const bool isSuccess = httpResponse.statusCode >= 200 && httpResponse.statusCode < 300;
  if (!isSuccess) {
    outResponse.ok = false;
    outResponse.errorCode = parseOpenAiErrorCode(doc);
    outResponse.errorMessage = parseOpenAiErrorMessage(doc);
    return false;
  }

  ArduinoJson::JsonObjectConst message =
      doc["choices"][0]["message"].as<ArduinoJson::JsonObjectConst>();
  ArduinoJson::JsonObjectConst audio = message["audio"].as<ArduinoJson::JsonObjectConst>();

  String audioBase64 = String(audio["data"] | "");
  String format = String(audio["format"] | "");

  if (audioBase64.length() == 0) {
    ArduinoJson::JsonObjectConst choiceAudio =
        doc["choices"][0]["audio"].as<ArduinoJson::JsonObjectConst>();
    audioBase64 = String(choiceAudio["data"] | "");
    if (format.length() == 0) {
      format = String(choiceAudio["format"] | "");
    }
  }

  if (audioBase64.length() == 0) {
    outResponse.ok = false;
    outResponse.errorCode = "empty_audio";
    outResponse.errorMessage = "Response does not contain audio payload";
    return false;
  }

  outResponse.ok = true;
  outResponse.audioBase64 = audioBase64;
  outResponse.audioMimeType = audioFormatToMime(format);
  outResponse.audioBytes = estimateBase64DecodedSize(audioBase64);
  return true;
}

}  // namespace ai::provider