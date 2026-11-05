// 每日活动/入厕指标写入验证：跨午夜切分、段数归属、覆盖位。
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

static DailyRecord* dayFor(uint32_t dayKey) {
  const size_t index = findDaily(dayKey);
  return index < kDailyCapacity ? &daily[index] : nullptr;
}

int main() {
  Preferences preferences;
  preferences.begin("MimiVent", false);
  preferences.putBool("statsv3", true);
  preferences.putShort("tzoffset", 480);

  statisticsStoreBegin();
  assert(statisticsStoreSetClock(static_cast<uint32_t>(fakeNow), 480));

  // ---- 自动排风：设定 600 秒，实际跑 300 秒（正好一半）----
  // 本场景没有前置感应活动：入厕时长取活动跨度，拿不到就不计样本
  // （用排风时长冒充会让平均值系统性偏大）。
  statisticsStoreFanStarted(kAutomatic, fakeMillis, 600);
  fakeNow += 300;
  fakeMillis += 300000;
  statisticsStoreFanStopped(300000);

  // ---- 活动段跨午夜：23:59 触发，00:01 再触发（间隔 120 秒 = 合并窗口）----
  fakeNow = 1793289540;  // 23:59:00
  fakeMillis += 6000;
  statisticsStoreRecordActivity(120);
  fakeNow = 1793289660;  // 次日 00:01:00
  fakeMillis += 120000;
  statisticsStoreRecordActivity(120);
  statisticsStoreTick();

  DailyRecord* day29 = dayFor(20261029);
  DailyRecord* day30 = dayFor(20261030);
  assert(day29 && day30);

  std::cout << "{\"toiletCount\":" << day29->toiletSampleCount
            << ",\"toiletDuration\":" << day29->toiletDurationSeconds
            << ",\"toiletCoverage\":" << ((day29->metricCoverage & kMetricToilet) ? 1 : 0)
            << ",\"seg29\":" << day29->activitySegmentCount
            << ",\"seg30\":" << day30->activitySegmentCount
            << ",\"trig29_23\":" << day29->activityTriggerBuckets[23]
            << ",\"trig30_0\":" << day30->activityTriggerBuckets[0]
            << ",\"segBucket29_23\":" << day29->activitySegmentBuckets[23]
            << ",\"segBucket30_0\":" << day30->activitySegmentBuckets[0]
            << ",\"span29\":" << day29->activitySpanSeconds
            << ",\"span30\":" << day30->activitySpanSeconds
            << ",\"spanBucket29_23\":" << day29->activitySpanBuckets[23]
            << ",\"spanBucket30_0\":" << day30->activitySpanBuckets[0]
            << ",\"runtime29\":" << day29->runtimeBuckets[23]
            << ",\"cov29\":" << int(day29->metricCoverage)
            << ",\"cov30\":" << int(day30->metricCoverage)
            << "}" << std::endl;
  return 0;
}
