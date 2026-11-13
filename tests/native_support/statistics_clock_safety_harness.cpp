#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <cstdint>

inline time_t fakeNow = 0;
inline time_t testTime(time_t* out) { if (out) *out = fakeNow; return fakeNow; }
inline int testSetTime(const timeval* value, const void*) { fakeNow = value->tv_sec; return 0; }
#define time testTime
#define settimeofday testSetTime
#include "../../SettingsStore.h"
TimingSettings settingsStoreGet(TimingProfile) { return {30000, 30000, 120000, 60000, 300000}; }
#include "../../StatisticsStore.cpp"
#undef time
#undef settimeofday

int main() {
  Preferences preferences;
  preferences.begin("MimiVent", false);
  preferences.putBool("statsv3", true);
  statisticsStoreBegin();

  assert(!statisticsStoreTimezoneInitialized());
  assert(!statisticsStoreSetClockFromSource(1800000000UL, 480,
                                            StatisticsClockSource::kNas));
  assert(!statisticsStoreHasClock());
  assert(!statisticsStoreTimezoneInitialized());
  const String unsyncedStats = statisticsStoreBuildJson("today", false, 0);
  assert(unsyncedStats.indexOf("\"timeSyncSource\":\"none\"") >= 0);

  // BLE/授权初始化先固定手机时区；之后 NAS 只能校 UTC，不能改变偏移。
  assert(statisticsStoreSetClockFromSource(1800000000UL, 480,
                                           StatisticsClockSource::kPhone));
  assert(statisticsStoreTimezoneInitialized());
  assert(statisticsStoreTimezoneOffsetSeconds() == 480 * 60);
  const String phoneStats = statisticsStoreBuildJson("today", false, 0);
  assert(phoneStats.indexOf("\"timeSyncSource\":\"phone\"") >= 0);
  uint8_t rangeError = 0xFF;
  const uint32_t todayStart = localDayStartOfEpoch(static_cast<uint32_t>(fakeNow));
  const String phoneRange = statisticsStoreBuildRangeJson(
      todayStart, static_cast<uint32_t>(fakeNow + 1),
      statisticsStoreTimezoneOffsetSeconds(), &rangeError);
  assert(rangeError == 0);
  assert(phoneRange.indexOf("\"timeSyncSource\":\"phone\"") >= 0);
  assert(statisticsStoreSetClockFromSource(static_cast<uint32_t>(fakeNow + 1), -300,
                                           StatisticsClockSource::kNas));
  assert(statisticsStoreTimezoneOffsetSeconds() == 480 * 60);
  const String nasStats = statisticsStoreBuildJson("today", false, 0);
  assert(nasStats.indexOf("\"timeSyncSource\":\"nas\"") >= 0);
  const String nasRange = statisticsStoreBuildRangeJson(
      todayStart, static_cast<uint32_t>(fakeNow + 1),
      statisticsStoreTimezoneOffsetSeconds(), &rangeError);
  assert(rangeError == 0);
  assert(nasRange.indexOf("\"timeSyncSource\":\"nas\"") >= 0);
  assert(statisticsStoreClockSyncSafe());

  // 已有时钟时，排风仍运行不能跳变 UTC。
  statisticsStoreFanStarted(kAutomatic, fakeMillis, 180);
  assert(!statisticsStoreClockSyncSafe());
  assert(!statisticsStoreSetClockFromSource(static_cast<uint32_t>(fakeNow + 30), 480,
                                            StatisticsClockSource::kNas));
  statisticsStoreFanStopped(30000);
  assert(statisticsStoreClockSyncSafe());

  // 最近可信 NAS 样本优先：不同的手机时间不得覆盖。
  assert(!statisticsStoreSetClockFromSource(static_cast<uint32_t>(fakeNow + 30), 480,
                                            StatisticsClockSource::kPhone));

  // 运行 60 秒后允许手机作为备用源；后续回拨在事件上标记 bit2。
  fakeMillis += 60000;
  assert(statisticsStoreSetClockFromSource(static_cast<uint32_t>(fakeNow + 1), 480,
                                           StatisticsClockSource::kPhone));
  statisticsStoreRecordLockedOff();
  const uint32_t previous = static_cast<uint32_t>(fakeNow);
  assert(statisticsStoreSetClockFromSource(previous - 10, 480,
                                           StatisticsClockSource::kNas));
  statisticsStoreRecordAutoResume();
  assert((events[eventCount - 1].flags & kEventFlagClockWentBackwards) != 0);
  assert((persistentFlags & kHeaderFlagClockWentBackwards) != 0);

  // 活动期间即使墙上时钟发生回拨，也按 monotonic 合并并保留单调跨度。
  fakeNow = 1800000000;
  fakeMillis += 10000;
  statisticsStoreRecordActivity(120);
  const size_t activityCountBeforeExtension = eventCount;
  fakeMillis += 30000;
  fakeNow -= 300;
  statisticsStoreRecordActivity(120);
  assert(eventCount == activityCountBeforeExtension);
  assert(events[eventCount - 1].reserved == 2);
  assert(events[eventCount - 1].durationSeconds == 30);
  assert((events[eventCount - 1].flags & kEventFlagClockWentBackwards) != 0);
  assert(!statisticsStoreClockSyncSafe());
  return 0;
}
