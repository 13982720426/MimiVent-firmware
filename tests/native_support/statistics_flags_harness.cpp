// 事件标志与淘汰语义验证：2038 边界、未校时淘汰、运行时长补回。
#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <cstring>
#include "Arduino.h"
inline time_t fakeNow = 1793287800;
inline time_t testTime(time_t* out) { if (out) *out = fakeNow; return fakeNow; }
inline int testSetTime(const timeval* value, const void*) { fakeNow = value->tv_sec; return 0; }
#define time testTime
#define settimeofday testSetTime
#include "../../SettingsStore.h"
TimingSettings settingsStoreGet(TimingProfile) { return {30000, 30000, 120000, 60000, 300000}; }
#include "../../StatisticsStore.cpp"
#undef time
#undef settimeofday

static void resetStore() {
  memset(daily, 0, sizeof(daily));
  memset(events, 0, sizeof(events));
  dailyCount = 0; eventCount = 0; generation = 0; nextEventId = 1;
  droppedThrough = 0; activeRun = {}; ready = false; clockReady = false;
  persistentFlags = 0; files.clear();
}

int main() {
  Preferences preferences;
  preferences.begin("MimiVent", false);
  preferences.putBool("statsv3", true);
  preferences.putShort("tzoffset", 480);

  // ---- 1) 2038 之后的真实时间戳不得被识别成未校时（D41）----
  resetStore();
  fakeNow = 2208988800;  // 2040-01-01 00:00 UTC，时间戳高位为 1
  fakeMillis = 0;
  statisticsStoreBegin();
  assert(statisticsStoreSetClock(static_cast<uint32_t>(fakeNow), 480));
  statisticsStoreRecordActivity(120);
  const bool lateUnsynced = eventIsUnsynced(events[0]);
  const uint32_t lateTimestamp = events[0].unixSeconds;
  const auto lateJson = statisticsStoreBuildJson("today", false, 0, 0);

  // ---- 2) 未校时事件带 flags.bit0；被淘汰时不推进 droppedThrough（D38）----
  resetStore();
  fakeNow = 0;
  fakeMillis = 1000;
  statisticsStoreBegin();
  statisticsStoreFanStarted(kForce, fakeMillis);
  const uint32_t fanId = activeRun.id;
  for (int i = 0; i < 600; ++i) {
    statisticsStoreRecordActivity(60);
    fakeMillis += 61000;
  }
  const bool anyUnsyncedFlag = (events[0].flags & kEventFlagTimeUnsynced) != 0;
  const uint32_t droppedAfterFlood = droppedThrough;
  const size_t countAfterFlood = eventCount;
  const uint32_t firstUptime = events[0].unixSeconds;

  // 运行停止时记录已被淘汰：必须补回一条完整记录，时长不能丢。
  fakeMillis += 1000;
  statisticsStoreFanStopped(60000);
  bool fanRecordPresent = false;
  for (size_t i = 0; i < eventCount; ++i) if (events[i].id == fanId) fanRecordPresent = true;
  const bool fanRecordUnsynced = fanRecordPresent && (events[eventCount - 1].flags & kEventFlagTimeUnsynced) != 0;

  assert(statisticsStoreSetClock(1793298600, 480));
  uint64_t runtimeAfterSync = 0;
  for (size_t i = 0; i < dailyCount; ++i) {
    for (size_t h = 0; h < 24; ++h) runtimeAfterSync += daily[i].runtimeBuckets[h];
  }

  std::cout << "{\"lateUnsynced\":" << (lateUnsynced ? "true" : "false")
            << ",\"lateTimestamp\":" << lateTimestamp
            << ",\"lateHasActivity\":" << (lateJson.value.find("\"activityCount\":1") != std::string::npos ? "true" : "false")
            << ",\"anyUnsyncedFlag\":" << (anyUnsyncedFlag ? "true" : "false")
            << ",\"droppedAfterFlood\":" << droppedAfterFlood
            << ",\"countAfterFlood\":" << countAfterFlood
            << ",\"firstUptime\":" << firstUptime
            << ",\"fanRecordPresent\":" << (fanRecordPresent ? "true" : "false")
            << ",\"fanRecordUnsynced\":" << (fanRecordUnsynced ? "true" : "false")
            << ",\"runtimeAfterSync\":" << runtimeAfterSync
            << "}" << std::endl;
  return 0;
}
