// 满载验收：30 天汇总 + 256 条事件同时满载时的快照大小与写入成本。
#include <chrono>
#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <cstring>
#include "Arduino.h"
inline time_t fakeNow = 1790697600;  // UTC+8 2026-09-30 00:00
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
  preferences.putShort("tzoffset", 480);

  statisticsStoreBegin();
  assert(statisticsStoreSetClock(static_cast<uint32_t>(fakeNow), 480));

  // 连续 30 个自然日：每天一次自动排风（跑满设定时长）+ 一次活动触发。
  for (int day = 0; day < 30; ++day) {
    fakeNow += 3600;  // 每天 01:00
    fakeMillis += 3600UL * 1000UL;
    statisticsStoreFanStarted(kAutomatic, fakeMillis, 600);
    fakeMillis += 300000UL;
    fakeNow += 300;
    statisticsStoreFanStopped(300000);
    statisticsStoreRecordActivity(120);
    fakeNow += 86400 - 3600 - 300;  // 推进到次日零点
    fakeMillis += (86400UL - 3600UL - 300UL) * 1000UL;
  }

  // 在最近一天内灌满事件容量：每 60 秒一次不合并的触发。
  for (int i = 0; i < 700; ++i) {
    fakeNow += 60;
    fakeMillis += 60000UL;
    statisticsStoreRecordActivity(30);
  }
  statisticsStoreTick();

  const uint32_t now = static_cast<uint32_t>(fakeNow);
  const size_t expected = sizeof(StoreHeaderV5) + dailyCount * sizeof(DailyRecordV5) +
                          eventCount * sizeof(EventRecordV5);
  const char* slot = slotPathForGeneration(generation);
  const size_t actual = files[slot].size();

  // 写入与校验成本（主机 CPU 参考值，非 ESP32 实测）。
  const auto t0 = std::chrono::steady_clock::now();
  writeSnapshotToSlot(slotPathForGeneration(generation + 1), generation + 1, 0);
  const auto t1 = std::chrono::steady_clock::now();
  const bool verified = verifySnapshotInSlot(slotPathForGeneration(generation + 1), generation + 1);
  const auto t2 = std::chrono::steady_clock::now();

  // SPIFFS 占用：252 B/数据页 + 每 63 个数据页一个索引页，按 4 KiB block 取整。
  const size_t dataPages = (actual + 251) / 252;
  const size_t indexPages = (dataPages + 62) / 63;
  const size_t totalPages = dataPages + indexPages;
  const size_t blocks = (totalPages + 15) / 16;
  const size_t spiffsBytes = blocks * 4096;
  const size_t partition = StatisticsConfig::kStatisticsPartitionBytes;

  std::cout << "{\"dailyCount\":" << dailyCount
            << ",\"eventCount\":" << eventCount
            << ",\"snapshotBytes\":" << actual
            << ",\"expectedBytes\":" << expected
            << ",\"headerBytes\":" << sizeof(StoreHeaderV5)
            << ",\"dailyRecordBytes\":" << sizeof(DailyRecordV5)
            << ",\"eventRecordBytes\":" << sizeof(EventRecordV5)
            << ",\"persistUs\":" << std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
            << ",\"verifyUs\":" << std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count()
            << ",\"verifyOk\":" << (verified ? "true" : "false")
            << ",\"dataPages\":" << dataPages
            << ",\"indexPages\":" << indexPages
            << ",\"blocks\":" << blocks
            << ",\"spiffsBytes\":" << spiffsBytes
            << ",\"twoSlotBytes\":" << spiffsBytes * 2
            << ",\"partitionBytes\":" << partition
            << ",\"remainingBytes\":" << (partition - spiffsBytes * 2)
            << ",\"droppedThrough\":" << droppedThrough
            << ",\"day0Coverage\":" << int(daily[0].metricCoverage)
            << "}" << std::endl;
  return 0;
}
