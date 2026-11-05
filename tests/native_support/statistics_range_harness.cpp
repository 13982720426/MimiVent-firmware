// 范围查询验证：选桶、coveredSeconds、coverage、days、detail、错误码。
#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <cstring>
#include "Arduino.h"
inline time_t fakeNow = 1793287800;  // UTC+8 2026-10-29 23:30
inline time_t testTime(time_t* out) { if (out) *out = fakeNow; return fakeNow; }
inline int testSetTime(const timeval* value, const void*) { fakeNow = value->tv_sec; return 0; }
#define time testTime
#define settimeofday testSetTime
#include "../../SettingsStore.h"
TimingSettings settingsStoreGet(TimingProfile) { return {30000, 30000, 120000, 60000, 300000}; }
#include "../../StatisticsStore.cpp"
#undef time
#undef settimeofday

static void emit(const char* name, uint32_t from, uint32_t to, int32_t tz) {
  uint8_t error = 0;
  const String body = statisticsStoreBuildRangeJson(from, to, tz, &error);
  std::cout << name << '\t' << int(error) << '\t' << body.c_str() << '\n';
}

int main() {
  Preferences preferences;
  preferences.begin("MimiVent", false);
  preferences.putBool("statsv3", true);
  preferences.putShort("tzoffset", 480);

  statisticsStoreBegin();
  assert(statisticsStoreSetClock(static_cast<uint32_t>(fakeNow), 480));

  // 23:30 起自动排风跑 300 秒（设定 600）
  statisticsStoreFanStarted(kAutomatic, fakeMillis, 600);
  fakeNow += 300;
  fakeMillis += 300000;
  statisticsStoreFanStopped(300000);

  // 活动段跨午夜：23:59 -> 次日 00:01
  fakeNow = 1793289540;
  fakeMillis += 6000;
  statisticsStoreRecordActivity(120);
  fakeNow = 1793289660;
  fakeMillis += 120000;
  statisticsStoreRecordActivity(120);
  statisticsStoreTick();

  const uint32_t day29 = 1793203200;   // 2026-10-29 00:00 +08
  const uint32_t day30 = 1793289600;
  const uint32_t day31 = 1793376000;

  emit("single", day29, day30, 28800);
  emit("twoDays", day29, day31, 28800);
  emit("sevenDays", day29 - 5 * 86400, day31, 28800);
  emit("todayPartial", day29, day30 - 3600, 28800);       // 23:00 截断，末桶 0 覆盖
  emit("badTz", day29, day30, 0);
  emit("notMidnight", day29 + 300, day30, 28800);
  emit("tooLarge", day29 - 30 * 86400, day31, 28800);
  emit("outOfRetention", day29 - 90 * 86400, day29 - 89 * 86400, 28800);

  // 事件接口与统计接口共用同一份完整性判定。
  const String eventsSingle = statisticsStoreBuildEventsJson(day29, day30, "all", 0, 32, 0, 0);
  const String eventsMulti = statisticsStoreBuildEventsJson(day29, day31, "all", 0, 32, 0, 0);
  std::cout << "eventsSingle\t0\t" << eventsSingle.c_str() << '\n';
  std::cout << "eventsMulti\t0\t" << eventsMulti.c_str() << '\n';

  // 未校时：另起干净数据集，验证 timeSynced=false + pendingUnsynced。
  memset(daily, 0, sizeof(daily));
  memset(events, 0, sizeof(events));
  dailyCount = 0; eventCount = 0; generation = 0; nextEventId = 1;
  droppedThrough = 0; ready = false; clockReady = false;
  files.clear();
  fakeNow = 0;
  fakeMillis = 1000;
  statisticsStoreBegin();
  statisticsStoreFanStarted(kForce, fakeMillis);
  fakeMillis += 60000;
  statisticsStoreFanStopped(60000);
  statisticsStoreRecordActivity(120);
  emit("unsynced", day29, day30, 28800);
  return 0;
}
