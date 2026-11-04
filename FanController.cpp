// PIR 优先停风的自动状态机，以及限时手动控制。
#include "FanController.h"

#include "AppConfig.h"
#include "StatisticsStore.h"

namespace {

ControlMode controlMode = ControlMode::AUTO;
AutoPhase autoPhase = AutoPhase::ARMED;
TimingProfile activeProfile = TimingProfile::PRODUCTION;
bool fanOn = false;
bool presenceDetected = false;
bool previousPresenceDetected = false;
unsigned long lastPirPollMs = 0;
unsigned long phaseStartedMs = 0;
unsigned long phaseDurationMs = 0;
unsigned long fanStartedMs = 0;
unsigned long overrideStartedMs = 0;
unsigned long overrideDurationMs = 0;

uint8_t relayOnLevel() { return AppConfig::kRelayActiveLow ? LOW : HIGH; }

uint8_t relayOffLevel() { return AppConfig::kRelayActiveLow ? HIGH : LOW; }

void setFan(bool turnOn, unsigned long nowMs) {
  if (fanOn == turnOn) return;
  if (turnOn) {
    uint8_t kind = 3;  // 临时排风
    if (controlMode == ControlMode::AUTO && autoPhase == AutoPhase::EXHAUSTING) kind = 2;
    else if (controlMode == ControlMode::FORCE_TEST) kind = 4;
    statisticsStoreFanStarted(kind, nowMs,
        kind == 2 ? static_cast<uint16_t>(phaseDurationMs / 1000UL) : 0);
  } else {
    statisticsStoreFanStopped(nowMs - fanStartedMs);
  }
  fanOn = turnOn;
  fanStartedMs = fanOn ? nowMs : 0;
  digitalWrite(AppConfig::kRelayPin, fanOn ? relayOnLevel() : relayOffLevel());
  Serial.printf("[%lu ms] Fan: %s\n", nowMs, fanOn ? "ON" : "OFF");
}

void enterOccupied(unsigned long nowMs, const char* reason) {
  autoPhase = AutoPhase::OCCUPIED;
  phaseStartedMs = nowMs;
  phaseDurationMs = 0;
  setFan(false, nowMs);
  Serial.printf("[%lu ms] Activity: %s; fan stopped, waiting for PIR LOW\n", nowMs,
                reason);
}

void enterClearConfirm(unsigned long nowMs, unsigned long leaveConfirmMs) {
  autoPhase = AutoPhase::CLEAR_CONFIRM;
  phaseStartedMs = nowMs;
  phaseDurationMs = leaveConfirmMs;
  setFan(false, nowMs);
  Serial.printf("[%lu ms] PIR clear; leave confirmation started for %lu ms\n", nowMs,
                leaveConfirmMs);
}

void returnToAutoAfterExhaust(unsigned long nowMs) {
  overrideDurationMs = 0;
  controlMode = ControlMode::AUTO;
  if (presenceDetected) {
    autoPhase = AutoPhase::OCCUPIED;
    phaseStartedMs = nowMs;
  } else {
    // 临时排风已完成本次换气，不再紧接着启动自动排风。
    autoPhase = AutoPhase::ARMED;
    phaseStartedMs = nowMs;
  }
  Serial.println("Timed exhaust complete; returned to automatic sensing");
}

void returnToAutoAfterPause(unsigned long nowMs) {
  overrideDurationMs = 0;
  controlMode = ControlMode::AUTO;
  if (presenceDetected) {
    autoPhase = AutoPhase::OCCUPIED;
    phaseStartedMs = nowMs;
  } else {
    const TimingSettings settings = settingsStoreGet(activeProfile);
    enterClearConfirm(nowMs, settings.leaveConfirmMs);
  }
  Serial.println("Timed pause complete; automatic sensing resumed");
}

// 强制操作优先于启动保护；结束冷静期并建立合理的自动感应起点。
void skipStartupCooldown(unsigned long nowMs) {
  if (autoPhase != AutoPhase::STARTUP_COOLDOWN) return;
  autoPhase = presenceDetected ? AutoPhase::OCCUPIED : AutoPhase::ARMED;
  phaseStartedMs = nowMs;
  phaseDurationMs = 0;
}

void updatePresence(unsigned long nowMs) {
  if (nowMs - lastPirPollMs < AppConfig::kPirPollIntervalMs) return;
  lastPirPollMs = nowMs;
  presenceDetected = digitalRead(AppConfig::kPirPin) == HIGH;
  if (presenceDetected == previousPresenceDetected) return;

  previousPresenceDetected = presenceDetected;
  Serial.printf("[%lu ms] PIR: %s\n", nowMs, presenceDetected ? "HIGH" : "LOW");
  if (presenceDetected) {
    const TimingSettings settings = settingsStoreGet(activeProfile);
    const uint32_t gapSeconds = settings.autoExhaustMs / 1000UL;
    statisticsStoreRecordActivity(static_cast<uint16_t>(gapSeconds));
  }

  // 启动冷静期只记录 PIR 当前电平，不推进自动排风状态。
  if (controlMode == ControlMode::AUTO && autoPhase == AutoPhase::STARTUP_COOLDOWN) return;

  if (presenceDetected) {
    if (controlMode == ControlMode::TIMED_ON) {
      // 普通临时排风遵守 PIR 优先；取消本轮手动排风并转入自动等待。
      controlMode = ControlMode::AUTO;
      overrideDurationMs = 0;
      enterOccupied(nowMs, "manual exhaust interrupted by PIR");
    } else if (controlMode == ControlMode::AUTO) {
      enterOccupied(nowMs, "PIR HIGH");
    } else if (controlMode == ControlMode::TIMED_OFF) {
      // 暂停期间记录活动；到期后仍需等 PIR 变 LOW 并通过确认时间。
      autoPhase = AutoPhase::OCCUPIED;
      phaseStartedMs = nowMs;
    }
    // FORCE_TEST 明确用于继电器/风机检查，期间忽略 PIR。
  } else if (controlMode == ControlMode::AUTO && autoPhase == AutoPhase::OCCUPIED) {
    const TimingSettings settings = settingsStoreGet(activeProfile);
    enterClearConfirm(nowMs, settings.leaveConfirmMs);
  }
}

void updateAutomatic(unsigned long nowMs) {
  const TimingSettings settings = settingsStoreGet(activeProfile);

  switch (autoPhase) {
    case AutoPhase::ARMED:
      setFan(false, nowMs);
      break;

    case AutoPhase::OCCUPIED:
      setFan(false, nowMs);
      if (!presenceDetected) enterClearConfirm(nowMs, settings.leaveConfirmMs);
      break;

    case AutoPhase::CLEAR_CONFIRM:
      setFan(false, nowMs);
      if (presenceDetected) {
        enterOccupied(nowMs, "PIR HIGH during leave confirmation");
      } else if (nowMs - phaseStartedMs >= phaseDurationMs) {
        autoPhase = AutoPhase::EXHAUSTING;
        phaseStartedMs = nowMs;
        phaseDurationMs = settings.autoExhaustMs;
        setFan(true, nowMs);
        Serial.printf("[%lu ms] Automatic exhaust started for %lu ms\n", nowMs,
                      phaseDurationMs);
      }
      break;

    case AutoPhase::EXHAUSTING:
      if (presenceDetected) {
        enterOccupied(nowMs, "PIR HIGH during automatic exhaust");
      } else if (nowMs - phaseStartedMs >= phaseDurationMs) {
        setFan(false, nowMs);
        autoPhase = AutoPhase::ARMED;
        phaseStartedMs = nowMs;
        phaseDurationMs = 0;
        Serial.println("Automatic exhaust complete; waiting for next activity");
      }
      break;
  }
}

}  // namespace

void fanControllerBegin() {
  // 先设继电器关闭电平，再切换为输出，避免上电瞬间误吸合。
  digitalWrite(AppConfig::kRelayPin, relayOffLevel());
  pinMode(AppConfig::kRelayPin, OUTPUT);
  digitalWrite(AppConfig::kRelayPin, relayOffLevel());
  pinMode(AppConfig::kPirPin, INPUT);

  controlMode = ControlMode::AUTO;
  // 每次重启默认使用正式配置；DEV 仅由网页在运行期间显式切换。
  activeProfile = TimingProfile::PRODUCTION;
  fanOn = false;
  lastPirPollMs = millis() - AppConfig::kPirPollIntervalMs;
  presenceDetected = digitalRead(AppConfig::kPirPin) == HIGH;
  previousPresenceDetected = presenceDetected;
  phaseStartedMs = millis();
  phaseDurationMs = settingsStoreGet(activeProfile).startupCooldownMs;
  overrideStartedMs = 0;
  overrideDurationMs = 0;
  autoPhase = AutoPhase::STARTUP_COOLDOWN;

  Serial.printf("Fan controller ready; active profile: %s; startup cooldown: %lu ms\n",
                settingsStoreProfileName(activeProfile), phaseDurationMs);
}

void fanControllerUpdate(unsigned long nowMs) {
  updatePresence(nowMs);

  if (autoPhase == AutoPhase::STARTUP_COOLDOWN) {
    setFan(false, nowMs);
    if (nowMs - phaseStartedMs >= phaseDurationMs) {
      autoPhase = presenceDetected ? AutoPhase::OCCUPIED : AutoPhase::ARMED;
      phaseStartedMs = nowMs;
      phaseDurationMs = 0;
      Serial.println("Startup cooldown complete; automatic sensing enabled");
    }
    return;
  }

  if (controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::TIMED_OFF ||
      controlMode == ControlMode::FORCE_TEST) {
    if (nowMs - overrideStartedMs >= overrideDurationMs) {
      if (controlMode == ControlMode::TIMED_OFF) {
        returnToAutoAfterPause(nowMs);
      } else {
        returnToAutoAfterExhaust(nowMs);
      }
    } else {
      setFan(controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::FORCE_TEST,
             nowMs);
      return;
    }
  }

  if (controlMode == ControlMode::AUTO) updateAutomatic(nowMs);
  else setFan(false, nowMs);
}

FanControllerStatus fanControllerGetStatus(unsigned long nowMs) {
  FanControllerStatus status{};
  status.fanOn = fanOn;
  status.presenceDetected = presenceDetected;
  status.mode = controlMode;
  status.autoPhase = autoPhase;
  status.activeProfile = activeProfile;
  status.fanRunMs = fanOn ? nowMs - fanStartedMs : 0;
  status.overrideDurationMs = overrideDurationMs;
  status.phaseDurationMs = phaseDurationMs;

  if (autoPhase == AutoPhase::STARTUP_COOLDOWN) {
    const unsigned long elapsed = nowMs - phaseStartedMs;
    status.remainingMs = elapsed >= phaseDurationMs ? 0 : phaseDurationMs - elapsed;
  } else if (controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::TIMED_OFF ||
      controlMode == ControlMode::FORCE_TEST) {
    const unsigned long elapsed = nowMs - overrideStartedMs;
    status.remainingMs = elapsed >= overrideDurationMs ? 0 : overrideDurationMs - elapsed;
  } else if (controlMode == ControlMode::AUTO &&
             (autoPhase == AutoPhase::CLEAR_CONFIRM || autoPhase == AutoPhase::EXHAUSTING)) {
    const unsigned long elapsed = nowMs - phaseStartedMs;
    status.remainingMs = elapsed >= phaseDurationMs ? 0 : phaseDurationMs - elapsed;
  }

  return status;
}

void fanControllerSetProfile(TimingProfile profile) {
  if (activeProfile == profile) return;
  activeProfile = profile;
  if (autoPhase == AutoPhase::STARTUP_COOLDOWN) {
    phaseStartedMs = millis();
    phaseDurationMs = settingsStoreGet(activeProfile).startupCooldownMs;
  } else if (controlMode == ControlMode::AUTO) {
    autoPhase = presenceDetected ? AutoPhase::OCCUPIED : AutoPhase::ARMED;
    phaseStartedMs = millis();
    phaseDurationMs = 0;
    setFan(false, phaseStartedMs);
  }
  Serial.printf("Active timing profile: %s\n", settingsStoreProfileName(activeProfile));
}

bool fanControllerStartTimedExhaust(unsigned long durationMs, unsigned long nowMs) {
  // 新操作前即时采样一次，避免使用上次轮询的旧 PIR 状态。
  presenceDetected = digitalRead(AppConfig::kPirPin) == HIGH;
  previousPresenceDetected = presenceDetected;
  if (presenceDetected) {
    controlMode = ControlMode::AUTO;
    overrideDurationMs = 0;
    enterOccupied(nowMs, "manual exhaust refused while PIR HIGH");
    return false;
  }
  controlMode = ControlMode::TIMED_ON;
  overrideStartedMs = nowMs;
  overrideDurationMs = durationMs;
  setFan(true, nowMs);
  Serial.printf("Timed exhaust started for %lu ms\n", durationMs);
  return true;
}

void fanControllerStartTimedPause(unsigned long durationMs, unsigned long nowMs) {
  controlMode = ControlMode::TIMED_OFF;
  overrideStartedMs = nowMs;
  overrideDurationMs = durationMs;
  setFan(false, nowMs);
  statisticsStoreRecordTimedPause(durationMs);
  Serial.printf("Timed pause started for %lu ms\n", durationMs);
}

void fanControllerStartForceTest(unsigned long durationMs, unsigned long nowMs) {
  skipStartupCooldown(nowMs);
  controlMode = ControlMode::FORCE_TEST;
  overrideStartedMs = nowMs;
  overrideDurationMs = durationMs;
  setFan(true, nowMs);
  Serial.printf("Force fan test started for %lu ms (PIR ignored)\n", durationMs);
}

void fanControllerSetAuto(unsigned long nowMs, FanControlSource source) {
  if (source == FanControlSource::USER &&
      (controlMode == ControlMode::LOCKED_OFF || controlMode == ControlMode::TIMED_OFF)) {
    statisticsStoreRecordAutoResume();
  }
  controlMode = ControlMode::AUTO;
  overrideDurationMs = 0;
  setFan(false, nowMs);
  if (autoPhase == AutoPhase::STARTUP_COOLDOWN) {
    Serial.println("Control mode: automatic sensing (startup cooldown continues)");
    return;
  }
  autoPhase = presenceDetected ? AutoPhase::OCCUPIED : AutoPhase::ARMED;
  phaseStartedMs = nowMs;
  phaseDurationMs = 0;
  Serial.println("Control mode: automatic sensing");
}

void fanControllerSetLockedOff(unsigned long nowMs, FanControlSource source) {
  skipStartupCooldown(nowMs);
  controlMode = ControlMode::LOCKED_OFF;
  overrideDurationMs = 0;
  setFan(false, nowMs);
  if (source == FanControlSource::USER) statisticsStoreRecordLockedOff();
  Serial.println("Control mode: locked off");
}

bool fanControllerHasInterruptibleTask() {
  if (controlMode == ControlMode::TIMED_ON || controlMode == ControlMode::TIMED_OFF ||
      controlMode == ControlMode::FORCE_TEST) {
    return true;
  }
  return controlMode == ControlMode::AUTO &&
         (autoPhase == AutoPhase::CLEAR_CONFIRM || autoPhase == AutoPhase::EXHAUSTING);
}

bool fanControllerIsStartupCoolingDown() {
  return autoPhase == AutoPhase::STARTUP_COOLDOWN;
}

bool fanControllerIsFanOn() { return fanOn; }

const char* fanControllerModeName() {
  switch (controlMode) {
    case ControlMode::TIMED_ON:
      return "timed_on";
    case ControlMode::TIMED_OFF:
      return "timed_off";
    case ControlMode::FORCE_TEST:
      return "force_test";
    case ControlMode::LOCKED_OFF:
      return "lock_off";
    default:
      return "auto";
  }
}

const char* fanControllerPhaseName() {
  switch (autoPhase) {
    case AutoPhase::STARTUP_COOLDOWN:
      return "startup_cooldown";
    case AutoPhase::OCCUPIED:
      return "occupied";
    case AutoPhase::CLEAR_CONFIRM:
      return "clear_confirm";
    case AutoPhase::EXHAUSTING:
      return "exhausting";
    default:
      return "armed";
  }
}
