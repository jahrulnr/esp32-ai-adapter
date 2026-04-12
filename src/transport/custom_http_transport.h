#pragma once

#include <Arduino.h>
#include <Client.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "core/ai_transport.h"

namespace ai::provider {

class CustomHttpTransport : public IAiTransport {
 public:
  struct Config {
    uint32_t defaultTimeoutMs = 45000;
    bool insecureTls = true;
    size_t pollChunkBytes = 384;
  };

  CustomHttpTransport();
  explicit CustomHttpTransport(const Config& config);

  bool execute(const AiHttpRequest& request, AiHttpResponse& outResponse) override;

  bool supportsAsync() const override;

  AiAsyncSubmitResult executeAsync(const AiHttpRequest& request,
                                   const AiTransportCallbacks& callbacks,
                                   String& outErrorMessage) override;

  void poll() override;

  bool isBusy() const override;

  void cancel() override;

 private:
  struct UrlParts {
    bool tls = false;
    String host;
    uint16_t port = 0;
    String path;
  };

  enum class AsyncState : uint8_t {
    Idle = 0,
    ReadingHeaders,
    ReadingBody,
  };

  class PsrAmBuffer {
   public:
    PsrAmBuffer();
    ~PsrAmBuffer();

    void reset();
    bool append(const char* data, size_t size, bool preferPsrAm);
    size_t size() const;
    String toString() const;

   private:
    char* data_;
    size_t size_;
    size_t capacity_;
  };

  static bool parseUrl(const String& url, UrlParts& outParts, String& outErrorMessage);
  bool connectAndSend(const AiHttpRequest& request,
                      const UrlParts& urlParts,
                      String& outErrorMessage);
  static String readHeaderValueIgnoreCase(const String& headers, const char* name);
  static String extractTextDeltaFromEventJson(const String& payload,
                                              bool& outDone,
                                              String& outDoneReason);

  void parseHeadersIfReady();
  void handleBodyChunk(const char* data, size_t size);
  void parseEventStreamLines(const char* data, size_t size);
  void parseEventStreamData(const String& payload);
  void emitChunkCallback(const AiStreamChunk& chunk) const;
  void finishRequest(bool ok, const String& errorMessage = String());
  bool hasTimedOut(uint32_t nowMs) const;

  Config config_;

  WiFiClient plainClient_;
  WiFiClientSecure secureClient_;
  Client* activeClient_;

  AsyncState state_;
  bool preferPsrAm_;
  AiHttpResponse activeResponse_;
  AiTransportCallbacks callbacks_;

  String headerBuffer_;
  bool headersParsed_;
  bool chunkedTransfer_;
  bool eventStreamMode_;
  bool streamDoneEmitted_;
  String eventStreamLineBuffer_;
  int contentLength_;
  size_t bodyReceived_;
  PsrAmBuffer bodyBuffer_;

  uint32_t startedAtMs_;
  uint32_t lastActivityMs_;
  uint32_t timeoutMs_;

  bool lastResultOk_;
  AiHttpResponse completedResponse_;
};

}  // namespace ai::provider
