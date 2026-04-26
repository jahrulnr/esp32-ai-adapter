#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/semphr.h>

#include <array>

#include "core/ai_provider_types.h"

namespace ai::provider {

struct AiChatSessionStoreConfig {
  const char* memoryPath = "/cache/chat_sessions";
  const char* skillsPath = "/skills";
  uint8_t maxRecords = 8;
  uint32_t expiryMs = 30U * 60U * 1000U;
  uint32_t cleanupIntervalMs = 30000U;
  uint32_t softFileBytes = 24U * 1024U;
  uint32_t hardTotalBytes = 256U * 1024U;
  uint8_t retainedTurnsAfterCompact = 10;
  uint8_t compactEveryTurns = 2;
  uint16_t summaryMaxChars = 768;
  uint16_t maxTextChars = 512;
};

class IAiChatSessionStore {
 public:
  struct Status {
    uint32_t sessionCount = 0;
    uint32_t totalBytes = 0;
  };

  virtual ~IAiChatSessionStore() = default;

  virtual void setStorage(fs::FS* storage) = 0;
  virtual void setMemoryPath(const String& memoryPath) = 0;
  virtual void setSkillsPath(const String& skillsPath) = 0;
  virtual bool begin() = 0;

  virtual bool openChat(const char* sessionId, uint32_t nowMs) = 0;
  virtual bool touch(const char* sessionId, uint32_t nowMs) = 0;
  virtual bool markInFlight(const char* sessionId, int delta, uint32_t nowMs) = 0;
  virtual bool appendTurn(const char* sessionId,
                          const char* role,
                          const String& text,
                          uint32_t nowMs) = 0;
  virtual bool buildContextMessages(const char* sessionId,
                                    AiInvokeRequest& request,
                                    uint8_t maxTurns) = 0;
  virtual bool compactChat(const char* sessionId, uint8_t retainTurns) = 0;
  virtual bool deleteChat(const char* sessionId) = 0;
  virtual bool reset(const char* sessionId) = 0;

  virtual void cleanupChat(uint32_t nowMs) = 0;
  virtual void runMandatoryCleanup(uint32_t nowMs) = 0;

  virtual Status chatDetails() const = 0;
  virtual Status status() const = 0;
};

class AiChatSessionStore : public IAiChatSessionStore {
 public:
  static constexpr uint8_t kMaxTrackedSessions = 8;

  explicit AiChatSessionStore(const AiChatSessionStoreConfig& config = AiChatSessionStoreConfig{});

  void setConfig(const AiChatSessionStoreConfig& config);

  void setStorage(fs::FS* storage) override;
  void setMemoryPath(const String& memoryPath) override;
  void setSkillsPath(const String& skillsPath) override;
  bool begin() override;

  bool openChat(const char* sessionId, uint32_t nowMs) override;
  bool touch(const char* sessionId, uint32_t nowMs) override;
  bool markInFlight(const char* sessionId, int delta, uint32_t nowMs) override;
  bool appendTurn(const char* sessionId,
                  const char* role,
                  const String& text,
                  uint32_t nowMs) override;
  bool buildContextMessages(const char* sessionId,
                            AiInvokeRequest& request,
                            uint8_t maxTurns) override;
  bool compactChat(const char* sessionId, uint8_t retainTurns) override;
  bool deleteChat(const char* sessionId) override;
  bool reset(const char* sessionId) override;

  void cleanupChat(uint32_t nowMs) override;
  void runMandatoryCleanup(uint32_t nowMs) override;

  Status chatDetails() const override;
  Status status() const override;

 private:
  struct SessionMeta {
    bool used = false;
    char id[24] = {0};
    uint32_t lastActiveMs = 0;
    uint32_t expiresAtMs = 0;
    uint16_t inFlight = 0;
    uint32_t bytes = 0;
    uint32_t lastCompactedAtMs = 0;
  };

  struct TurnRecord {
    String role;
    String text;
    uint32_t tsMs = 0;
  };

  fs::FS* storage_;
  mutable SemaphoreHandle_t mutex_;
  std::array<SessionMeta, kMaxTrackedSessions> records_;
  uint32_t lastCleanupAtMs_;
  AiChatSessionStoreConfig config_;
  String memoryPath_;
  String skillsPath_;
  uint8_t recordLimit_;

  void applyConfigLocked(const AiChatSessionStoreConfig& config);

  SessionMeta* findOrCreateSessionLocked(const char* sessionId, uint32_t nowMs);
  SessionMeta* findSessionLocked(const char* sessionId);
  bool hasTrackedSessionLocked(const char* sessionId) const;
  SessionMeta* evictCandidateLocked(uint32_t nowMs);

  bool ensureDirectoryLocked() const;
  String sessionPathLocked(const char* sessionId) const;
  String tempPathLocked(const char* sessionId) const;
  uint32_t fileSizeLocked(const char* path) const;
  uint32_t computeTotalBytesLocked() const;
  uint32_t refreshBytesLocked(SessionMeta& meta);

  bool compactSessionLocked(SessionMeta& meta, uint8_t retainTurns);
  bool buildCompactSummaryLocked(const SessionMeta& meta,
                                 size_t olderTurnCount,
                                 String& outSummary);
  bool loadTailTurnsLocked(const SessionMeta& meta,
                           TurnRecord* outTurns,
                           size_t maxTurns,
                           size_t& outCount,
                           uint32_t& outTotalTurns);
  bool parseTurnLine(const String& line, TurnRecord& outTurn) const;

  static AiMessageRole roleFromText(const String& role);
  static const char* normalizedRoleText(const char* role);
  static String truncateText(const String& text, size_t maxChars);
};

}  // namespace ai::provider
