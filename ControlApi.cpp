// 统一维护 HTTP 与 BLE 共用的业务规则，传输层只负责解析请求和发送响应。
#include "ControlApi.h"

#include <cstdio>
#include <cstdlib>

#include "AppConfig.h"
#include "FanController.h"
#include "FirmwareVersion.h"
#include "StatisticsStore.h"

namespace {

bool lockOffArmed = false;
unsigned long lockOffArmedMs = 0;
TimingProfile lockOffProfile = TimingProfile::PRODUCTION;

ControlApiResponse response(uint16_t code, const char* type, const String& body) {
  return {code, type, body};
}

ControlApiResponse jsonError(uint16_t code, const char* message) {
  String body = "{\"error\":\"";
  body += message;
  body += "\"}";
  return response(code, "application/json; charset=utf-8", body);
}

ControlApiResponse jsonOk(const char* body = "{\"ok\":true}") {
  return response(200, "application/json; charset=utf-8", body);
}

bool profileMatches(TimingProfile requested) {
  return fanControllerGetStatus(millis()).activeProfile == requested;
}

bool replacementAllowed(bool replace) {
  return !fanControllerHasInterruptibleTask() || replace;
}

bool parseUnsignedParameter(const String& value, unsigned long* parsed) {
  if (!value.length()) return false;
  char* end = nullptr;
  const unsigned long result = strtoul(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0') return false;
  *parsed = result;
  return true;
}

String formValue(const String& body, const char* key) {
  const String prefix = String(key) + "=";
  int start = 0;
  while (start < body.length()) {
    const int end = body.indexOf('&', start);
    const String item = end < 0 ? body.substring(start) : body.substring(start, end);
    if (item.startsWith(prefix)) return item.substring(prefix.length());
    if (end < 0) break;
    start = end + 1;
  }
  return String();
}

ControlApiResponse statusResponse(TimingProfile viewProfile) {
  const FanControllerStatus status = fanControllerGetStatus(millis());
  const TimingSettings active = settingsStoreGet(status.activeProfile);
  char payload[680];
  snprintf(payload, sizeof(payload),
           "{\"fan\":%s,\"pir\":%s,\"mode\":\"%s\",\"phase\":\"%s\","
           "\"activeProfile\":\"%s\",\"viewProfile\":\"%s\",\"fanRunMs\":%lu,"
           "\"remainingMs\":%lu,\"durationMs\":%lu,\"phaseDurationMs\":%lu,\"startupCooldownMs\":%lu,\"leaveConfirmMs\":%lu,"
           "\"autoExhaustMs\":%lu,\"manualExhaustMs\":%lu,\"pauseMs\":%lu,\"firmware\":\"%s\",\"minimumAppVersion\":\"%s\"}",
           status.fanOn ? "true" : "false", status.presenceDetected ? "true" : "false",
           fanControllerModeName(), fanControllerPhaseName(),
           settingsStoreProfileName(status.activeProfile), settingsStoreProfileName(viewProfile),
           status.fanRunMs, status.remainingMs, status.overrideDurationMs, status.phaseDurationMs,
           active.startupCooldownMs, active.leaveConfirmMs, active.autoExhaustMs,
           active.manualExhaustMs, active.pauseMs, FIRMWARE_VERSION, FIRMWARE_MINIMUM_APP_VERSION);
  return response(200, "application/json; charset=utf-8", payload);
}

ControlApiResponse settingsResponse(TimingProfile profile) {
  const TimingSettings settings = settingsStoreGet(profile);
  char payload[180];
  snprintf(payload, sizeof(payload),
           "{\"profile\":\"%s\",\"startupCooldownMs\":%lu,\"leaveConfirmMs\":%lu,\"autoExhaustMs\":%lu,"
           "\"manualExhaustMs\":%lu,\"pauseMs\":%lu}",
           settingsStoreProfileName(profile), settings.startupCooldownMs, settings.leaveConfirmMs,
           settings.autoExhaustMs, settings.manualExhaustMs, settings.pauseMs);
  return response(200, "application/json; charset=utf-8", payload);
}

ControlApiResponse timedResponse(const ControlApiRequest& request) {
  if (!profileMatches(request.profile)) return jsonError(409, "PROFILE_MISMATCH");
  if (!replacementAllowed(request.replace)) return jsonError(409, "当前有限时任务，请先确认停止并切换");
  if (fanControllerIsStartupCoolingDown()) return jsonError(423, "STARTUP_COOLDOWN");
  if (request.mode != "on" && request.mode != "off") return jsonError(400, "mode must be on or off");

  fanControllerSetProfile(request.profile);
  const TimingSettings settings = settingsStoreGet(request.profile);
  const unsigned long nowMs = millis();
  if (request.mode == "on") {
    if (!fanControllerStartTimedExhaust(settings.manualExhaustMs, nowMs)) {
      return jsonError(409, "PIR_HIGH");
    }
  } else {
    fanControllerStartTimedPause(settings.pauseMs, nowMs);
  }
  return jsonOk();
}

ControlApiResponse autoResponse(const ControlApiRequest& request) {
  if (!profileMatches(request.profile) && !request.switchProfile) {
    return jsonError(409, "PROFILE_MISMATCH");
  }
  if (!replacementAllowed(request.replace)) return jsonError(409, "当前有限时任务，请先确认停止并切换");
  fanControllerSetProfile(request.profile);
  fanControllerSetAuto(millis());
  return jsonOk();
}

ControlApiResponse lockOffResponse(const ControlApiRequest& request) {
  const unsigned long nowMs = millis();
  if (request.stage == "prepare") {
    if (!profileMatches(request.profile)) return jsonError(409, "PROFILE_MISMATCH");
    lockOffArmed = true;
    lockOffArmedMs = nowMs;
    lockOffProfile = request.profile;
    return jsonOk("{\"armed\":true}");
  }
  if (request.stage == "cancel") {
    lockOffArmed = false;
    return jsonOk();
  }
  if (request.stage != "confirm") return jsonError(400, "invalid stage");
  if (!lockOffArmed || nowMs - lockOffArmedMs > 15000UL) {
    lockOffArmed = false;
    return jsonError(409, "confirmation expired");
  }
  if (!profileMatches(lockOffProfile)) {
    lockOffArmed = false;
    return jsonError(409, "PROFILE_MISMATCH");
  }
  if (!replacementAllowed(request.replace)) return jsonError(409, "当前有限时任务，请先确认停止并切换");
  lockOffArmed = false;
  fanControllerSetProfile(lockOffProfile);
  fanControllerSetLockedOff(nowMs);
  return jsonOk();
}

}  // namespace

ControlApiResponse controlApiDispatch(const ControlApiRequest& request) {
  if (request.path == "/api/status" && request.method == "GET") return statusResponse(request.profile);
  if (request.path == "/api/events" && request.method == "GET") {
    unsigned long start = 0;
    unsigned long end = 0;
    unsigned long offset = 0;
    unsigned long limit = 32;
    if (!parseUnsignedParameter(request.eventStart, &start) ||
        !parseUnsignedParameter(request.eventEnd, &end) || start >= end ||
        end > 0xFFFFFFFFUL) {
      return jsonError(400, "INVALID_EVENT_RANGE");
    }
    if (request.eventOffset.length() &&
        (!parseUnsignedParameter(request.eventOffset, &offset) || offset > 512UL)) {
      return jsonError(400, "INVALID_EVENT_OFFSET");
    }
    if (request.eventLimit.length() &&
        (!parseUnsignedParameter(request.eventLimit, &limit) || limit < 1 || limit > 32)) {
      return jsonError(400, "INVALID_EVENT_LIMIT");
    }
    const String kind = request.eventKind.length() ? request.eventKind : "all";
    if (kind != "all" && kind != "activity" && kind != "automatic" &&
        kind != "manual" && kind != "force" && kind != "pause" &&
        kind != "locked_off" && kind != "resume") {
      return jsonError(400, "INVALID_EVENT_KIND");
    }
    const FanControllerStatus status = fanControllerGetStatus(millis());
    return response(200, "application/json; charset=utf-8",
                    statisticsStoreBuildEventsJson(
                        static_cast<uint32_t>(start), static_cast<uint32_t>(end), kind,
                        static_cast<uint16_t>(offset), static_cast<uint8_t>(limit),
                        status.fanRunMs));
  }
  if (request.path == "/api/stats" && request.method == "GET") {
    const FanControllerStatus status = fanControllerGetStatus(millis());
    unsigned long eventOffset = 0;
    if (request.eventOffset.length()) {
      char* end = nullptr;
      eventOffset = strtoul(request.eventOffset.c_str(), &end, 10);
      if (end == request.eventOffset.c_str() || *end != '\0' || eventOffset > 512UL) {
        return jsonError(400, "INVALID_EVENT_OFFSET");
      }
    }
    return response(200, "application/json; charset=utf-8",
                    statisticsStoreBuildJson(request.mode, status.fanOn, status.fanRunMs,
                                             static_cast<uint16_t>(eventOffset)));
  }
  if (request.path == "/api/time" && request.method == "POST") {
    const String epochText = formValue(request.body, "unixTime");
    const String offsetText = formValue(request.body, "utcOffsetMinutes");
    if (!epochText.length() || !offsetText.length()) return jsonError(400, "TIME_REQUIRED");
    char* end = nullptr;
    const unsigned long epoch = strtoul(epochText.c_str(), &end, 10);
    if (end == epochText.c_str() || *end != '\0') return jsonError(400, "INVALID_TIME");
    const long offset = strtol(offsetText.c_str(), &end, 10);
    if (end == offsetText.c_str() || *end != '\0' || offset < -720 || offset > 840) {
      return jsonError(400, "INVALID_TIMEZONE_OFFSET");
    }
    if (!statisticsStoreSetClock(epoch, static_cast<int16_t>(offset))) {
      return jsonError(400, "INVALID_TIME");
    }
    return jsonOk("{\"synced\":true}");
  }
  if (request.path == "/api/settings" && request.method == "GET") return settingsResponse(request.profile);
  if (request.path == "/api/settings" && request.method == "POST") {
    if (!profileMatches(request.profile)) return jsonError(409, "PROFILE_MISMATCH");
    if (!request.hasSettings) return jsonError(400, "all five timing settings are required");
    if (!settingsStoreSave(request.profile, request.settings)) {
      return jsonError(400, "timing value is not in the allowed options");
    }
    return jsonOk();
  }
  if (request.path == "/api/settings/reset" && request.method == "POST") {
    if (!profileMatches(request.profile)) return jsonError(409, "PROFILE_MISMATCH");
    if (!settingsStoreReset(request.profile)) return jsonError(500, "failed to reset timing settings");
    return jsonOk();
  }
  if (request.path == "/api/timed" && request.method == "POST") return timedResponse(request);
  if (request.path == "/api/test-fan" && request.method == "POST") {
    if (!profileMatches(request.profile)) return jsonError(409, "PROFILE_MISMATCH");
    if (!replacementAllowed(request.replace)) return jsonError(409, "当前有限时任务，请先确认停止并切换");
    fanControllerSetProfile(request.profile);
    fanControllerStartForceTest(settingsStoreGet(request.profile).manualExhaustMs, millis());
    return jsonOk();
  }
  if (request.path == "/api/auto" && request.method == "POST") return autoResponse(request);
  if (request.path == "/api/lockoff" && request.method == "POST") return lockOffResponse(request);
  return jsonError(404, "Not found");
}

bool controlApiParseTimeValue(const String& value, bool allowZero, unsigned long* parsedValue) {
  if (parsedValue == nullptr || !value.length()) return false;

  // 页面最大选项为 10 分钟；逐位解析并在运算前限幅，拒绝符号、后缀和溢出值。
  constexpr unsigned long kMaximumAllowedTimeMs = 600000UL;
  unsigned long result = 0;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (character < '0' || character > '9') return false;
    const unsigned long digit = static_cast<unsigned long>(character - '0');
    if (result > (kMaximumAllowedTimeMs - digit) / 10UL) return false;
    result = result * 10UL + digit;
  }

  if (!settingsStoreIsAllowedTime(result, allowZero)) return false;
  *parsedValue = result;
  return true;
}
