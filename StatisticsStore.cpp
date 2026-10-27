// BLE/网页共用的统计存储。采用 SPIFFS 双槽快照，避免频繁写 NVS 并保留 OTA 分区。
#include "StatisticsStore.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <time.h>
#include <sys/time.h>

namespace {

constexpr uint32_t kMagic = 0x4D565354;  // MVST
constexpr uint16_t kSchema = 1;
constexpr size_t kDailyCapacity = 365;
constexpr size_t kEventCapacity = 512;
constexpr uint32_t kSecondsPerDay = 86400UL;
constexpr uint32_t kRetentionSeconds = 30UL * kSecondsPerDay;
constexpr uint32_t kDailyRetentionSeconds = 365UL * kSecondsPerDay;
// ESP32-C3 无后备电池时钟。高位表示低位保存本次开机 millis 秒数。
constexpr uint32_t kUnsyncedTimeFlag = 0x80000000UL;
constexpr uint32_t kUnsyncedTimeMask = ~kUnsyncedTimeFlag;
constexpr uint32_t kMillisWrapSeconds = 4294967UL;
constexpr char kPreferencesNamespace[] = "MimiVent";
constexpr char kFilesystemReadyKey[] = "statsfs";
constexpr char kSlotA[] = "/stats-a.bin";
constexpr char kSlotB[] = "/stats-b.bin";

enum : uint8_t {
  kActivity = 1,
  kAutomatic = 2,
  kManual = 3,
  kForce = 4,
  kPause = 5,
  kLockedOff = 6,
  kAutoResume = 7
};
enum : uint8_t { kActive = 1, kComplete = 2, kInterrupted = 3 };

struct DailyRecord {
  uint32_t dayKey;
  uint32_t runtimeSeconds;
  uint16_t activityCount;
  uint16_t automaticCount;
};

struct EventRecord {
  uint32_t unixSeconds;
  uint32_t durationSeconds;
  uint8_t kind;
  uint8_t state;
  uint16_t reserved;
};

struct StoreHeader {
  uint32_t magic;
  uint16_t schema;
  uint16_t dailyCount;
  uint16_t eventCount;
  uint16_t reserved;
  uint32_t generation;
  uint32_t checksum;
};

DailyRecord daily[kDailyCapacity]{};
EventRecord events[kEventCapacity]{};
size_t dailyCount = 0;
size_t eventCount = 0;
uint32_t generation = 0;
bool ready = false;
bool clockReady = false;
int16_t timezoneOffsetMinutes = 0;
size_t activeEventIndex = kEventCapacity;

uint32_t checksumBytes(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

uint32_t currentEpoch() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<uint32_t>(now) : 0;
}

bool isUnsyncedTime(uint32_t value) {
  return (value & kUnsyncedTimeFlag) != 0;
}

uint32_t eventTimestamp() {
  const uint32_t epoch = currentEpoch();
  if (epoch) return epoch;
  return kUnsyncedTimeFlag | ((millis() / 1000UL) & kUnsyncedTimeMask);
}

bool mountStatisticsFilesystem() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) {
    Serial.println("Statistics: NVS unavailable; refusing to format SPIFFS");
    return false;
  }

  const bool previouslyInitialized = preferences.getBool(kFilesystemReadyKey, false);
  if (SPIFFS.begin(false)) {
    if (!previouslyInitialized &&
        preferences.putBool(kFilesystemReadyKey, true) != sizeof(bool)) {
      Serial.println("Statistics: failed to persist filesystem marker");
      SPIFFS.end();
      preferences.end();
      return false;
    }
    preferences.end();
    return true;
  }

  // 只在首次初始化时格式化空分区；已初始化过的文件系统挂载失败时保留现场，
  // 禁止自动擦除历史统计，交由维护流程明确处理。
  if (previouslyInitialized) {
    Serial.println("Statistics: SPIFFS mount failed; history preserved, statistics disabled");
    preferences.end();
    return false;
  }

  if (!SPIFFS.format() || !SPIFFS.begin(false)) {
    Serial.println("Statistics: first-use SPIFFS initialization failed");
    preferences.end();
    return false;
  }
  if (preferences.putBool(kFilesystemReadyKey, true) != sizeof(bool)) {
    Serial.println("Statistics: failed to persist filesystem marker; disabling statistics");
    SPIFFS.end();
    preferences.end();
    return false;
  }
  preferences.end();
  return true;
}

uint32_t currentDayKey() {
  const uint32_t epoch = currentEpoch();
  if (!epoch) return 0;
  const time_t localEpoch = static_cast<time_t>(static_cast<int64_t>(epoch) +
                            static_cast<int32_t>(timezoneOffsetMinutes) * 60);
  struct tm parts{};
  gmtime_r(&localEpoch, &parts);
  return static_cast<uint32_t>((parts.tm_year + 1900) * 10000 +
                               (parts.tm_mon + 1) * 100 + parts.tm_mday);
}

uint32_t dayKeyForEpoch(uint32_t epoch) {
  if (!epoch) return 0;
  const time_t localEpoch = static_cast<time_t>(static_cast<int64_t>(epoch) +
                            static_cast<int32_t>(timezoneOffsetMinutes) * 60);
  struct tm parts{};
  gmtime_r(&localEpoch, &parts);
  return static_cast<uint32_t>((parts.tm_year + 1900) * 10000 +
                               (parts.tm_mon + 1) * 100 + parts.tm_mday);
}

size_t findDaily(uint32_t dayKey) {
  for (size_t i = 0; i < dailyCount; ++i) {
    if (daily[i].dayKey == dayKey) return i;
  }
  return kDailyCapacity;
}

DailyRecord* ensureDaily(uint32_t dayKey) {
  if (!dayKey) return nullptr;
  size_t index = findDaily(dayKey);
  if (index < kDailyCapacity) return &daily[index];
  if (dailyCount == kDailyCapacity) {
    memmove(daily, daily + 1, (kDailyCapacity - 1) * sizeof(DailyRecord));
    --dailyCount;
  }
  daily[dailyCount] = {dayKey, 0, 0, 0};
  return &daily[dailyCount++];
}

bool readHeader(const char* path, StoreHeader* header) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file || file.size() < sizeof(StoreHeader)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(header), sizeof(StoreHeader)) !=
      sizeof(StoreHeader)) return false;
  if (header->magic != kMagic || header->schema != kSchema ||
      header->dailyCount > kDailyCapacity || header->eventCount > kEventCapacity) {
    return false;
  }
  file.close();
  return true;
}

bool readSlot(const char* path, StoreHeader* header) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file || file.size() < sizeof(StoreHeader)) return false;
  if (file.read(reinterpret_cast<uint8_t*>(header), sizeof(StoreHeader)) !=
      sizeof(StoreHeader)) return false;
  if (header->magic != kMagic || header->schema != kSchema ||
      header->dailyCount > kDailyCapacity || header->eventCount > kEventCapacity) return false;
  const size_t dailyBytes = header->dailyCount * sizeof(DailyRecord);
  const size_t eventBytes = header->eventCount * sizeof(EventRecord);
  if (file.size() != sizeof(StoreHeader) + dailyBytes + eventBytes) return false;
  if (file.read(reinterpret_cast<uint8_t*>(daily), dailyBytes) != dailyBytes ||
      file.read(reinterpret_cast<uint8_t*>(events), eventBytes) != eventBytes) {
    return false;
  }
  uint32_t expected = header->checksum;
  header->checksum = 0;
  uint32_t checksum = checksumBytes(reinterpret_cast<uint8_t*>(header), sizeof(*header));
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(daily), dailyBytes) ^ checksum;
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(events), eventBytes) ^ checksum;
  header->checksum = expected;
  file.close();
  return checksum == expected;
}

bool persist() {
  if (!ready) return false;
  StoreHeader header{kMagic, kSchema, static_cast<uint16_t>(dailyCount),
                     static_cast<uint16_t>(eventCount), 0, generation + 1, 0};
  uint32_t checksum = checksumBytes(reinterpret_cast<uint8_t*>(&header), sizeof(header));
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(daily), dailyCount * sizeof(DailyRecord)) ^ checksum;
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(events), eventCount * sizeof(EventRecord)) ^ checksum;
  header.checksum = checksum;
  const char* path = (header.generation & 1U) ? kSlotA : kSlotB;
  File file = SPIFFS.open(path, FILE_WRITE);
  if (!file) return false;
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
      file.write(reinterpret_cast<const uint8_t*>(daily), dailyCount * sizeof(DailyRecord)) == dailyCount * sizeof(DailyRecord) &&
      file.write(reinterpret_cast<const uint8_t*>(events), eventCount * sizeof(EventRecord)) == eventCount * sizeof(EventRecord);
  file.flush();
  file.close();
  if (written) generation = header.generation;
  return written;
}

void prune() {
  const uint32_t now = currentEpoch();
  if (!now) return;
  const uint32_t eventCutoff = now > kRetentionSeconds ? now - kRetentionSeconds : 0;
  size_t out = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    if (events[i].state == kActive || !events[i].unixSeconds || events[i].unixSeconds >= eventCutoff) {
      events[out++] = events[i];
    }
  }
  eventCount = out;

  const uint32_t dayCutoff = now > kDailyRetentionSeconds ? now - kDailyRetentionSeconds : 0;
  const uint32_t cutoffKey = dayKeyForEpoch(dayCutoff);
  out = 0;
  for (size_t i = 0; i < dailyCount; ++i) {
    if (daily[i].dayKey >= cutoffKey) daily[out++] = daily[i];
  }
  dailyCount = out;
}

void findActiveEvent() {
  activeEventIndex = kEventCapacity;
  for (size_t i = eventCount; i > 0; --i) {
    if (events[i - 1].state == kActive) {
      activeEventIndex = i - 1;
      return;
    }
  }
}

void addEvent(uint8_t kind, uint8_t state, uint32_t durationSeconds = 0) {
  const uint32_t timestamp = eventTimestamp();
  if (eventCount == kEventCapacity) {
    memmove(events, events + 1, (kEventCapacity - 1) * sizeof(EventRecord));
    --eventCount;
    if (activeEventIndex < kEventCapacity) {
      activeEventIndex = activeEventIndex == 0 ? kEventCapacity : activeEventIndex - 1;
    }
  }
  events[eventCount] = {timestamp, durationSeconds, kind, state, 0};
  if (kind == kActivity) {
    DailyRecord* day = ensureDaily(currentDayKey());
    if (day && day->activityCount < 0xFFFF) ++day->activityCount;
  } else if (kind == kAutomatic) {
    DailyRecord* day = ensureDaily(currentDayKey());
    if (day && day->automaticCount < 0xFFFF) ++day->automaticCount;
  }
  ++eventCount;
  prune();
  if (kind != kActivity && state == kActive) findActiveEvent();
  persist();
}

const char* kindName(uint8_t kind) {
  switch (kind) {
    case kActivity: return "activity";
    case kAutomatic: return "automatic";
    case kManual: return "manual";
    case kForce: return "force";
    case kPause: return "pause";
    case kLockedOff: return "locked_off";
    case kAutoResume: return "resume";
    default: return "unknown";
  }
}

const char* stateName(uint8_t state) {
  return state == kActive ? "active" : state == kInterrupted ? "interrupted" : "complete";
}

bool eventInRange(const EventRecord& event, uint32_t start, uint32_t end) {
  return event.unixSeconds >= start && event.unixSeconds < end;
}

}  // namespace

void statisticsStoreBegin() {
  if (!mountStatisticsFilesystem()) {
    Serial.println("Statistics: SPIFFS unavailable; statistics disabled");
    return;
  }
  StoreHeader a{}, b{};
  const bool headerA = readHeader(kSlotA, &a);
  const bool headerB = readHeader(kSlotB, &b);
  const bool preferA = headerA && (!headerB || a.generation >= b.generation);
  bool loaded = false;
  StoreHeader selected{};
  if (preferA) {
    loaded = readSlot(kSlotA, &a);
    if (loaded) selected = a;
    if (!loaded && headerB) { loaded = readSlot(kSlotB, &b); if (loaded) selected = b; }
  } else if (headerB) {
    loaded = readSlot(kSlotB, &b);
    if (loaded) selected = b;
    if (!loaded && headerA) { loaded = readSlot(kSlotA, &a); if (loaded) selected = a; }
  }
  if (loaded) {
    dailyCount = selected.dailyCount;
    eventCount = selected.eventCount;
    generation = selected.generation;
  }
  // 本次开机前尚未校时的事件无法在重启后还原日期，丢弃其不完整时间戳；
  // 已有可信时间戳的运行事件则保留并标记为重启中断。
  size_t out = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    if (isUnsyncedTime(events[i].unixSeconds)) continue;
    if (events[i].state == kActive) events[i].state = kInterrupted;
    events[out++] = events[i];
  }
  eventCount = out;
  ready = true;
  findActiveEvent();
  prune();
  persist();
  Serial.printf("Statistics ready; daily=%u events=%u\n",
                static_cast<unsigned>(dailyCount), static_cast<unsigned>(eventCount));
}

bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes) {
  if (unixSeconds < 1700000000UL || utcOffsetMinutes < -720 || utcOffsetMinutes > 840) return false;
  timeval tv{static_cast<time_t>(unixSeconds), 0};
  if (settimeofday(&tv, nullptr) != 0) return false;
  timezoneOffsetMinutes = utcOffsetMinutes;
  clockReady = true;

  // 回填本次开机以来发生的未校时事件，并把已结束排风的时长归入对应日期。
  const uint32_t uptimeNow = millis() / 1000UL;
  for (size_t i = 0; i < eventCount; ++i) {
    EventRecord& event = events[i];
    if (!isUnsyncedTime(event.unixSeconds)) continue;
    const uint32_t eventUptime = event.unixSeconds & kUnsyncedTimeMask;
    const uint32_t elapsed = uptimeNow >= eventUptime
        ? uptimeNow - eventUptime
        : uptimeNow + kMillisWrapSeconds - eventUptime;
    if (elapsed >= unixSeconds || unixSeconds - elapsed < 1700000000UL) {
      event.unixSeconds = 0;
      continue;
    }
    event.unixSeconds = unixSeconds - elapsed;
    DailyRecord* day = ensureDaily(dayKeyForEpoch(event.unixSeconds));
    if (!day) continue;
    if (event.kind == kActivity && day->activityCount < 0xFFFF) {
      ++day->activityCount;
    } else if (event.kind == kAutomatic && day->automaticCount < 0xFFFF) {
      ++day->automaticCount;
    }
    if (event.kind != kActivity && event.state != kActive) {
      day->runtimeSeconds += event.durationSeconds;
    }
  }
  prune();
  findActiveEvent();
  persist();
  return true;
}

bool statisticsStoreHasClock() { return clockReady || currentEpoch() != 0; }

void statisticsStoreRecordActivity() {
  if (!ready) return;
  addEvent(kActivity, kComplete);
}

void statisticsStoreRecordTimedPause(unsigned long durationMs) {
  if (!ready) return;
  // 暂停事件记录设定时长；任务可被提前替换，因此这是用户设置的时长。
  addEvent(kPause, kComplete, durationMs / 1000UL);
}

void statisticsStoreRecordLockedOff() {
  if (!ready) return;
  // 持续关闭没有预设结束时间，事件中以 0 表示不限时。
  addEvent(kLockedOff, kComplete);
}

void statisticsStoreRecordAutoResume() {
  if (!ready) return;
  // 为持续关闭写入明确的结束事件，App 据此计算真实关闭时长。
  addEvent(kAutoResume, kComplete);
}

void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs) {
  if (!ready || kind == kActivity) return;
  (void)nowMs;
  addEvent(kind, kActive);
}

void statisticsStoreFanStopped(unsigned long durationMs) {
  if (!ready || activeEventIndex >= eventCount) return;
  EventRecord& event = events[activeEventIndex];
  const bool timestampPending = isUnsyncedTime(event.unixSeconds);
  event.state = kComplete;
  event.durationSeconds = durationMs / 1000UL;
  if (!timestampPending) {
    DailyRecord* day = ensureDaily(dayKeyForEpoch(event.unixSeconds));
    if (day) day->runtimeSeconds += event.durationSeconds;
  }
  activeEventIndex = kEventCapacity;
  persist();
}

String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs,
                                uint16_t eventOffset) {
  const uint32_t now = currentEpoch();
  const int64_t signedOffsetSeconds = static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  const int64_t localNow = now ? static_cast<int64_t>(now) + signedOffsetSeconds : 0;
  const uint32_t todayStart = now ? static_cast<uint32_t>((localNow / kSecondsPerDay) * kSecondsPerDay - signedOffsetSeconds) : 0;
  uint8_t daySpan = 1;
  uint8_t bucketSpanHours = 1;
  if (range == "yesterday") { daySpan = 1; bucketSpanHours = 2; }
  else if (range == "threeDays") { daySpan = 3; bucketSpanHours = 8; }
  else if (range == "sevenDays") { daySpan = 7; bucketSpanHours = 24; }
  else if (range != "today") return "{\"error\":\"INVALID_RANGE\"}";

  const uint8_t bucketCount = range == "today" ? 24 : range == "yesterday" ? 12 : daySpan * (24 / bucketSpanHours);
  const uint32_t periodStart = todayStart - (range == "yesterday" ? kSecondsPerDay : range == "threeDays" ? 2 * kSecondsPerDay : range == "sevenDays" ? 6 * kSecondsPerDay : 0);
  uint32_t runtimeSeconds = 0;
  uint32_t activityCount = 0;
  uint32_t automaticCount = 0;
  uint32_t buckets[24]{};
  String eventsJson = "[";
  uint16_t eventTotal = 0;
  uint8_t recentCount = 0;

  for (size_t i = 0; i < dailyCount; ++i) {
    const uint32_t day = daily[i].dayKey;
    // 通过每日汇总的本地日期键判断查询区间；逐日游标从 periodStart 推进。
    for (uint8_t dayIndex = 0; dayIndex < daySpan; ++dayIndex) {
      const uint32_t epoch = periodStart + dayIndex * kSecondsPerDay;
      const uint32_t key = dayKeyForEpoch(epoch);
      if (day == key) {
        runtimeSeconds += daily[i].runtimeSeconds;
        activityCount += daily[i].activityCount;
        automaticCount += daily[i].automaticCount;
      }
    }
  }

  for (size_t i = 0; i < eventCount; ++i) {
    if (!now) continue;
    EventRecord event = events[i];
    // 活动事件的运行时长在下面统一加入一次，避免当前运行时长被重复计入图表。
    if (event.state == kActive && i == activeEventIndex) continue;
    if (!eventInRange(event, periodStart, now + 1)) continue;
    const time_t localEpoch = static_cast<time_t>(static_cast<int64_t>(event.unixSeconds) + signedOffsetSeconds);
    struct tm parts{};
    gmtime_r(&localEpoch, &parts);
    const uint32_t dayIndex = static_cast<uint32_t>((event.unixSeconds - periodStart) / kSecondsPerDay);
    const uint32_t bucket = dayIndex * (24 / bucketSpanHours) + parts.tm_hour / bucketSpanHours;
    if (bucket < bucketCount && event.kind != kActivity) buckets[bucket] += event.durationSeconds;
  }
  if (activeRunMs && activeEventIndex < eventCount && events[activeEventIndex].unixSeconds >= periodStart) {
    runtimeSeconds += activeRunMs / 1000UL;
    const EventRecord& current = events[activeEventIndex];
    const time_t localEpoch = static_cast<time_t>(static_cast<int64_t>(current.unixSeconds) + signedOffsetSeconds);
    struct tm parts{};
    gmtime_r(&localEpoch, &parts);
    const uint32_t dayIndex = static_cast<uint32_t>((current.unixSeconds - periodStart) / kSecondsPerDay);
    const uint32_t bucket = dayIndex * (24 / bucketSpanHours) + parts.tm_hour / bucketSpanHours;
    if (bucket < bucketCount && current.kind != kActivity) buckets[bucket] += activeRunMs / 1000UL;
  }
  if (range == "today" || range == "yesterday") {
    // 今日按滚动 24 小时查询；昨日按本地自然日查询，由 mode 参数决定窗口。
    const uint32_t eventStart = range == "today"
        ? (now > kSecondsPerDay ? now - kSecondsPerDay : 0)
        : periodStart;
    const uint32_t eventEnd = range == "today" ? now + 1 : todayStart;
    for (int i = static_cast<int>(eventCount) - 1; i >= 0; --i) {
      if (!now) break;
      EventRecord event = events[i];
      if (event.state == kActive && static_cast<size_t>(i) == activeEventIndex) event.durationSeconds = activeRunMs / 1000UL;
      if (!eventInRange(event, eventStart, eventEnd)) continue;
      if (eventTotal >= eventOffset && recentCount < 32) {
        if (recentCount++) eventsJson += ',';
        eventsJson += "{\"time\":" + String(event.unixSeconds) + ",\"kind\":\"" + kindName(event.kind) +
                      "\",\"state\":\"" + stateName(event.state) + "\",\"durationSeconds\":" + String(event.durationSeconds) + "}";
      }
      if (eventTotal < 0xFFFF) ++eventTotal;
    }
  }
  eventsJson += ']';
  String json = "{\"timeSynced\":" + String(statisticsStoreHasClock() ? "true" : "false") +
      ",\"active\":" + String(fanOn ? "true" : "false") +
      ",\"range\":\"" + range + "\",\"runtimeSeconds\":" + String(runtimeSeconds) +
      ",\"activityCount\":" + String(activityCount) + ",\"automaticCount\":" + String(automaticCount) + ",\"bucketSeconds\":[";
  for (uint8_t i = 0; i < bucketCount; ++i) { if (i) json += ','; json += String(buckets[i]); }
  json += "],\"events\":" + eventsJson;
  const bool eventsHasMore = static_cast<uint32_t>(eventOffset) + recentCount < eventTotal;
  json += ",\"eventsOffset\":" + String(eventOffset) +
          ",\"eventsNextOffset\":" + String(eventOffset + recentCount) +
          ",\"eventsTotal\":" + String(eventTotal) +
          ",\"eventsHasMore\":" + String(eventsHasMore ? "true" : "false");
  // 昨日页同样需要一条按日汇总，供 App 展示日期、活动次数与总运行时长。
  if (range == "threeDays" || range == "sevenDays") {
    json += ",\"days\":[";
    for (uint8_t dayIndex = 0; dayIndex < daySpan; ++dayIndex) {
      if (dayIndex) json += ',';
      const uint32_t dayKey = dayKeyForEpoch(periodStart + dayIndex * kSecondsPerDay);
      const size_t recordIndex = findDaily(dayKey);
      if (recordIndex == kDailyCapacity) {
        json += "{\"date\":" + String(dayKey) + ",\"runtimeSeconds\":0,\"activityCount\":0,\"automaticCount\":0}";
      } else {
        const DailyRecord& day = daily[recordIndex];
        json += "{\"date\":" + String(day.dayKey) + ",\"runtimeSeconds\":" + String(day.runtimeSeconds) +
                ",\"activityCount\":" + String(day.activityCount) + ",\"automaticCount\":" + String(day.automaticCount) + "}";
      }
    }
    json += ']';
  }
  json += '}';
  return json;
}

String statisticsStoreBuildEventsJson(uint32_t start, uint32_t end, const String& kind,
                                      uint16_t offset, uint8_t limit,
                                      unsigned long activeRunMs) {
  String eventsJson = "[";
  uint16_t total = 0;
  uint8_t count = 0;
  for (int i = static_cast<int>(eventCount) - 1; i >= 0; --i) {
    if (!clockReady) break;
    EventRecord event = events[i];
    if (!eventInRange(event, start, end)) continue;
    const String eventKind = kindName(event.kind);
    if (kind != "all" && kind != eventKind) continue;
    if (event.state == kActive && static_cast<size_t>(i) == activeEventIndex) {
      event.durationSeconds = activeRunMs / 1000UL;
    }
    if (total >= offset && count < limit) {
      if (count++) eventsJson += ',';
      eventsJson += "{\"time\":" + String(event.unixSeconds) +
                    ",\"kind\":\"" + eventKind +
                    "\",\"state\":\"" + stateName(event.state) +
                    "\",\"durationSeconds\":" + String(event.durationSeconds) + "}";
    }
    if (total < 0xFFFF) ++total;
  }
  eventsJson += ']';
  const uint16_t nextOffset = offset + count;
  const bool hasMore = nextOffset < total;
  String json = "{\"start\":" + String(start) + ",\"end\":" + String(end) +
                ",\"kind\":\"" + kind + "\",\"events\":" + eventsJson +
                ",\"offset\":" + String(offset) +
                ",\"nextOffset\":" + String(nextOffset) +
                ",\"total\":" + String(total) +
                ",\"hasMore\":" + String(hasMore ? "true" : "false") + "}";
  return json;
}
