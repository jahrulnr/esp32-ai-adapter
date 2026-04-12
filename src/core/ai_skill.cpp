#include "core/ai_skill.h"

#include <ArduinoJson.h>
#include <Preferences.h>

#include <array>
#include <cstdlib>

#if defined(ESP32)
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#endif

namespace ai::provider {

namespace {

constexpr const char* kSkillsRootKey = "skills";
constexpr const char* kSkillIdKey = "id";
constexpr const char* kSkillNameKey = "name";
constexpr const char* kSkillDescriptionKey = "description";
constexpr const char* kSkillInstructionsKey = "instructions";

void* allocateBuffer(size_t size, bool preferPsrAm) {
#if defined(ESP32)
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

void freeBuffer(void* ptr) {
  if (ptr == nullptr) {
    return;
  }
#if defined(ESP32)
  heap_caps_free(ptr);
#else
  std::free(ptr);
#endif
}

}  // namespace

bool Skill::ConfigureMemory(const AiSkillMemoryConfig& config, String& outErrorMessage) {
  outErrorMessage = String();
  preferPsrAm_ = config.preferPsrAm;
  extmemAlwaysInternalThreshold_ = config.extmemAlwaysInternalThreshold;
  psramPreferenceApplied_ = false;

  return applyPsramPreference(outErrorMessage);
}

bool Skill::Add(const AiSkillItem& item, String& outErrorMessage) {
  outErrorMessage = String();

  if (!applyPsramPreference(outErrorMessage)) {
    return false;
  }

  if (item.id.length() == 0) {
    outErrorMessage = "Skill id is required";
    return false;
  }

  if (item.name.length() == 0) {
    outErrorMessage = "Skill name is required";
    return false;
  }

  Entry* existing = findMutableById(item.id);
  if (existing != nullptr) {
    outErrorMessage = "Skill already exists";
    return false;
  }

  for (Entry& entry : entries_) {
    if (entry.used) {
      continue;
    }

    entry.used = true;
    if (!copyItem(entry.item, item, outErrorMessage)) {
      entry = Entry{};
      return false;
    }
    return true;
  }

  outErrorMessage = "Skill storage is full";
  return false;
}

bool Skill::List(AiSkillList& outList) const {
  outList.count = 0;

  for (const Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }

    if (outList.count >= outList.items.size()) {
      return false;
    }

    outList.items[outList.count++] = entry.item;
  }

  return true;
}

bool Skill::Get(const String& skillId, AiSkillItem& outItem) const {
  const Entry* entry = findById(skillId);
  if (entry == nullptr) {
    return false;
  }

  outItem = entry->item;
  return true;
}

bool Skill::Remove(const String& skillId) {
  Entry* entry = findMutableById(skillId);
  if (entry == nullptr) {
    return false;
  }

  *entry = Entry{};
  return true;
}

bool Skill::Update(const String& skillId,
                   const AiSkillItem& updatedItem,
                   String& outErrorMessage) {
  outErrorMessage = String();

  if (!applyPsramPreference(outErrorMessage)) {
    return false;
  }

  if (updatedItem.id.length() == 0) {
    outErrorMessage = "Skill id is required";
    return false;
  }

  if (updatedItem.name.length() == 0) {
    outErrorMessage = "Skill name is required";
    return false;
  }

  Entry* current = findMutableById(skillId);
  if (current == nullptr) {
    outErrorMessage = "Skill not found";
    return false;
  }

  if (!skillId.equalsIgnoreCase(updatedItem.id)) {
    const Entry* duplicate = findById(updatedItem.id);
    if (duplicate != nullptr) {
      outErrorMessage = "Target skill id already exists";
      return false;
    }
  }

  AiSkillItem replaced;
  if (!copyItem(replaced, updatedItem, outErrorMessage)) {
    return false;
  }

  current->item = replaced;
  return true;
}

bool Skill::SaveToFs(fs::FS& filesystem, const String& path, String& outErrorMessage) const {
  outErrorMessage = String();

  if (path.length() == 0) {
    outErrorMessage = "FS path is required";
    return false;
  }

  String payload;
  if (!encodeJson(payload, outErrorMessage)) {
    return false;
  }

  File file = filesystem.open(path, "w");
  if (!file) {
    outErrorMessage = String("Unable to open file for write: ") + path;
    return false;
  }

  const size_t expected = payload.length();
  const size_t written = file.print(payload);
  file.flush();
  file.close();

  if (written != expected) {
    outErrorMessage = "Failed to write complete skill payload to FS";
    return false;
  }

  return true;
}

bool Skill::LoadFromFs(fs::FS& filesystem, const String& path, String& outErrorMessage) {
  outErrorMessage = String();

  if (path.length() == 0) {
    outErrorMessage = "FS path is required";
    return false;
  }

  File file = filesystem.open(path, "r");
  if (!file) {
    outErrorMessage = String("Unable to open file for read: ") + path;
    return false;
  }

  String payload = file.readString();
  file.close();

  if (payload.length() == 0) {
    outErrorMessage = "Skill file is empty";
    return false;
  }

  return decodeJson(payload, outErrorMessage);
}

bool Skill::SaveToNvs(const String& nameSpace,
                     const String& key,
                     const size_t maxBytes,
                     String& outErrorMessage) const {
  outErrorMessage = String();

  if (nameSpace.length() == 0 || key.length() == 0) {
    outErrorMessage = "NVS namespace and key are required";
    return false;
  }

  if (maxBytes == 0) {
    outErrorMessage = "NVS maxBytes must be greater than zero";
    return false;
  }

  String payload;
  if (!encodeJson(payload, outErrorMessage)) {
    return false;
  }

  const size_t payloadBytes = payload.length() + 1;
  if (payloadBytes > maxBytes) {
    outErrorMessage = "Skill payload exceeds NVS maxBytes";
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(nameSpace.c_str(), false)) {
    outErrorMessage = "Failed to open NVS namespace for write";
    return false;
  }

  const size_t written = prefs.putBytes(key.c_str(), payload.c_str(), payloadBytes);
  prefs.end();

  if (written != payloadBytes) {
    outErrorMessage = "Failed to persist skill payload to NVS";
    return false;
  }

  return true;
}

bool Skill::LoadFromNvs(const String& nameSpace,
                       const String& key,
                       const size_t maxBytes,
                       String& outErrorMessage) {
  outErrorMessage = String();

  if (nameSpace.length() == 0 || key.length() == 0) {
    outErrorMessage = "NVS namespace and key are required";
    return false;
  }

  if (maxBytes == 0) {
    outErrorMessage = "NVS maxBytes must be greater than zero";
    return false;
  }

  Preferences prefs;
  if (!prefs.begin(nameSpace.c_str(), true)) {
    outErrorMessage = "Failed to open NVS namespace for read";
    return false;
  }

  const size_t storedSize = prefs.getBytesLength(key.c_str());
  if (storedSize == 0) {
    prefs.end();
    outErrorMessage = "No skill payload found in NVS";
    return false;
  }

  if (storedSize > maxBytes) {
    prefs.end();
    outErrorMessage = "Stored NVS payload exceeds configured maxBytes";
    return false;
  }

  char* payloadBuffer = static_cast<char*>(allocateBuffer(storedSize + 1, preferPsrAm_));
  if (payloadBuffer == nullptr) {
    prefs.end();
    outErrorMessage = "Failed to allocate buffer for NVS payload";
    return false;
  }

  const size_t readSize = prefs.getBytes(key.c_str(), payloadBuffer, storedSize);
  prefs.end();
  if (readSize != storedSize) {
    freeBuffer(payloadBuffer);
    outErrorMessage = "Failed to read full skill payload from NVS";
    return false;
  }

  payloadBuffer[storedSize] = '\0';
  String payload(payloadBuffer);
  freeBuffer(payloadBuffer);

  if (payload.length() == 0) {
    outErrorMessage = "Stored NVS skill payload is empty";
    return false;
  }

  return decodeJson(payload, outErrorMessage);
}

size_t Skill::Size() const {
  size_t count = 0;
  for (const Entry& entry : entries_) {
    if (entry.used) {
      ++count;
    }
  }
  return count;
}

Skill::Entry* Skill::findMutableById(const String& skillId) {
  for (Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }
    if (entry.item.id.equalsIgnoreCase(skillId)) {
      return &entry;
    }
  }
  return nullptr;
}

const Skill::Entry* Skill::findById(const String& skillId) const {
  for (const Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }
    if (entry.item.id.equalsIgnoreCase(skillId)) {
      return &entry;
    }
  }
  return nullptr;
}

bool Skill::applyPsramPreference(String& outErrorMessage) {
  outErrorMessage = String();

  if (!preferPsrAm_) {
    return true;
  }

#if !defined(ESP32)
  outErrorMessage = "PSRAM preference is only supported on ESP32 targets";
  return false;
#else
  if (psramPreferenceApplied_) {
    return true;
  }

  if (!psramFound()) {
    outErrorMessage = "PSRAM not found";
    return false;
  }

  heap_caps_malloc_extmem_enable(extmemAlwaysInternalThreshold_);
  psramPreferenceApplied_ = true;
  return true;
#endif
}

bool Skill::copyItem(AiSkillItem& outTarget,
                     const AiSkillItem& source,
                     String& outErrorMessage) {
  outErrorMessage = String();

  if (preferPsrAm_) {
    if (!outTarget.id.reserve(source.id.length() + 1)) {
      outErrorMessage = "Failed reserving memory for skill id";
      return false;
    }
    if (!outTarget.name.reserve(source.name.length() + 1)) {
      outErrorMessage = "Failed reserving memory for skill name";
      return false;
    }
    if (!outTarget.description.reserve(source.description.length() + 1)) {
      outErrorMessage = "Failed reserving memory for skill description";
      return false;
    }
    if (!outTarget.instructions.reserve(source.instructions.length() + 1)) {
      outErrorMessage = "Failed reserving memory for skill instructions";
      return false;
    }
  }

  outTarget = source;
  return true;
}

bool Skill::encodeJson(String& outJson, String& outErrorMessage) const {
  outErrorMessage = String();
  outJson = String();

  ArduinoJson::JsonDocument doc;
  ArduinoJson::JsonArray outSkills = doc[kSkillsRootKey].to<ArduinoJson::JsonArray>();

  for (const Entry& entry : entries_) {
    if (!entry.used) {
      continue;
    }

    ArduinoJson::JsonObject outItem = outSkills.add<ArduinoJson::JsonObject>();
    outItem[kSkillIdKey] = entry.item.id;
    outItem[kSkillNameKey] = entry.item.name;
    outItem[kSkillDescriptionKey] = entry.item.description;
    outItem[kSkillInstructionsKey] = entry.item.instructions;
  }

  const size_t bytes = serializeJson(doc, outJson);
  if (bytes == 0) {
    outErrorMessage = "Failed to serialize skill payload";
    return false;
  }

  return true;
}

bool Skill::decodeJson(const String& json, String& outErrorMessage) {
  outErrorMessage = String();

  if (!applyPsramPreference(outErrorMessage)) {
    return false;
  }

  ArduinoJson::JsonDocument doc;
  const auto parseError = deserializeJson(doc, json);
  if (parseError) {
    outErrorMessage = String("Failed to parse skill payload: ") + parseError.c_str();
    return false;
  }

  ArduinoJson::JsonArrayConst inSkills = doc[kSkillsRootKey].as<ArduinoJson::JsonArrayConst>();
  if (inSkills.isNull()) {
    outErrorMessage = "Invalid skill payload: missing skills array";
    return false;
  }

  std::array<Entry, kAiMaxSkills> nextEntries{};
  size_t loadedCount = 0;

  for (ArduinoJson::JsonVariantConst value : inSkills) {
    if (loadedCount >= nextEntries.size()) {
      outErrorMessage = "Skill payload exceeds in-memory capacity";
      return false;
    }

    ArduinoJson::JsonObjectConst inItem = value.as<ArduinoJson::JsonObjectConst>();
    if (inItem.isNull()) {
      outErrorMessage = "Invalid skill payload: one item is not an object";
      return false;
    }

    AiSkillItem item;
    item.id = String(inItem[kSkillIdKey] | "");
    item.name = String(inItem[kSkillNameKey] | "");
    item.description = String(inItem[kSkillDescriptionKey] | "");
    item.instructions = String(inItem[kSkillInstructionsKey] | "");

    if (item.id.length() == 0 || item.name.length() == 0) {
      outErrorMessage = "Invalid skill payload: id/name cannot be empty";
      return false;
    }

    Entry entry;
    entry.used = true;
    if (!copyItem(entry.item, item, outErrorMessage)) {
      return false;
    }

    nextEntries[loadedCount++] = entry;
  }

  entries_ = nextEntries;
  return true;
}

}  // namespace ai::provider
