#pragma once

#include <array>

#include <Arduino.h>
#include <FS.h>

namespace ai::provider {

constexpr size_t kAiMaxSkills = 16;

struct AiSkillItem {
  String id;
  String name;
  String description;
  String instructions;
};

struct AiSkillList {
  std::array<AiSkillItem, kAiMaxSkills> items;
  size_t count = 0;
};

struct AiSkillMemoryConfig {
  bool preferPsrAm = false;
  uint32_t extmemAlwaysInternalThreshold = 64;
};

class Skill {
 public:
  bool ConfigureMemory(const AiSkillMemoryConfig& config, String& outErrorMessage);

  bool Add(const AiSkillItem& item, String& outErrorMessage);
  bool List(AiSkillList& outList) const;
  bool Get(const String& skillId, AiSkillItem& outItem) const;
  bool Remove(const String& skillId);
  bool Update(const String& skillId,
              const AiSkillItem& updatedItem,
              String& outErrorMessage);

  bool SaveToFs(fs::FS& filesystem, const String& path, String& outErrorMessage) const;
  bool LoadFromFs(fs::FS& filesystem, const String& path, String& outErrorMessage);

  bool SaveToNvs(const String& nameSpace,
                 const String& key,
                 size_t maxBytes,
                 String& outErrorMessage) const;
  bool LoadFromNvs(const String& nameSpace,
                   const String& key,
                   size_t maxBytes,
                   String& outErrorMessage);

  size_t Size() const;

 private:
  struct Entry {
    bool used = false;
    AiSkillItem item;
  };

  std::array<Entry, kAiMaxSkills> entries_{};
  bool preferPsrAm_ = false;
  uint32_t extmemAlwaysInternalThreshold_ = 64;
  bool psramPreferenceApplied_ = false;

  Entry* findMutableById(const String& skillId);
  const Entry* findById(const String& skillId) const;

  bool applyPsramPreference(String& outErrorMessage);
  bool copyItem(AiSkillItem& outTarget,
                const AiSkillItem& source,
                String& outErrorMessage);
  bool encodeJson(String& outJson, String& outErrorMessage) const;
  bool decodeJson(const String& json, String& outErrorMessage);
};

}  // namespace ai::provider
