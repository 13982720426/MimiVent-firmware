// 本模块集中管理正式/DEV 时序和 NVS 持久化，避免网页、状态机各自维护默认值。
#include "SettingsStore.h"

#include <cstdio>
#include <Preferences.h>

namespace {

constexpr char kPreferencesNamespace[] = "MimiVent";
constexpr uint32_t kSettingsSchemaVersion = 2;
constexpr unsigned long kAllowedTimesMs[] = {
    0UL,       5000UL,     10000UL,     20000UL,     30000UL,
    60000UL,   120000UL,   180000UL,    300000UL,    600000UL,
};

constexpr TimingSettings kProductionDefaults = {
    30000UL,   // 启动冷静期：30 秒
    30000UL,   // 离开确认：30 秒
    120000UL,  // 自动排风：2 分钟
    60000UL,   // 临时排风：1 分钟
    300000UL,  // 临时暂停：5 分钟
};

constexpr TimingSettings kDevDefaults = {
    5000UL,    // 启动冷静期：5 秒
    0UL,       // 联调时 PIR 一变 LOW 就允许开始排风
    10000UL,   // 自动排风：10 秒
    10000UL,   // 临时排风：10 秒
    10000UL,   // 临时暂停：10 秒
};

TimingSettings productionSettings = kProductionDefaults;
TimingSettings devSettings = kDevDefaults;

TimingSettings& mutableSettings(TimingProfile profile) {
  return profile == TimingProfile::DEV ? devSettings : productionSettings;
}

const TimingSettings& defaultsFor(TimingProfile profile) {
  return profile == TimingProfile::DEV ? kDevDefaults : kProductionDefaults;
}

void loadOneProfile(Preferences& preferences, TimingProfile profile, const char* prefix) {
  TimingSettings& current = mutableSettings(profile);
  const TimingSettings& defaults = defaultsFor(profile);

  char key[16];
  snprintf(key, sizeof(key), "%s_start", prefix);
  current.startupCooldownMs = preferences.getULong(key, defaults.startupCooldownMs);
  snprintf(key, sizeof(key), "%s_leave", prefix);
  current.leaveConfirmMs = preferences.getULong(key, defaults.leaveConfirmMs);
  snprintf(key, sizeof(key), "%s_auto", prefix);
  current.autoExhaustMs = preferences.getULong(key, defaults.autoExhaustMs);
  snprintf(key, sizeof(key), "%s_manual", prefix);
  current.manualExhaustMs = preferences.getULong(key, defaults.manualExhaustMs);
  snprintf(key, sizeof(key), "%s_pause", prefix);
  current.pauseMs = preferences.getULong(key, defaults.pauseMs);

  // 即使 NVS 中有旧版/损坏数据，也只接受公共选项范围内的值。
  if (!settingsStoreIsAllowedTime(current.startupCooldownMs, false)) {
    current.startupCooldownMs = defaults.startupCooldownMs;
  }
  if (!settingsStoreIsAllowedTime(current.leaveConfirmMs, true)) {
    current.leaveConfirmMs = defaults.leaveConfirmMs;
  }
  if (!settingsStoreIsAllowedTime(current.autoExhaustMs, false)) {
    current.autoExhaustMs = defaults.autoExhaustMs;
  }
  if (!settingsStoreIsAllowedTime(current.manualExhaustMs, false)) {
    current.manualExhaustMs = defaults.manualExhaustMs;
  }
  if (!settingsStoreIsAllowedTime(current.pauseMs, false)) {
    current.pauseMs = defaults.pauseMs;
  }
}

bool putIfChanged(Preferences& preferences, const char* key, unsigned long value) {
  if (preferences.getULong(key, 0xFFFFFFFFUL) == value) return true;
  return preferences.putULong(key, value) == sizeof(uint32_t);
}

// 迁移旧版内置冷静期默认值，保留其他自定义时长。
void migrateStartupCooldownDefaults(Preferences& preferences) {
  const uint32_t schemaVersion = preferences.getUInt("schema", 0);
  if (schemaVersion >= kSettingsSchemaVersion) return;

  bool migrated = true;
  if (preferences.getULong("prod_start", 0xFFFFFFFFUL) == 60000UL) {
    migrated = putIfChanged(preferences, "prod_start",
                            kProductionDefaults.startupCooldownMs) &&
               migrated;
  }
  if (preferences.getULong("dev_start", 0xFFFFFFFFUL) == 10000UL) {
    migrated = putIfChanged(preferences, "dev_start",
                            kDevDefaults.startupCooldownMs) &&
               migrated;
  }
  if (migrated && preferences.putUInt("schema", kSettingsSchemaVersion) == sizeof(uint32_t)) {
    Serial.println("Settings: migrated built-in startup cooldown defaults");
  } else {
    Serial.println("Settings: startup cooldown migration will retry next boot");
  }
}

}  // namespace

void settingsStoreBegin() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("Settings: NVS unavailable; using built-in defaults");
    return;
  }
  migrateStartupCooldownDefaults(preferences);
  loadOneProfile(preferences, TimingProfile::PRODUCTION, "prod");
  loadOneProfile(preferences, TimingProfile::DEV, "dev");
  preferences.end();
}

TimingSettings settingsStoreGet(TimingProfile profile) { return mutableSettings(profile); }

bool settingsStoreSave(TimingProfile profile, const TimingSettings& settings) {
  if (!settingsStoreIsAllowedTime(settings.startupCooldownMs, false) ||
      !settingsStoreIsAllowedTime(settings.leaveConfirmMs, true) ||
      !settingsStoreIsAllowedTime(settings.autoExhaustMs, false) ||
      !settingsStoreIsAllowedTime(settings.manualExhaustMs, false) ||
      !settingsStoreIsAllowedTime(settings.pauseMs, false)) {
    return false;
  }

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;

  const char* prefix = profile == TimingProfile::DEV ? "dev" : "prod";
  char key[16];
  bool saved = true;
  snprintf(key, sizeof(key), "%s_start", prefix);
  saved = putIfChanged(preferences, key, settings.startupCooldownMs) && saved;
  snprintf(key, sizeof(key), "%s_leave", prefix);
  saved = putIfChanged(preferences, key, settings.leaveConfirmMs) && saved;
  snprintf(key, sizeof(key), "%s_auto", prefix);
  saved = putIfChanged(preferences, key, settings.autoExhaustMs) && saved;
  snprintf(key, sizeof(key), "%s_manual", prefix);
  saved = putIfChanged(preferences, key, settings.manualExhaustMs) && saved;
  snprintf(key, sizeof(key), "%s_pause", prefix);
  saved = putIfChanged(preferences, key, settings.pauseMs) && saved;
  preferences.end();

  if (saved) mutableSettings(profile) = settings;
  return saved;
}

bool settingsStoreReset(TimingProfile profile) {
  return settingsStoreSave(profile, defaultsFor(profile));
}

bool settingsStoreIsAllowedTime(unsigned long valueMs, bool allowZero) {
  for (unsigned long option : kAllowedTimesMs) {
    if (option == valueMs) return allowZero || valueMs != 0;
  }
  return false;
}

const char* settingsStoreProfileName(TimingProfile profile) {
  return profile == TimingProfile::DEV ? "dev" : "production";
}

bool settingsStoreParseProfile(const String& value, TimingProfile* profile) {
  if (profile == nullptr) return false;
  if (value == "dev") {
    *profile = TimingProfile::DEV;
    return true;
  }
  if (value == "production") {
    *profile = TimingProfile::PRODUCTION;
    return true;
  }
  return false;
}
