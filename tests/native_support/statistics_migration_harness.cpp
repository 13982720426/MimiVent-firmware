// schema 4 → 5 迁移的主机验证：构造旧槽位，启动后检查转换结果与幂等性。
#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <cstring>
#include <vector>
#include "Arduino.h"
inline time_t fakeNow = 1793287800;  // UTC+8 的 2026-10-29 23:30
inline time_t testTime(time_t* out) { if (out) *out = fakeNow; return fakeNow; }
inline int testSetTime(const timeval* value, const void*) { fakeNow = value->tv_sec; return 0; }
#define time testTime
#define settimeofday testSetTime
#include "../../SettingsStore.h"
TimingSettings settingsStoreGet(TimingProfile) { return {30000, 30000, 120000, 60000, 300000}; }
#include "../../StatisticsStore.cpp"
#undef time
#undef settimeofday

static uint16_t readU16(const std::vector<uint8_t>& v, size_t off) {
  uint16_t x = 0;
  memcpy(&x, v.data() + off, 2);
  return x;
}

int main() {
  // ---- 构造一份 schema 4 数据：2 天汇总 + 3 条事件，generation 7 → A 槽 ----
  StoreHeaderV4 header{};
  header.magic = kMagic;
  header.schema = kSchemaV4;
  header.dailyCount = 2;
  header.eventCount = 3;
  header.generation = 7;
  header.nextEventId = 99;
  header.droppedThroughEpoch = 0;

  DailyRecordV4 daily4[2]{};
  daily4[0].dayKey = 20261028; daily4[0].activityCount = 7; daily4[0].automaticCount = 2;
  daily4[0].runtimeBuckets[5] = 600; daily4[0].runtimeBuckets[6] = 1800;
  daily4[1].dayKey = 20261029; daily4[1].activityCount = 3; daily4[1].automaticCount = 1;
  daily4[1].runtimeBuckets[23] = 120;

  EventRecordV4 events4[3]{};
  events4[0] = {50u, 1793203200u + 3600u * 5u, 2400u, 2u, 2u, 120u};
  events4[1] = {51u, 1793203200u + 3600u * 6u, 0u, 1u, 2u, 3u};
  events4[2] = {52u, 1793286000u, 30u, 3u, 2u, 0u};

  const size_t dailyBytes = sizeof(daily4);
  const size_t eventBytes = sizeof(events4);
  std::vector<uint8_t> slot(sizeof(StoreHeaderV4) + dailyBytes + eventBytes);
  memcpy(slot.data(), &header, sizeof(header));
  memcpy(slot.data() + sizeof(header), daily4, dailyBytes);
  memcpy(slot.data() + sizeof(header) + dailyBytes, events4, eventBytes);
  header.checksum = 0;
  uint32_t crc = checksumBytes(reinterpret_cast<uint8_t*>(&header), sizeof(header));
  crc = checksumBytes(slot.data() + sizeof(header), dailyBytes) ^ crc;
  crc = checksumBytes(slot.data() + sizeof(header) + dailyBytes, eventBytes) ^ crc;
  header.checksum = crc;
  memcpy(slot.data(), &header, sizeof(header));
  files["/stats-a.bin"] = slot;

  {
    Preferences preferences;
    preferences.begin("MimiVent", false);
    preferences.putBool("statsv3", true);
    preferences.putShort("tzoffset", 480);
  }

  statisticsStoreBegin();
  const String epochFirst = String(statisticsStoreDataEpoch());
  const String todayFirst = statisticsStoreBuildJson("today", false, 0, 0);

  // ---- 再次启动：迁移必须幂等，dataEpoch 与数据保持不变 ----
  memset(daily, 0, sizeof(daily));
  memset(events, 0, sizeof(events));
  dailyCount = 0; eventCount = 0; generation = 0; nextEventId = 0;
  droppedThrough = 0; ready = false;
  statisticsStoreBegin();

  const uint16_t slotASchema = files.count("/stats-a.bin") ? readU16(files["/stats-a.bin"], 4) : 0;
  const uint16_t slotBSchema = files.count("/stats-b.bin") ? readU16(files["/stats-b.bin"], 4) : 0;

  std::cout << "{\"schema\":" << statisticsStoreSchemaVersion()
            << ",\"compat\":" << statisticsSchemaCompatVersion()
            << ",\"daily\":" << dailyCount
            << ",\"events\":" << eventCount
            << ",\"generation\":" << generation
            << ",\"nextEventId\":" << nextEventId
            << ",\"slotASchema\":" << slotASchema
            << ",\"slotBSchema\":" << slotBSchema
            << ",\"coverage\":" << int(daily[0].metricCoverage)
            << ",\"segmentCount\":" << daily[0].activitySegmentCount
            << ",\"spanSeconds\":" << daily[0].activitySpanSeconds
            << ",\"toiletSamples\":" << daily[0].toiletSampleCount
            << ",\"runtimeBucket6\":" << daily[0].runtimeBuckets[6]
            << ",\"eventEndReason0\":" << int(events[0].endReason)
            << ",\"eventReserved0\":" << events[0].reserved
            << ",\"eventKind0\":" << int(events[0].kind)
            << ",\"epochStable\":" << (epochFirst == String(statisticsStoreDataEpoch()) ? "true" : "false")
            << ",\"today\":" << todayFirst.c_str()
            << "}" << std::endl;
  return 0;
}
