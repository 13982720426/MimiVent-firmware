#pragma once

#include <Arduino.h>

#include "SettingsStore.h"

// 用户可见的控制方式。自动模式内部阶段由 AutoPhase 表示。
enum class ControlMode : uint8_t {
  AUTO,
  TIMED_ON,
  TIMED_OFF,
  FORCE_TEST,
  LOCKED_OFF,
};

enum class AutoPhase : uint8_t {
  STARTUP_COOLDOWN,
  ARMED,
  OCCUPIED,
  CLEAR_CONFIRM,
  EXHAUSTING,
};

struct FanControllerStatus {
  bool fanOn;
  bool presenceDetected;
  ControlMode mode;
  AutoPhase autoPhase;
  TimingProfile activeProfile;
  unsigned long fanRunMs;
  unsigned long remainingMs;
  unsigned long overrideDurationMs;
  unsigned long phaseDurationMs;
};

// 初始化 PIR 与继电器；启动后默认使用正式配置并保持风机关闭。
void fanControllerBegin();

// 非阻塞轮询 PIR、执行自动状态机和限时操作。
void fanControllerUpdate(unsigned long nowMs);

// 返回当前控制状态及页面所需的倒计时。
FanControllerStatus fanControllerGetStatus(unsigned long nowMs);

// 明确应用一套配置。网页仅查看配置时不调用此方法。
void fanControllerSetProfile(TimingProfile profile);

// 启动限时临时排风；PIR 已为 HIGH 时拒绝启动。
bool fanControllerStartTimedExhaust(unsigned long durationMs, unsigned long nowMs);

// 启动限时暂停；到时后重新开始自动流程。
void fanControllerStartTimedPause(unsigned long durationMs, unsigned long nowMs);

// 启动忽略 PIR 的限时继电器/风机测试。
void fanControllerStartForceTest(unsigned long durationMs, unsigned long nowMs);

// 区分用户控制与 OTA 临时停风，避免内部控制污染用户操作统计。
enum class FanControlSource : uint8_t { USER, OTA };

// 立即取消临时操作、停止风机并恢复自动感应。
void fanControllerSetAuto(unsigned long nowMs,
                          FanControlSource source = FanControlSource::USER);

// 进入需要用户手动解除的持续关闭状态。
void fanControllerSetLockedOff(unsigned long nowMs,
                               FanControlSource source = FanControlSource::USER);

// 当前是否有进行中的倒计时/自动排风，需要先确认再替换。
bool fanControllerHasInterruptibleTask();

// 普通限时排风/暂停遵守冷静期；强制排风测试与持续关闭可跳过冷静期。
bool fanControllerIsStartupCoolingDown();

// 当前风机状态，供状态灯模块读取。
bool fanControllerIsFanOn();

// 页面显示用名称。
const char* fanControllerModeName();
const char* fanControllerPhaseName();
