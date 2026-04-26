#include "core/ai_chat_session_store.h"

#include <ArduinoJson.h>
#include <SpiJsonDocument.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>

namespace ai::provider {
namespace {

bool lockSemaphore(SemaphoreHandle_t mutex) {
  if (mutex == nullptr) {
    return false;
  }
  return xSemaphoreTake(mutex, pdMS_TO_TICKS(250)) == pdTRUE;
}

void unlockSemaphore(SemaphoreHandle_t mutex) {
  if (mutex != nullptr) {
    xSemaphoreGive(mutex);
  }
}

}  // namespace

AiChatSessionStore::AiChatSessionStore(const AiChatSessionStoreConfig& config)
    : storage_(nullptr),
      mutex_(nullptr),
      records_{},
      lastCleanupAtMs_(0),
      config_{},
      memoryPath_("/cache/chat_sessions"),
      skillsPath_("/skills"),
      recordLimit_(kMaxTrackedSessions) {
  applyConfigLocked(config);
}

void AiChatSessionStore::setConfig(const AiChatSessionStoreConfig& config) {
  if (!lockSemaphore(mutex_)) {
    applyConfigLocked(config);
    return;
  }

  applyConfigLocked(config);
  unlockSemaphore(mutex_);
}

void AiChatSessionStore::setStorage(fs::FS* storage) {
  storage_ = storage;
}

void AiChatSessionStore::setMemoryPath(const String& memoryPath) {
  if (memoryPath.length() == 0) {
    return;
  }

  if (!lockSemaphore(mutex_)) {
    memoryPath_ = memoryPath;
    return;
  }

  memoryPath_ = memoryPath;
  unlockSemaphore(mutex_);
}

void AiChatSessionStore::setSkillsPath(const String& skillsPath) {
  if (skillsPath.length() == 0) {
    return;
  }

  if (!lockSemaphore(mutex_)) {
    skillsPath_ = skillsPath;
    return;
  }

  skillsPath_ = skillsPath;
  unlockSemaphore(mutex_);
}

bool AiChatSessionStore::begin() {
  if (storage_ == nullptr) {
    return false;
  }

  if (mutex_ == nullptr) {
    mutex_ = xSemaphoreCreateMutex();
  }
  if (mutex_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  const bool ok = ensureDirectoryLocked();
  unlockSemaphore(mutex_);
  return ok;
}

bool AiChatSessionStore::openChat(const char* sessionId, uint32_t nowMs) {
  return touch(sessionId, nowMs);
}

bool AiChatSessionStore::touch(const char* sessionId, uint32_t nowMs) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findOrCreateSessionLocked(sessionId, nowMs);
  if (meta == nullptr) {
    unlockSemaphore(mutex_);
    return false;
  }

  meta->lastActiveMs = nowMs;
  meta->expiresAtMs = nowMs + config_.expiryMs;
  unlockSemaphore(mutex_);
  return true;
}

bool AiChatSessionStore::markInFlight(const char* sessionId, int delta, uint32_t nowMs) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findOrCreateSessionLocked(sessionId, nowMs);
  if (meta == nullptr) {
    unlockSemaphore(mutex_);
    return false;
  }

  if (delta > 0) {
    const uint32_t increased = static_cast<uint32_t>(meta->inFlight) + static_cast<uint32_t>(delta);
    meta->inFlight = static_cast<uint16_t>(increased > 0xFFFFU ? 0xFFFFU : increased);
  } else if (delta < 0) {
    const uint16_t decrement = static_cast<uint16_t>(-delta);
    meta->inFlight = meta->inFlight > decrement ? static_cast<uint16_t>(meta->inFlight - decrement) : 0;
  }

  meta->lastActiveMs = nowMs;
  meta->expiresAtMs = nowMs + config_.expiryMs;
  if (meta->inFlight == 0 && config_.compactEveryTurns > 0) {
    compactSessionLocked(*meta, config_.retainedTurnsAfterCompact);
  }
  unlockSemaphore(mutex_);
  return true;
}

bool AiChatSessionStore::appendTurn(const char* sessionId,
                                    const char* role,
                                    const String& text,
                                    uint32_t nowMs) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findOrCreateSessionLocked(sessionId, nowMs);
  if (meta == nullptr || !ensureDirectoryLocked()) {
    unlockSemaphore(mutex_);
    return false;
  }

  String payload = truncateText(text, config_.maxTextChars);
  if (payload.length() == 0) {
    unlockSemaphore(mutex_);
    return true;
  }

  const String path = sessionPathLocked(meta->id);
  File file = storage_->open(path.c_str(), FILE_APPEND);
  if (!file) {
    unlockSemaphore(mutex_);
    return false;
  }

  SpiJsonDocument line;
  line["tsMs"] = nowMs;
  line["role"] = normalizedRoleText(role);
  line["text"] = payload;

  String serialized;
  serializeJson(line, serialized);
  const size_t printed = file.print(serialized);
  file.print('\n');
  file.close();

  if (printed == 0) {
    unlockSemaphore(mutex_);
    return false;
  }

  meta->lastActiveMs = nowMs;
  meta->expiresAtMs = nowMs + config_.expiryMs;
  refreshBytesLocked(*meta);

  const bool hardLimitExceeded = computeTotalBytesLocked() > config_.hardTotalBytes;

  if (meta->bytes > config_.softFileBytes) {
    compactSessionLocked(*meta, config_.retainedTurnsAfterCompact);
  }

  unlockSemaphore(mutex_);

  if (hardLimitExceeded) {
    runMandatoryCleanup(nowMs);
  }

  return true;
}

bool AiChatSessionStore::buildContextMessages(const char* sessionId,
                                              AiInvokeRequest& request,
                                              uint8_t maxTurns) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findSessionLocked(sessionId);
  if (meta == nullptr) {
    unlockSemaphore(mutex_);
    return false;
  }

  constexpr size_t kMaxTurns = 12;
  TurnRecord turns[kMaxTurns] = {};
  const size_t turnCap = maxTurns > kMaxTurns ? kMaxTurns : static_cast<size_t>(maxTurns);
  size_t turnCount = 0;
  uint32_t totalTurns = 0;
  const bool loaded = loadTailTurnsLocked(*meta, turns, turnCap, turnCount, totalTurns);
  if (!loaded) {
    unlockSemaphore(mutex_);
    return false;
  }

  const String prompt = request.prompt;
  const String systemPrompt = request.systemPrompt;

  request.messageCount = 0;
  if (systemPrompt.length() > 0) {
    request.addMessage(AiMessageRole::System, systemPrompt);
  }

  if (totalTurns > turnCount && turnCount > 0) {
    request.addMessage(AiMessageRole::Assistant,
                       String("Summary: ") + String(totalTurns - turnCount) +
                           String(" earlier turns were compacted. Use latest context."));
  }

  for (size_t i = 0; i < turnCount; ++i) {
    if (!request.addMessage(roleFromText(turns[i].role), turns[i].text)) {
      break;
    }
  }

  request.addMessage(AiMessageRole::User, prompt);

  meta->lastActiveMs = millis();
  meta->expiresAtMs = meta->lastActiveMs + config_.expiryMs;

  unlockSemaphore(mutex_);
  return true;
}

bool AiChatSessionStore::compactChat(const char* sessionId, uint8_t retainTurns) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findSessionLocked(sessionId);
  if (meta == nullptr) {
    unlockSemaphore(mutex_);
    return false;
  }

  if (retainTurns == 0) {
    retainTurns = config_.retainedTurnsAfterCompact;
  }

  const bool ok = compactSessionLocked(*meta, retainTurns);
  unlockSemaphore(mutex_);
  return ok;
}

bool AiChatSessionStore::deleteChat(const char* sessionId) {
  return reset(sessionId);
}

bool AiChatSessionStore::reset(const char* sessionId) {
  if (sessionId == nullptr || sessionId[0] == '\0' || storage_ == nullptr) {
    return false;
  }

  if (!lockSemaphore(mutex_)) {
    return false;
  }

  SessionMeta* meta = findSessionLocked(sessionId);
  if (meta == nullptr) {
    unlockSemaphore(mutex_);
    return true;
  }

  if (meta->inFlight > 0) {
    unlockSemaphore(mutex_);
    return false;
  }

  const String path = sessionPathLocked(meta->id);
  if (storage_->exists(path.c_str())) {
    storage_->remove(path.c_str());
  }

  *meta = SessionMeta{};
  unlockSemaphore(mutex_);
  return true;
}

void AiChatSessionStore::cleanupChat(uint32_t nowMs) {
  runMandatoryCleanup(nowMs);
}

void AiChatSessionStore::runMandatoryCleanup(uint32_t nowMs) {
  if (storage_ == nullptr || mutex_ == nullptr) {
    return;
  }

  if (!lockSemaphore(mutex_)) {
    return;
  }

  if ((nowMs - lastCleanupAtMs_) < config_.cleanupIntervalMs) {
    unlockSemaphore(mutex_);
    return;
  }
  lastCleanupAtMs_ = nowMs;

  for (size_t i = 0; i < recordLimit_; ++i) {
    auto& meta = records_[i];
    if (!meta.used) {
      continue;
    }

    const bool expired = static_cast<int32_t>(nowMs - meta.expiresAtMs) >= 0;
    if (expired && meta.inFlight == 0) {
      const String path = sessionPathLocked(meta.id);
      if (storage_->exists(path.c_str())) {
        storage_->remove(path.c_str());
      }
      meta = SessionMeta{};
      continue;
    }

    refreshBytesLocked(meta);
    if (meta.bytes > config_.softFileBytes) {
      compactSessionLocked(meta, config_.retainedTurnsAfterCompact);
      refreshBytesLocked(meta);
    }
  }

  uint32_t totalBytes = computeTotalBytesLocked();
  if (totalBytes > config_.hardTotalBytes) {
    for (size_t i = 0; i < recordLimit_; ++i) {
      auto& meta = records_[i];
      if (!meta.used || meta.inFlight > 0) {
        continue;
      }
      compactSessionLocked(meta, config_.retainedTurnsAfterCompact);
      refreshBytesLocked(meta);
      totalBytes = computeTotalBytesLocked();
      if (totalBytes <= config_.hardTotalBytes) {
        break;
      }
    }

    if (totalBytes > config_.hardTotalBytes && storage_->exists(memoryPath_.c_str())) {
      File directory = storage_->open(memoryPath_.c_str(), FILE_READ);
      if (directory) {
        File entry = directory.openNextFile();
        while (entry && totalBytes > config_.hardTotalBytes) {
          if (!entry.isDirectory()) {
            String name = entry.name();
            const uint32_t entrySize = static_cast<uint32_t>(entry.size());
            const int slash = name.lastIndexOf('/');
            if (slash >= 0) {
              name = name.substring(static_cast<size_t>(slash + 1));
            }

            if (name.endsWith(".jsonl")) {
              String sessionId = name.substring(0, name.length() - 6);
              if (!hasTrackedSessionLocked(sessionId.c_str())) {
                String path = memoryPath_ + String("/") + name;
                storage_->remove(path.c_str());
                totalBytes = totalBytes > entrySize ? totalBytes - entrySize : 0;
              }
            }
          }

          entry.close();
          entry = directory.openNextFile();
        }
        directory.close();
      }
    }
  }

  unlockSemaphore(mutex_);
}

IAiChatSessionStore::Status AiChatSessionStore::chatDetails() const {
  return status();
}

IAiChatSessionStore::Status AiChatSessionStore::status() const {
  Status out;
  if (storage_ == nullptr || mutex_ == nullptr) {
    return out;
  }

  if (!lockSemaphore(mutex_)) {
    return out;
  }

  for (size_t i = 0; i < recordLimit_; ++i) {
    if (records_[i].used) {
      ++out.sessionCount;
    }
  }
  out.totalBytes = computeTotalBytesLocked();

  unlockSemaphore(mutex_);
  return out;
}

void AiChatSessionStore::applyConfigLocked(const AiChatSessionStoreConfig& config) {
  config_ = config;

  memoryPath_ = (config.memoryPath != nullptr && config.memoryPath[0] != '\0')
                    ? String(config.memoryPath)
                    : String("/cache/chat_sessions");
  skillsPath_ = (config.skillsPath != nullptr && config.skillsPath[0] != '\0')
                    ? String(config.skillsPath)
                    : String("/skills");

  if (config_.maxRecords == 0) {
    config_.maxRecords = 1;
  }
  if (config_.maxRecords > kMaxTrackedSessions) {
    config_.maxRecords = kMaxTrackedSessions;
  }

  if (config_.retainedTurnsAfterCompact == 0) {
    config_.retainedTurnsAfterCompact = 1;
  }
  if (config_.summaryMaxChars == 0) {
    config_.summaryMaxChars = 256;
  }

  recordLimit_ = config_.maxRecords;
}

AiChatSessionStore::SessionMeta* AiChatSessionStore::findOrCreateSessionLocked(const char* sessionId,
                                                                                uint32_t nowMs) {
  SessionMeta* existing = findSessionLocked(sessionId);
  if (existing != nullptr) {
    return existing;
  }

  for (size_t i = 0; i < recordLimit_; ++i) {
    auto& meta = records_[i];
    if (meta.used) {
      continue;
    }

    meta = SessionMeta{};
    meta.used = true;
    strncpy(meta.id, sessionId, sizeof(meta.id) - 1);
    meta.id[sizeof(meta.id) - 1] = '\0';
    meta.lastActiveMs = nowMs;
    meta.expiresAtMs = nowMs + config_.expiryMs;
    meta.bytes = 0;
    return &meta;
  }

  SessionMeta* evicted = evictCandidateLocked(nowMs);
  if (evicted == nullptr) {
    return nullptr;
  }

  const String oldPath = sessionPathLocked(evicted->id);
  if (storage_->exists(oldPath.c_str())) {
    storage_->remove(oldPath.c_str());
  }

  *evicted = SessionMeta{};
  evicted->used = true;
  strncpy(evicted->id, sessionId, sizeof(evicted->id) - 1);
  evicted->id[sizeof(evicted->id) - 1] = '\0';
  evicted->lastActiveMs = nowMs;
  evicted->expiresAtMs = nowMs + config_.expiryMs;
  return evicted;
}

AiChatSessionStore::SessionMeta* AiChatSessionStore::findSessionLocked(const char* sessionId) {
  for (size_t i = 0; i < recordLimit_; ++i) {
    auto& meta = records_[i];
    if (!meta.used) {
      continue;
    }
    if (strncmp(meta.id, sessionId, sizeof(meta.id)) == 0) {
      return &meta;
    }
  }
  return nullptr;
}

bool AiChatSessionStore::hasTrackedSessionLocked(const char* sessionId) const {
  if (sessionId == nullptr || sessionId[0] == '\0') {
    return false;
  }

  for (size_t i = 0; i < recordLimit_; ++i) {
    const auto& meta = records_[i];
    if (!meta.used) {
      continue;
    }
    if (strncmp(meta.id, sessionId, sizeof(meta.id)) == 0) {
      return true;
    }
  }

  return false;
}

AiChatSessionStore::SessionMeta* AiChatSessionStore::evictCandidateLocked(uint32_t nowMs) {
  SessionMeta* candidate = nullptr;
  for (size_t i = 0; i < recordLimit_; ++i) {
    auto& meta = records_[i];
    if (!meta.used || meta.inFlight > 0) {
      continue;
    }

    if (candidate == nullptr) {
      candidate = &meta;
      continue;
    }

    const bool metaExpired = static_cast<int32_t>(nowMs - meta.expiresAtMs) >= 0;
    const bool candidateExpired = static_cast<int32_t>(nowMs - candidate->expiresAtMs) >= 0;
    if (metaExpired && !candidateExpired) {
      candidate = &meta;
      continue;
    }

    if (meta.lastActiveMs < candidate->lastActiveMs) {
      candidate = &meta;
    }
  }
  return candidate;
}

bool AiChatSessionStore::ensureDirectoryLocked() const {
  if (storage_ == nullptr || memoryPath_.length() == 0) {
    return false;
  }

  if (storage_->exists(memoryPath_.c_str())) {
    return true;
  }

  return storage_->mkdir(memoryPath_.c_str());
}

String AiChatSessionStore::sessionPathLocked(const char* sessionId) const {
  String path = memoryPath_;
  path += "/";
  path += sessionId;
  path += ".jsonl";
  return path;
}

String AiChatSessionStore::tempPathLocked(const char* sessionId) const {
  String path = memoryPath_;
  path += "/.";
  path += sessionId;
  path += ".tmp";
  return path;
}

uint32_t AiChatSessionStore::fileSizeLocked(const char* path) const {
  if (storage_ == nullptr || path == nullptr || !storage_->exists(path)) {
    return 0;
  }

  File file = storage_->open(path, FILE_READ);
  if (!file) {
    return 0;
  }

  const uint32_t size = static_cast<uint32_t>(file.size());
  file.close();
  return size;
}

uint32_t AiChatSessionStore::computeTotalBytesLocked() const {
  if (storage_ == nullptr || !storage_->exists(memoryPath_.c_str())) {
    return 0;
  }

  uint32_t total = 0;
  File directory = storage_->open(memoryPath_.c_str(), FILE_READ);
  if (!directory) {
    return 0;
  }

  File entry = directory.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      total += static_cast<uint32_t>(entry.size());
    }
    entry.close();
    entry = directory.openNextFile();
  }

  directory.close();
  return total;
}

uint32_t AiChatSessionStore::refreshBytesLocked(SessionMeta& meta) {
  const String path = sessionPathLocked(meta.id);
  meta.bytes = fileSizeLocked(path.c_str());
  return meta.bytes;
}

bool AiChatSessionStore::compactSessionLocked(SessionMeta& meta, uint8_t retainTurns) {
  if (!meta.used || storage_ == nullptr || retainTurns == 0) {
    return false;
  }

  const String srcPath = sessionPathLocked(meta.id);
  if (!storage_->exists(srcPath.c_str())) {
    meta.bytes = 0;
    return true;
  }

  constexpr size_t kMaxTurns = 16;
  TurnRecord turns[kMaxTurns] = {};
  size_t count = 0;
  uint32_t totalTurns = 0;
  if (!loadTailTurnsLocked(meta,
                           turns,
                           retainTurns > kMaxTurns ? kMaxTurns : retainTurns,
                           count,
                           totalTurns)) {
    return false;
  }

  if (totalTurns <= count) {
    meta.lastCompactedAtMs = millis();
    return true;
  }

  const String tmpPath = tempPathLocked(meta.id);
  if (storage_->exists(tmpPath.c_str())) {
    storage_->remove(tmpPath.c_str());
  }

  File tmp = storage_->open(tmpPath.c_str(), FILE_WRITE);
  if (!tmp) {
    return false;
  }

  String compactSummary;
  if (!buildCompactSummaryLocked(meta, totalTurns - count, compactSummary)) {
    compactSummary = String("Summary: ") + String(totalTurns - count) +
                     String(" older turns compacted to preserve memory.");
  }

  SpiJsonDocument summary;
  summary["tsMs"] = millis();
  summary["role"] = "assistant";
  summary["text"] = compactSummary;
  String line;
  serializeJson(summary, line);
  tmp.print(line);
  tmp.print('\n');

  for (size_t i = 0; i < count; ++i) {
    SpiJsonDocument record;
    record["tsMs"] = turns[i].tsMs;
    record["role"] = turns[i].role;
    record["text"] = turns[i].text;
    line = "";
    serializeJson(record, line);
    tmp.print(line);
    tmp.print('\n');
  }

  tmp.close();

  storage_->remove(srcPath.c_str());
  if (!storage_->rename(tmpPath.c_str(), srcPath.c_str())) {
    storage_->remove(tmpPath.c_str());
    return false;
  }

  meta.lastCompactedAtMs = millis();
  meta.bytes = fileSizeLocked(srcPath.c_str());
  return true;
}

bool AiChatSessionStore::buildCompactSummaryLocked(const SessionMeta& meta,
                                                   size_t olderTurnCount,
                                                   String& outSummary) {
  outSummary = "";
  if (!meta.used || storage_ == nullptr || olderTurnCount == 0) {
    return false;
  }

  const String path = sessionPathLocked(meta.id);
  File file = storage_->open(path.c_str(), FILE_READ);
  if (!file) {
    return false;
  }

  outSummary.reserve(config_.summaryMaxChars + 16U);
  outSummary = "Summary: ";
  size_t consumed = 0;
  while (file.available() && consumed < olderTurnCount) {
    const String line = file.readStringUntil('\n');
    TurnRecord parsed;
    if (!parseTurnLine(line, parsed)) {
      continue;
    }
    ++consumed;

    String item = parsed.role;
    item += ": ";
    item += parsed.text;
    item.replace('\n', ' ');
    item.replace('\r', ' ');
    item.trim();
    if (item.length() == 0) {
      continue;
    }

    if (outSummary.length() > 9) {
      outSummary += " | ";
    }
    outSummary += item;
    if (outSummary.length() >= config_.summaryMaxChars) {
      outSummary.remove(config_.summaryMaxChars);
      outSummary.trim();
      break;
    }
  }

  file.close();

  if (outSummary.length() <= 9) {
    outSummary = String("Summary: ") + String(olderTurnCount) +
                 String(" older turns compacted.");
  }
  return true;
}

bool AiChatSessionStore::loadTailTurnsLocked(const SessionMeta& meta,
                                             TurnRecord* outTurns,
                                             size_t maxTurns,
                                             size_t& outCount,
                                             uint32_t& outTotalTurns) {
  outCount = 0;
  outTotalTurns = 0;
  if (!meta.used || storage_ == nullptr || outTurns == nullptr || maxTurns == 0) {
    return false;
  }

  const String path = sessionPathLocked(meta.id);
  File file = storage_->open(path.c_str(), FILE_READ);
  if (!file) {
    return true;
  }

  size_t writeIndex = 0;
  while (file.available()) {
    const String line = file.readStringUntil('\n');
    TurnRecord parsed;
    if (!parseTurnLine(line, parsed)) {
      continue;
    }

    outTurns[writeIndex % maxTurns] = parsed;
    ++writeIndex;
    ++outTotalTurns;
  }
  file.close();

  const size_t stored = writeIndex < maxTurns ? writeIndex : maxTurns;
  if (stored == 0) {
    outCount = 0;
    return true;
  }

  if (writeIndex <= maxTurns) {
    outCount = stored;
    return true;
  }

  TurnRecord ordered[16] = {};
  const size_t start = writeIndex % maxTurns;
  for (size_t i = 0; i < stored; ++i) {
    ordered[i] = outTurns[(start + i) % maxTurns];
  }
  for (size_t i = 0; i < stored; ++i) {
    outTurns[i] = ordered[i];
  }

  outCount = stored;
  return true;
}

bool AiChatSessionStore::parseTurnLine(const String& line, TurnRecord& outTurn) const {
  if (line.length() == 0) {
    return false;
  }

  SpiJsonDocument doc;
  const auto err = deserializeJson(doc, line);
  if (err) {
    return false;
  }

  const char* role = doc["role"].as<const char*>();
  const char* text = doc["text"].as<const char*>();
  if (role == nullptr || text == nullptr || text[0] == '\0') {
    return false;
  }

  outTurn.role = role;
  outTurn.text = truncateText(String(text), config_.maxTextChars);
  outTurn.tsMs = doc["tsMs"].as<uint32_t>();
  return outTurn.text.length() > 0;
}

AiMessageRole AiChatSessionStore::roleFromText(const String& role) {
  if (role.equalsIgnoreCase("system")) {
    return AiMessageRole::System;
  }
  if (role.equalsIgnoreCase("assistant")) {
    return AiMessageRole::Assistant;
  }
  if (role.equalsIgnoreCase("tool")) {
    return AiMessageRole::Tool;
  }

  return AiMessageRole::User;
}

const char* AiChatSessionStore::normalizedRoleText(const char* role) {
  if (role == nullptr) {
    return "user";
  }

  if (strcmp(role, "system") == 0 || strcmp(role, "assistant") == 0 || strcmp(role, "tool") == 0) {
    return role;
  }

  return "user";
}

String AiChatSessionStore::truncateText(const String& text, size_t maxChars) {
  if (text.length() <= maxChars) {
    return text;
  }

  String out = text;
  out.remove(maxChars);
  out.trim();
  return out;
}

}  // namespace ai::provider
