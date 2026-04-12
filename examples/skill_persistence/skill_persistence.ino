#include <Arduino.h>
#include <LittleFS.h>

#include <core/ai_skill.h>

using namespace ai::provider;

Skill g_skills;

void printList(const char* title) {
  AiSkillList list;
  if (!g_skills.List(list)) {
    Serial.println("List failed");
    return;
  }

  Serial.printf("%s (count=%u)\n", title, static_cast<unsigned>(list.count));
  for (size_t i = 0; i < list.count; ++i) {
    const AiSkillItem& item = list.items[i];
    Serial.printf("- [%s] %s\n", item.id.c_str(), item.name.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  String error;

  AiSkillMemoryConfig memConfig;
  memConfig.preferPsrAm = true;
  memConfig.extmemAlwaysInternalThreshold = 64;
  if (!g_skills.ConfigureMemory(memConfig, error)) {
    Serial.printf("ConfigureMemory failed: %s\n", error.c_str());
    return;
  }

  if (!LittleFS.begin(false)) {
    Serial.println("LittleFS mount failed");
    return;
  }

  AiSkillItem weather;
  weather.id = "weather";
  weather.name = "Weather Analyst";
  weather.description = "Use weather APIs and summarize output";
  weather.instructions = "Always include city and unit";

  if (!g_skills.Add(weather, error)) {
    Serial.printf("Add failed: %s\n", error.c_str());
    return;
  }

  printList("Before save");

  if (!g_skills.SaveToFs(LittleFS, "/skills.json", error)) {
    Serial.printf("SaveToFs failed: %s\n", error.c_str());
    return;
  }

  if (!g_skills.SaveToNvs("aikit_sk", "payload", 2048, error)) {
    Serial.printf("SaveToNvs failed: %s\n", error.c_str());
    return;
  }

  if (!g_skills.Remove("weather")) {
    Serial.println("Remove failed");
    return;
  }

  printList("After remove");

  if (!g_skills.LoadFromFs(LittleFS, "/skills.json", error)) {
    Serial.printf("LoadFromFs failed: %s\n", error.c_str());
    return;
  }

  printList("After LoadFromFs");

  if (!g_skills.LoadFromNvs("aikit_sk", "payload", 2048, error)) {
    Serial.printf("LoadFromNvs failed: %s\n", error.c_str());
    return;
  }

  printList("After LoadFromNvs");
}

void loop() {
  delay(1000);
}
