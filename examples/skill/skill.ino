#include <Arduino.h>

#include <core/ai_skill.h>

using namespace ai::provider;

Skill g_skillStore;

void printSkills() {
  AiSkillList list;
  if (!g_skillStore.List(list)) {
    Serial.println("List failed");
    return;
  }

  Serial.printf("Total skills: %u\n", static_cast<unsigned>(list.count));
  for (size_t i = 0; i < list.count; ++i) {
    const AiSkillItem& item = list.items[i];
    Serial.printf("- [%s] %s\n", item.id.c_str(), item.name.c_str());
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  String error;

  AiSkillItem weather;
  weather.id = "weather";
  weather.name = "Weather Analyst";
  weather.description = "Use weather APIs and summarize output";
  weather.instructions = "Always include city and temperature unit";

  if (!g_skillStore.Add(weather, error)) {
    Serial.printf("Add failed: %s\n", error.c_str());
    return;
  }

  printSkills();

  AiSkillItem fetched;
  if (g_skillStore.Get("weather", fetched)) {
    Serial.printf("Get success: %s => %s\n", fetched.id.c_str(), fetched.description.c_str());
  }

  AiSkillItem updated = fetched;
  updated.description = "Use weather APIs, validate units, and summarize output";
  if (!g_skillStore.Update("weather", updated, error)) {
    Serial.printf("Update failed: %s\n", error.c_str());
    return;
  }

  if (!g_skillStore.Remove("weather")) {
    Serial.println("Remove failed");
    return;
  }

  printSkills();
}

void loop() {
  delay(1000);
}
