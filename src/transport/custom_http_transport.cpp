#include "transport/custom_http_transport.h"

#include <SpiJsonDocument.h>

#include <cstdlib>
#include <cstring>

#if defined(ESP32) && __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define AI_HAS_ESP32_HEAP_CAPS 1
#else
#define AI_HAS_ESP32_HEAP_CAPS 0
#endif

namespace ai::provider {

namespace {

void* allocBytes(size_t size, bool preferPsrAm) {
  if (size == 0) {
    return nullptr;
  }

#if AI_HAS_ESP32_HEAP_CAPS
  if (preferPsrAm) {
    void* ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr != nullptr) {
      return ptr;
    }
  }
  return heap_caps_malloc(size, MALLOC_CAP_8BIT);
#else
  (void)preferPsrAm;
  return std::malloc(size);
#endif
}

void* reallocBytes(void* ptr, size_t size, bool preferPsrAm) {
  if (size == 0) {
    return nullptr;
  }

#if AI_HAS_ESP32_HEAP_CAPS
  if (ptr != nullptr) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
  }
  return allocBytes(size, preferPsrAm);
#else
  (void)preferPsrAm;
  return std::realloc(ptr, size);
#endif
}

void freeBytes(void* ptr) {
  if (ptr == nullptr) {
    return;
  }

#if AI_HAS_ESP32_HEAP_CAPS
  heap_caps_free(ptr);
#else
  std::free(ptr);
#endif
}

int parseStatusCode(const String& statusLine) {
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0 || firstSpace + 4 > statusLine.length()) {
    return 0;
  }

  return statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
}

String bytesToString(const char* data, size_t size) {
  if (data == nullptr || size == 0) {
    return String();
  }

  String out;
  out.reserve(size + 1);
  for (size_t i = 0; i < size; ++i) {
    out += data[i];
  }
  return out;
}

String readHeaderValueCaseInsensitive(const String& headers, const char* name) {
  if (name == nullptr || name[0] == '\0') {
    return String();
  }

  String lowerHeaders = headers;
  String lowerName = String(name);
  lowerHeaders.toLowerCase();
  lowerName.toLowerCase();

  const String key = lowerName + ":";
  int start = lowerHeaders.indexOf(key);
  if (start < 0) {
    return String();
  }

  start += key.length();
  while (start < headers.length() && (headers[start] == ' ' || headers[start] == '\t')) {
    ++start;
  }

  int end = headers.indexOf("\r\n", start);
  if (end < 0) {
    end = headers.length();
  }

  return headers.substring(start, end);
}

String extractOpenAiDelta(ArduinoJson::JsonVariantConst delta) {
  if (delta.isNull()) {
    return String();
  }

  if (delta["content"].is<const char*>()) {
    return String(delta["content"].as<const char*>());
  }

  if (delta["content"].is<ArduinoJson::JsonArrayConst>()) {
    String text;
    for (ArduinoJson::JsonVariantConst part : delta["content"].as<ArduinoJson::JsonArrayConst>()) {
      const char* partText = part["text"] | nullptr;
      if (partText == nullptr || partText[0] == '\0') {
        continue;
      }
      text += partText;
    }
    return text;
  }

  return String();
}

String extractClaudeDelta(SpiJsonDocument& doc, bool& outDone, String& outDoneReason) {
  const char* type = doc["type"] | "";
  if (std::strcmp(type, "content_block_delta") == 0) {
    const char* text = doc["delta"]["text"] | nullptr;
    return text == nullptr ? String() : String(text);
  }

  if (std::strcmp(type, "message_delta") == 0) {
    const char* reason = doc["delta"]["stop_reason"] | nullptr;
    if (reason != nullptr && reason[0] != '\0') {
      outDone = true;
      outDoneReason = reason;
    }
    return String();
  }

  if (std::strcmp(type, "message_stop") == 0) {
    outDone = true;
    if (outDoneReason.length() == 0) {
      outDoneReason = "stop";
    }
  }

  return String();
}

String extractOllamaDelta(SpiJsonDocument& doc, bool& outDone, String& outDoneReason) {
  const char* content = doc["message"]["content"] | nullptr;
  if (content != nullptr && content[0] != '\0') {
    return String(content);
  }

  if (doc["done"].is<bool>() && doc["done"].as<bool>()) {
    outDone = true;
    const char* reason = doc["done_reason"] | nullptr;
    if (reason != nullptr && reason[0] != '\0') {
      outDoneReason = reason;
    } else if (outDoneReason.length() == 0) {
      outDoneReason = "done";
    }
  }

  return String();
}

}  // namespace

CustomHttpTransport::PsrAmBuffer::PsrAmBuffer() : data_(nullptr), size_(0), capacity_(0) {}

CustomHttpTransport::PsrAmBuffer::~PsrAmBuffer() {
  reset();
}

void CustomHttpTransport::PsrAmBuffer::reset() {
  freeBytes(data_);
  data_ = nullptr;
  size_ = 0;
  capacity_ = 0;
}

bool CustomHttpTransport::PsrAmBuffer::append(const char* data, size_t size, bool preferPsrAm) {
  if (data == nullptr || size == 0) {
    return true;
  }

  const size_t required = size_ + size + 1;
  if (required > capacity_) {
    size_t nextCap = capacity_ == 0 ? 512 : capacity_;
    while (nextCap < required) {
      nextCap *= 2;
    }

    void* next = reallocBytes(data_, nextCap, preferPsrAm);
    if (next == nullptr) {
      return false;
    }

    data_ = static_cast<char*>(next);
    capacity_ = nextCap;
  }

  std::memcpy(data_ + size_, data, size);
  size_ += size;
  data_[size_] = '\0';
  return true;
}

size_t CustomHttpTransport::PsrAmBuffer::size() const {
  return size_;
}

String CustomHttpTransport::PsrAmBuffer::toString() const {
  if (data_ == nullptr || size_ == 0) {
    return String();
  }

  String out;
  out.reserve(size_ + 1);
  out = data_;
  return out;
}

CustomHttpTransport::CustomHttpTransport() : CustomHttpTransport(Config{}) {}

CustomHttpTransport::CustomHttpTransport(const Config& config)
    : config_(config),
      activeClient_(nullptr),
      state_(AsyncState::Idle),
      headersParsed_(false),
  eventStreamMode_(false),
  streamDoneEmitted_(false),
      contentLength_(-1),
      bodyReceived_(0),
      startedAtMs_(0),
      lastActivityMs_(0),
      timeoutMs_(config.defaultTimeoutMs),
      lastResultOk_(false) {}

bool CustomHttpTransport::supportsAsync() const {
  return true;
}

bool CustomHttpTransport::execute(const AiHttpRequest& request, AiHttpResponse& outResponse) {
  String error;
  AiTransportCallbacks callbacks;
  const AiAsyncSubmitResult submit = executeAsync(request, callbacks, error);
  if (submit != AiAsyncSubmitResult::Accepted) {
    outResponse.statusCode = 0;
    outResponse.errorMessage = error.length() > 0 ? error : String("submit_failed");
    outResponse.body = "";
    return false;
  }

  while (isBusy()) {
    poll();
    yield();
  }

  outResponse = completedResponse_;
  return lastResultOk_;
}

AiAsyncSubmitResult CustomHttpTransport::executeAsync(const AiHttpRequest& request,
                                                      const AiTransportCallbacks& callbacks,
                                                      String& outErrorMessage) {
  if (isBusy()) {
    outErrorMessage = "transport_busy";
    return AiAsyncSubmitResult::Busy;
  }

  UrlParts parts;
  if (!parseUrl(request.url, parts, outErrorMessage)) {
    return AiAsyncSubmitResult::Failed;
  }

  activeRequest_ = request;
  callbacks_ = callbacks;
  activeResponse_ = AiHttpResponse{};
  completedResponse_ = AiHttpResponse{};
  lastResultOk_ = false;

  headersParsed_ = false;
  headerBuffer_ = "";
  eventStreamMode_ = false;
  streamDoneEmitted_ = false;
  eventStreamLineBuffer_ = "";
  contentLength_ = -1;
  bodyReceived_ = 0;
  bodyBuffer_.reset();

  timeoutMs_ = request.timeoutMs > 0 ? request.timeoutMs : config_.defaultTimeoutMs;
  startedAtMs_ = millis();
  lastActivityMs_ = startedAtMs_;

  if (!connectAndSend(request, parts, outErrorMessage)) {
    state_ = AsyncState::Idle;
    activeClient_ = nullptr;
    activeRequest_ = AiHttpRequest{};
    callbacks_ = AiTransportCallbacks{};
    return AiAsyncSubmitResult::Failed;
  }

  state_ = AsyncState::ReadingHeaders;
  return AiAsyncSubmitResult::Accepted;
}

void CustomHttpTransport::poll() {
  if (state_ == AsyncState::Idle || activeClient_ == nullptr) {
    return;
  }

  const uint32_t nowMs = millis();
  if (hasTimedOut(nowMs)) {
    finishRequest(false, "request_timeout");
    return;
  }

  const size_t chunkMax = config_.pollChunkBytes > 0 ? config_.pollChunkBytes : 384;
  char chunk[385];
  while (activeClient_->available() > 0) {
    const size_t readTarget = chunkMax < (sizeof(chunk) - 1) ? chunkMax : (sizeof(chunk) - 1);
    const int readBytes = activeClient_->readBytes(chunk, readTarget);
    if (readBytes <= 0) {
      break;
    }
    chunk[readBytes] = '\0';

    lastActivityMs_ = nowMs;

    if (!headersParsed_) {
      headerBuffer_ += bytesToString(chunk, static_cast<size_t>(readBytes));
      parseHeadersIfReady();
      continue;
    }

    handleBodyChunk(chunk, static_cast<size_t>(readBytes));
  }

  if (headersParsed_ && contentLength_ >= 0 && bodyReceived_ >= static_cast<size_t>(contentLength_)) {
    finishRequest(true);
    return;
  }

  if (!activeClient_->connected() && activeClient_->available() == 0) {
    if (!headersParsed_) {
      finishRequest(false, "connection_closed_before_headers");
      return;
    }
    finishRequest(true);
  }
}

bool CustomHttpTransport::isBusy() const {
  return state_ != AsyncState::Idle;
}

void CustomHttpTransport::cancel() {
  if (activeClient_ != nullptr) {
    activeClient_->stop();
  }
  finishRequest(false, "request_cancelled");
}

bool CustomHttpTransport::parseUrl(const String& url,
                                   UrlParts& outParts,
                                   String& outErrorMessage) {
  outParts = UrlParts{};

  if (url.startsWith("https://")) {
    outParts.tls = true;
    outParts.port = 443;
  } else if (url.startsWith("http://")) {
    outParts.tls = false;
    outParts.port = 80;
  } else {
    outErrorMessage = "unsupported_url_scheme";
    return false;
  }

  const int schemeSep = url.indexOf("://");
  const int hostStart = schemeSep + 3;
  const int pathStart = url.indexOf('/', hostStart);

  String hostPort = pathStart < 0 ? url.substring(hostStart) : url.substring(hostStart, pathStart);
  outParts.path = pathStart < 0 ? String("/") : url.substring(pathStart);
  if (outParts.path.length() == 0) {
    outParts.path = "/";
  }

  const int colon = hostPort.lastIndexOf(':');
  if (colon >= 0) {
    outParts.host = hostPort.substring(0, colon);
    const uint16_t parsedPort = static_cast<uint16_t>(hostPort.substring(colon + 1).toInt());
    if (parsedPort == 0) {
      outErrorMessage = "invalid_url_port";
      return false;
    }
    outParts.port = parsedPort;
  } else {
    outParts.host = hostPort;
  }

  if (outParts.host.length() == 0) {
    outErrorMessage = "missing_url_host";
    return false;
  }

  return true;
}

bool CustomHttpTransport::connectAndSend(const AiHttpRequest& request,
                                         const UrlParts& urlParts,
                                         String& outErrorMessage) {
  activeClient_ = nullptr;

  if (urlParts.tls) {
    if (config_.insecureTls) {
      secureClient_.setInsecure();
    }
    activeClient_ = &secureClient_;
  } else {
    activeClient_ = &plainClient_;
  }

  if (!activeClient_->connect(urlParts.host.c_str(), urlParts.port)) {
    outErrorMessage = "connect_failed";
    activeClient_ = nullptr;
    return false;
  }

  activeClient_->print(request.method);
  activeClient_->print(" ");
  activeClient_->print(urlParts.path);
  activeClient_->print(" HTTP/1.1\r\nHost: ");
  activeClient_->print(urlParts.host);
  activeClient_->print("\r\nConnection: close\r\n");

  for (size_t i = 0; i < request.headerCount; ++i) {
    activeClient_->print(request.headers[i].key);
    activeClient_->print(": ");
    activeClient_->print(request.headers[i].value);
    activeClient_->print("\r\n");
  }

  if (request.body.length() > 0) {
    activeClient_->print("Content-Length: ");
    activeClient_->print(static_cast<unsigned>(request.body.length()));
    activeClient_->print("\r\n");
  }

  activeClient_->print("\r\n");

  if (request.body.length() > 0) {
    activeClient_->print(request.body);
  }

  return true;
}

String CustomHttpTransport::readHeaderValueIgnoreCase(const String& headers, const char* name) {
  return readHeaderValueCaseInsensitive(headers, name);
}

String CustomHttpTransport::extractTextDeltaFromEventJson(const String& payload,
                                                          bool& outDone,
                                                          String& outDoneReason) {
  outDone = false;
  outDoneReason = "";

  SpiJsonDocument doc;
  const auto err = deserializeJson(doc, payload);
  if (err) {
    return String();
  }

  if (doc["choices"].is<ArduinoJson::JsonArrayConst>()) {
    ArduinoJson::JsonArrayConst choices = doc["choices"].as<ArduinoJson::JsonArrayConst>();
    if (!choices.isNull() && choices.size() > 0) {
      ArduinoJson::JsonObjectConst choice = choices[0].as<ArduinoJson::JsonObjectConst>();
      const char* finishReason = choice["finish_reason"] | nullptr;
      if (finishReason != nullptr && finishReason[0] != '\0') {
        outDone = true;
        outDoneReason = finishReason;
      }

      return extractOpenAiDelta(choice["delta"]);
    }
  }

  const String claudeDelta = extractClaudeDelta(doc, outDone, outDoneReason);
  if (claudeDelta.length() > 0 || outDone) {
    return claudeDelta;
  }

  return extractOllamaDelta(doc, outDone, outDoneReason);
}

void CustomHttpTransport::parseHeadersIfReady() {
  const int headerEnd = headerBuffer_.indexOf("\r\n\r\n");
  if (headerEnd < 0) {
    return;
  }

  const String headersPart = headerBuffer_.substring(0, headerEnd + 2);
  const int firstLineEnd = headersPart.indexOf("\r\n");
  if (firstLineEnd < 0) {
    finishRequest(false, "invalid_status_line");
    return;
  }

  const String statusLine = headersPart.substring(0, firstLineEnd);
  activeResponse_.statusCode = parseStatusCode(statusLine);

  const String contentLengthHeader = readHeaderValueIgnoreCase(headersPart, "Content-Length");
  if (contentLengthHeader.length() > 0) {
    contentLength_ = contentLengthHeader.toInt();
  }

  const String contentType = readHeaderValueIgnoreCase(headersPart, "Content-Type");
  eventStreamMode_ = contentType.indexOf("text/event-stream") >= 0;
  activeResponse_.streaming = eventStreamMode_;
  activeResponse_.partial = eventStreamMode_;

  headersParsed_ = true;
  state_ = AsyncState::ReadingBody;

  const int bodyStart = headerEnd + 4;
  if (bodyStart < headerBuffer_.length()) {
    const String remainder = headerBuffer_.substring(bodyStart);
    handleBodyChunk(remainder.c_str(), remainder.length());
  }

  headerBuffer_ = "";
}

void CustomHttpTransport::handleBodyChunk(const char* data, size_t size) {
  if (size == 0) {
    return;
  }

  if (!bodyBuffer_.append(data, size, activeRequest_.preferPsrAm)) {
    finishRequest(false, "psram_buffer_allocation_failed");
    return;
  }

  bodyReceived_ += size;

  if (callbacks_.onChunk != nullptr) {
    if (eventStreamMode_) {
      parseEventStreamLines(data, size);
      return;
    }

    AiStreamChunk chunk;
    chunk.textDelta = bytesToString(data, size);
    chunk.rawChunk = chunk.textDelta;
    emitChunkCallback(chunk);
  }
}

void CustomHttpTransport::parseEventStreamLines(const char* data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    const char ch = data[i];
    if (ch == '\r') {
      continue;
    }

    if (ch != '\n') {
      eventStreamLineBuffer_ += ch;
      continue;
    }

    String line = eventStreamLineBuffer_;
    eventStreamLineBuffer_ = "";

    if (line.length() == 0) {
      continue;
    }

    if (!line.startsWith("data:")) {
      continue;
    }

    String payload = line.substring(5);
    payload.trim();
    parseEventStreamData(payload);
  }
}

void CustomHttpTransport::parseEventStreamData(const String& payload) {
  if (callbacks_.onChunk == nullptr || payload.length() == 0) {
    return;
  }

  AiStreamChunk chunk;
  chunk.rawChunk = payload;

  if (payload == "[DONE]") {
    chunk.done = true;
    chunk.doneReason = "done";
    streamDoneEmitted_ = true;
    emitChunkCallback(chunk);
    return;
  }

  bool done = false;
  String doneReason;
  chunk.textDelta = extractTextDeltaFromEventJson(payload, done, doneReason);
  chunk.done = done;
  chunk.doneReason = doneReason;

  if (chunk.textDelta.length() == 0 && !chunk.done) {
    return;
  }

  if (chunk.done) {
    streamDoneEmitted_ = true;
  }

  emitChunkCallback(chunk);
}

void CustomHttpTransport::emitChunkCallback(const AiStreamChunk& chunk) const {
  if (callbacks_.onChunk == nullptr) {
    return;
  }

  callbacks_.onChunk(chunk, callbacks_.userContext);
}

bool CustomHttpTransport::hasTimedOut(uint32_t nowMs) const {
  const uint32_t timeout = timeoutMs_ > 0 ? timeoutMs_ : config_.defaultTimeoutMs;
  if (timeout == 0) {
    return false;
  }

  return (nowMs - startedAtMs_) > timeout;
}

void CustomHttpTransport::finishRequest(bool ok, const String& errorMessage) {
  if (activeClient_ != nullptr) {
    activeClient_->stop();
  }

  if (eventStreamMode_ && callbacks_.onChunk != nullptr) {
    if (eventStreamLineBuffer_.length() > 0) {
      String dangling = eventStreamLineBuffer_;
      eventStreamLineBuffer_ = "";
      if (dangling.startsWith("data:")) {
        String payload = dangling.substring(5);
        payload.trim();
        parseEventStreamData(payload);
      }
    }

    if (!streamDoneEmitted_) {
      AiStreamChunk doneChunk;
      doneChunk.done = true;
      doneChunk.doneReason = ok ? "stream_end" : "stream_error";
      emitChunkCallback(doneChunk);
      streamDoneEmitted_ = true;
    }
  }

  activeResponse_.body = bodyBuffer_.toString();
  activeResponse_.bodyBytes = bodyBuffer_.size();
  activeResponse_.errorMessage = errorMessage;

  completedResponse_ = activeResponse_;
  lastResultOk_ = ok;

  if (callbacks_.onDone != nullptr) {
    callbacks_.onDone(completedResponse_, callbacks_.userContext);
  }

  state_ = AsyncState::Idle;
  activeClient_ = nullptr;
  activeRequest_ = AiHttpRequest{};
  callbacks_ = AiTransportCallbacks{};
  activeResponse_ = AiHttpResponse{};

  headerBuffer_ = "";
  headersParsed_ = false;
  eventStreamMode_ = false;
  streamDoneEmitted_ = false;
  eventStreamLineBuffer_ = "";
  contentLength_ = -1;
  bodyReceived_ = 0;
  bodyBuffer_.reset();
}

}  // namespace ai::provider
