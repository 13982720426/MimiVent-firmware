#pragma once

#include <Arduino.h>

// 正式运行和开发联调各自保存一份时序，网页路由只决定正在查看哪份配置。
enum class TimingProfile : uint8_t {
  PRODUCTION,
  DEV,
};

// 所有时长统一使用毫秒，网页和控制器共用同一份结构。
struct TimingSettings {
  unsigned long startupCooldownMs;
  unsigned long leaveConfirmMs;
  unsigned long autoExhaustMs;
  unsigned long manualExhaustMs;
  unsigned long pauseMs;
};

// 从 NVS 载入正式/DEV 时序；缺少或无效的值使用内置默认值。
void settingsStoreBegin();

// 读取指定配置。返回值是副本，修改后需调用 settingsStoreSave 显式保存。
TimingSettings settingsStoreGet(TimingProfile profile);

// 校验所有值均来自公共时间选项后，保存指定配置。
bool settingsStoreSave(TimingProfile profile, const TimingSettings& settings);

// 将指定环境恢复为内置默认值，并同步写入 NVS。
bool settingsStoreReset(TimingProfile profile);

// 公共时间选项校验；离开确认可允许 0，其余时长必须大于 0。
bool settingsStoreIsAllowedTime(unsigned long valueMs, bool allowZero);

// 网页/API 使用的配置名称转换。
const char* settingsStoreProfileName(TimingProfile profile);
bool settingsStoreParseProfile(const String& value, TimingProfile* profile);
