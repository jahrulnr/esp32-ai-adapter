#include "transport/custom_http_transport.h"

#include <SpiJsonDocument.h>

#include <cstdlib>
#include <cstring>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#if defined(ESP32) && __has_include(<WiFi.h>)
#include <WiFi.h>
#define AI_HAS_ESP32_WIFI 1
#else
#define AI_HAS_ESP32_WIFI 0
#endif

#if defined(ESP32) && __has_include(<esp_log.h>)
#include <esp_log.h>
#define AI_HAS_ESP_LOG 1
#else
#define AI_HAS_ESP_LOG 0
#endif

#if defined(ESP32) && __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define AI_HAS_ESP32_HEAP_CAPS 1
#else
#define AI_HAS_ESP32_HEAP_CAPS 0
#endif

#if defined(ESP32) && __has_include(<mbedtls/platform.h>)
#include <mbedtls/platform.h>
#define AI_HAS_MBEDTLS_PLATFORM_ALLOC 1
#else
#define AI_HAS_MBEDTLS_PLATFORM_ALLOC 0
#endif

namespace ai::provider {

namespace {

#if AI_HAS_ESP_LOG
constexpr const char* kTag = "AI_HTTP_TX";
#endif

#if AI_HAS_MBEDTLS_PLATFORM_ALLOC
bool gMbedTlsAllocatorConfigured = false;
bool gMbedTlsAllocatorConfigFailed = false;

void* mbedTlsPsrAmCalloc(size_t n, size_t size) {
  if (n == 0 || size == 0 || size > (SIZE_MAX / n)) {
    return nullptr;
  }

#if AI_HAS_ESP32_HEAP_CAPS
  void* ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr != nullptr) {
    return ptr;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
#else
  return std::calloc(n, size);
#endif
}

void mbedTlsPsrAmFree(void* ptr) {
  if (ptr == nullptr) {
    return;
  }

#if AI_HAS_ESP32_HEAP_CAPS
  heap_caps_free(ptr);
#else
  std::free(ptr);
#endif
}

bool ensureMbedTlsAllocatorForPsrAm() {
  if (gMbedTlsAllocatorConfigured) {
    return true;
  }

  if (gMbedTlsAllocatorConfigFailed) {
    return false;
  }

  const int rc = mbedtls_platform_set_calloc_free(mbedTlsPsrAmCalloc, mbedTlsPsrAmFree);
  if (rc == 0) {
    gMbedTlsAllocatorConfigured = true;
#if AI_HAS_ESP_LOG
    ESP_LOGW(kTag, "mbedTLS allocator switched to PSRAM-preferred mode");
#endif
    return true;
  }

  gMbedTlsAllocatorConfigFailed = true;
#if AI_HAS_ESP_LOG
  ESP_LOGW(kTag, "mbedTLS allocator switch failed rc=%d", rc);
#endif
  return false;
}
#else
bool ensureMbedTlsAllocatorForPsrAm() {
  return false;
}
#endif

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

bool parseHexChunkSize(const String& line, size_t& outSize) {
  String token = line;
  token.trim();
  const int extSep = token.indexOf(';');
  if (extSep >= 0) {
    token = token.substring(0, extSep);
    token.trim();
  }

  if (token.length() == 0) {
    return false;
  }

  char* endPtr = nullptr;
  const unsigned long value = std::strtoul(token.c_str(), &endPtr, 16);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }

  outSize = static_cast<size_t>(value);
  return true;
}

bool decodeChunkedBody(const String& rawBody, String& outDecoded) {
  outDecoded = "";
  size_t cursor = 0;
  while (cursor < static_cast<size_t>(rawBody.length())) {
    const int lineEnd = rawBody.indexOf("\r\n", static_cast<unsigned int>(cursor));
    if (lineEnd < 0) {
      return false;
    }

    size_t chunkSize = 0;
    if (!parseHexChunkSize(rawBody.substring(cursor, static_cast<size_t>(lineEnd)), chunkSize)) {
      return false;
    }

    cursor = static_cast<size_t>(lineEnd) + 2;
    if (chunkSize == 0) {
      return true;
    }

    if ((cursor + chunkSize) > static_cast<size_t>(rawBody.length())) {
      return false;
    }

    outDecoded.concat(rawBody.c_str() + cursor, chunkSize);
    cursor += chunkSize;

    if ((cursor + 2) > static_cast<size_t>(rawBody.length()) ||
        rawBody[cursor] != '\r' ||
        rawBody[cursor + 1] != '\n') {
      return false;
    }
    cursor += 2;
  }

  return false;
}

bool isZeroAddress(const IPAddress& address) {
  if (address.type() == IPv4) {
    return static_cast<uint32_t>(address) == 0;
  }

  for (size_t i = 0; i < 16; ++i) {
    if (address[i] != 0) {
      return false;
    }
  }
  return true;
}

bool applyEmergencyDnsOverride() {
#if AI_HAS_ESP32_WIFI
  const IPAddress currentPrimary = WiFi.dnsIP(0);
  const IPAddress currentSecondary = WiFi.dnsIP(1);

  if (!isZeroAddress(currentPrimary) || !isZeroAddress(currentSecondary)) {
    return false;
  }

  IPAddress emergencyPrimary;
  IPAddress emergencySecondary;
  if (!emergencyPrimary.fromString("1.1.1.1") || !emergencySecondary.fromString("8.8.8.8")) {
    return false;
  }

  if (!WiFi.setDNS(emergencyPrimary, emergencySecondary)) {
#if AI_HAS_ESP_LOG
    ESP_LOGW(kTag, "Emergency DNS override failed");
#endif
    return false;
  }

#if AI_HAS_ESP_LOG
  ESP_LOGW(kTag,
           "Emergency DNS override applied active=%s,%s",
           WiFi.dnsIP(0).toString().c_str(),
           WiFi.dnsIP(1).toString().c_str());
#endif
  return true;
#else
  return false;
#endif
}

bool resolveKnownHostFallback(const String& host,
                             IPAddress& outAddress,
                             String& outErrorDetail) {
  if (!host.equalsIgnoreCase("openrouter.ai")) {
    return false;
  }

#if CONFIG_LWIP_IPV6
  // Cloudflare anycast IPv6 observed for openrouter.ai. Used only when DNS fails.
  static const char* kOpenRouterIpv6Fallbacks[] = {
      "2606:4700::6812:273",
      "2606:4700::6812:373",
  };

  for (size_t i = 0; i < (sizeof(kOpenRouterIpv6Fallbacks) / sizeof(kOpenRouterIpv6Fallbacks[0])); ++i) {
    IPAddress candidate;
    if (!candidate.fromString(kOpenRouterIpv6Fallbacks[i])) {
      continue;
    }
    outAddress = candidate;
    outErrorDetail = String("fallback_openrouter_ipv6:") + kOpenRouterIpv6Fallbacks[i];
#if AI_HAS_ESP_LOG
    ESP_LOGW(kTag,
             "Using literal host fallback host=%s addr=%s",
             host.c_str(),
             kOpenRouterIpv6Fallbacks[i]);
#endif
    return true;
  }
#endif

  // Cloudflare anycast IPv4 observed for openrouter.ai. Used only when DNS fails.
  static const char* kOpenRouterIpv4Fallbacks[] = {
      "104.18.3.115",
      "104.18.2.115",
  };

  for (size_t i = 0; i < (sizeof(kOpenRouterIpv4Fallbacks) / sizeof(kOpenRouterIpv4Fallbacks[0])); ++i) {
    IPAddress candidate;
    if (!candidate.fromString(kOpenRouterIpv4Fallbacks[i])) {
      continue;
    }
    outAddress = candidate;
    outErrorDetail = String("fallback_openrouter_ipv4:") + kOpenRouterIpv4Fallbacks[i];
#if AI_HAS_ESP_LOG
    ESP_LOGW(kTag,
             "Using literal host fallback host=%s addr=%s",
             host.c_str(),
             kOpenRouterIpv4Fallbacks[i]);
#endif
    return true;
  }

  return false;
}

int readTlsLastError(NetworkClientSecure& client, String& outDetail) {
  char detail[128] = {0};
  const int code = client.lastError(detail, sizeof(detail));
  outDetail = "tls=";
  outDetail += String(code);
  if (detail[0] != '\0') {
    outDetail += ",";
    outDetail += detail;
  }
  return code;
}

bool resolveHostAddress(const String& host, IPAddress& outAddress, String& outErrorDetail) {
  if (host.length() == 0) {
    outErrorDetail = "empty_host";
    return false;
  }

  if (outAddress.fromString(host)) {
    outErrorDetail = "literal_ip";
    return true;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* result = nullptr;
  int rc = lwip_getaddrinfo(host.c_str(), "0", &hints, &result);
  if ((rc != 0 || result == nullptr) && applyEmergencyDnsOverride()) {
    result = nullptr;
    rc = lwip_getaddrinfo(host.c_str(), "0", &hints, &result);
  }

  if (rc != 0 || result == nullptr) {
    outErrorDetail = String("dns_rc=") + String(rc);
#if AI_HAS_ESP32_WIFI && AI_HAS_ESP_LOG
    ESP_LOGW(kTag,
             "Resolve failed host=%s detail=%s dns=%s,%s",
             host.c_str(),
             outErrorDetail.c_str(),
             WiFi.dnsIP(0).toString().c_str(),
             WiFi.dnsIP(1).toString().c_str());
#endif

    if (resolveKnownHostFallback(host, outAddress, outErrorDetail)) {
      return true;
    }

    return false;
  }

  bool hasIpv4 = false;
  bool hasIpv6 = false;
  IPAddress firstIpv4;
  IPAddress firstIpv6;

  for (struct addrinfo* current = result; current != nullptr; current = current->ai_next) {
    if (current->ai_addr == nullptr) {
      continue;
    }

    if (current->ai_family == AF_INET) {
      const auto* ipv4 = reinterpret_cast<const struct sockaddr_in*>(current->ai_addr);
      firstIpv4 = IPAddress(ipv4->sin_addr.s_addr);
      hasIpv4 = true;
      continue;
    }

#if CONFIG_LWIP_IPV6
    if (current->ai_family == AF_INET6) {
      const auto* ipv6 = reinterpret_cast<const struct sockaddr_in6*>(current->ai_addr);
      firstIpv6 = IPAddress(IPv6,
                            ipv6->sin6_addr.un.u8_addr,
                            static_cast<uint8_t>(ipv6->sin6_scope_id & 0xFF));
      hasIpv6 = true;
    }
#endif
  }

  lwip_freeaddrinfo(result);

#if CONFIG_LWIP_IPV6
  if (hasIpv6) {
    outAddress = firstIpv6;
    outErrorDetail = "resolved_ipv6";
    return true;
  }
#endif

  if (hasIpv4) {
    outAddress = firstIpv4;
    outErrorDetail = "resolved_ipv4";
    return true;
  }

  outErrorDetail = "no_supported_addr";
  return false;
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
      preferPsrAm_(true),
      headersParsed_(false),
      chunkedTransfer_(false),
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

  preferPsrAm_ = request.preferPsrAm;
  callbacks_ = callbacks;
  activeResponse_ = AiHttpResponse{};
  completedResponse_ = AiHttpResponse{};
  lastResultOk_ = false;

  headersParsed_ = false;
  headerBuffer_ = "";
  chunkedTransfer_ = false;
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
    preferPsrAm_ = true;
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
    const unsigned long handshakeTimeoutSec = timeoutMs_ > 0 ? ((timeoutMs_ + 999UL) / 1000UL) : 45UL;
    secureClient_.setHandshakeTimeout(handshakeTimeoutSec);

    // Prefer PSRAM allocator before the first TLS attempt to avoid one guaranteed -32512 failure.
    if (request.preferPsrAm) {
      ensureMbedTlsAllocatorForPsrAm();
    }

    activeClient_ = &secureClient_;
  } else {
    activeClient_ = &plainClient_;
  }

  IPAddress resolvedAddress;
  String resolveDetail;
  if (!resolveHostAddress(urlParts.host, resolvedAddress, resolveDetail)) {
    outErrorMessage = "resolve_failed";
    if (resolveDetail.length() > 0) {
      outErrorMessage += ":";
      outErrorMessage += resolveDetail;
    }
    activeClient_ = nullptr;
    return false;
  }

  bool connected = false;
  if (urlParts.tls) {
    connected = secureClient_.connect(resolvedAddress,
                                      urlParts.port,
                                      urlParts.host.c_str(),
                                      nullptr,
                                      nullptr,
                                      nullptr)
                == 1;

    if (!connected) {
      String tlsDetail;
      const int tlsError = readTlsLastError(secureClient_, tlsDetail);
      if (tlsError == -32512 && request.preferPsrAm && ensureMbedTlsAllocatorForPsrAm()) {
#if AI_HAS_ESP_LOG && AI_HAS_ESP32_HEAP_CAPS
        ESP_LOGW(kTag,
           "Retry TLS with PSRAM allocator free8=%u largest8=%u freeInt=%u largestInt=%u freePsram=%u largestPsram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));
#endif
        secureClient_.stop();
        delay(8);
        connected = secureClient_.connect(resolvedAddress,
                                          urlParts.port,
                                          urlParts.host.c_str(),
                                          nullptr,
                                          nullptr,
                                          nullptr)
                    == 1;
      }

      if (!connected && tlsError == -32512) {
        // Give allocator a chance to recover from transient pressure before one last retry.
#if AI_HAS_ESP_LOG && AI_HAS_ESP32_HEAP_CAPS
        ESP_LOGW(kTag,
                 "Last TLS retry after alloc fail freeInt=%u largestInt=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#endif
        secureClient_.stop();
        delay(8);
        connected = secureClient_.connect(resolvedAddress,
                                          urlParts.port,
                                          urlParts.host.c_str(),
                                          nullptr,
                                          nullptr,
                                          nullptr)
                    == 1;
      }

      if (!connected) {
        readTlsLastError(secureClient_, outErrorMessage);
      }
    }
  } else {
    connected = plainClient_.connect(resolvedAddress, urlParts.port) == 1;
  }

  if (!connected) {
    if (outErrorMessage.length() > 0) {
      outErrorMessage = String("connect_failed:") + outErrorMessage;
    } else {
      outErrorMessage = "connect_failed";
    }
    activeClient_ = nullptr;
    return false;
  }

  activeClient_->print(request.method);
  activeClient_->print(" ");
  activeClient_->print(urlParts.path);
  activeClient_->print(" HTTP/1.1\r\nHost: ");
  activeClient_->print(urlParts.host);
  activeClient_->print("\r\nConnection: close\r\n");

  const bool hasInlineBody = request.body.length() > 0;
  const bool hasFileBody = request.bodyFs != nullptr && request.bodyFilePath.length() > 0;

  if (hasInlineBody && hasFileBody) {
    outErrorMessage = "request_body_conflict";
    if (activeClient_ != nullptr) {
      activeClient_->stop();
    }
    activeClient_ = nullptr;
    return false;
  }

  size_t fileBodyBytes = 0;
  File fileBody;
  if (hasFileBody) {
    fileBody = request.bodyFs->open(request.bodyFilePath, FILE_READ);
    if (!fileBody) {
      outErrorMessage = "request_body_file_open_failed";
      if (activeClient_ != nullptr) {
        activeClient_->stop();
      }
      activeClient_ = nullptr;
      return false;
    }

    if (fileBody.isDirectory()) {
      fileBody.close();
      outErrorMessage = "request_body_file_is_directory";
      if (activeClient_ != nullptr) {
        activeClient_->stop();
      }
      activeClient_ = nullptr;
      return false;
    }

    fileBodyBytes = static_cast<size_t>(fileBody.size());
  }

  for (size_t i = 0; i < request.headerCount; ++i) {
    activeClient_->print(request.headers[i].key);
    activeClient_->print(": ");
    activeClient_->print(request.headers[i].value);
    activeClient_->print("\r\n");
  }

  if (hasInlineBody || hasFileBody) {
    const size_t contentLength = hasInlineBody ? request.body.length() : fileBodyBytes;
    activeClient_->print("Content-Length: ");
    activeClient_->print(static_cast<unsigned>(contentLength));
    activeClient_->print("\r\n");
  }

  activeClient_->print("\r\n");

  if (hasInlineBody) {
    activeClient_->print(request.body);
    return true;
  }

  if (!hasFileBody) {
    return true;
  }

  size_t chunkBytes = request.bodyStreamChunkBytes;
  if (chunkBytes < 256) {
    chunkBytes = 256;
  }
  if (chunkBytes > 4096) {
    chunkBytes = 4096;
  }

  uint8_t* buffer = static_cast<uint8_t*>(std::malloc(chunkBytes));
  if (buffer == nullptr) {
    fileBody.close();
    outErrorMessage = "request_body_stream_buffer_alloc_failed";
    if (activeClient_ != nullptr) {
      activeClient_->stop();
    }
    activeClient_ = nullptr;
    return false;
  }

  bool writeOk = true;
  while (fileBody.available()) {
    const size_t readLen = fileBody.read(buffer, chunkBytes);
    if (readLen == 0) {
      break;
    }

    size_t offset = 0;
    while (offset < readLen) {
      const size_t written = activeClient_->write(buffer + offset, readLen - offset);
      if (written == 0) {
        writeOk = false;
        break;
      }
      offset += written;
    }

    if (!writeOk) {
      break;
    }
  }

  std::free(buffer);
  fileBody.close();

  if (!writeOk) {
    outErrorMessage = "request_body_stream_write_failed";
    if (activeClient_ != nullptr) {
      activeClient_->stop();
    }
    activeClient_ = nullptr;
    return false;
  }

  if (request.removeBodyFileAfterSend) {
    request.bodyFs->remove(request.bodyFilePath);
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

  const String transferEncodingHeader = readHeaderValueIgnoreCase(headersPart, "Transfer-Encoding");
  String lowerTransferEncoding = transferEncodingHeader;
  lowerTransferEncoding.toLowerCase();
  chunkedTransfer_ = lowerTransferEncoding.indexOf("chunked") >= 0;
  if (chunkedTransfer_) {
    contentLength_ = -1;
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

  if (!bodyBuffer_.append(data, size, preferPsrAm_)) {
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

  String finalBody = bodyBuffer_.toString();
  String finalError = errorMessage;

  if (chunkedTransfer_ && !eventStreamMode_) {
    String decodedBody;
    if (decodeChunkedBody(finalBody, decodedBody)) {
      finalBody = decodedBody;
    } else {
      ok = false;
      finalError = "chunked_decode_failed";
    }
  }

  activeResponse_.body = finalBody;
  activeResponse_.bodyBytes = bodyBuffer_.size();
  activeResponse_.errorMessage = finalError;

  completedResponse_ = activeResponse_;
  lastResultOk_ = ok;

  if (callbacks_.onDone != nullptr) {
    callbacks_.onDone(completedResponse_, callbacks_.userContext);
  }

  state_ = AsyncState::Idle;
  activeClient_ = nullptr;
  preferPsrAm_ = true;
  callbacks_ = AiTransportCallbacks{};
  activeResponse_ = AiHttpResponse{};

  headerBuffer_ = "";
  headersParsed_ = false;
  chunkedTransfer_ = false;
  eventStreamMode_ = false;
  streamDoneEmitted_ = false;
  eventStreamLineBuffer_ = "";
  contentLength_ = -1;
  bodyReceived_ = 0;
  bodyBuffer_.reset();
}

}  // namespace ai::provider
