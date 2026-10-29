// BLE/网页共用的统计存储。采用 SPIFFS 双槽快照，避免频繁写 NVS 并保留 OTA 分区。
#include "StatisticsStore.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <time.h>
#include <sys/time.h>

#include "StatisticsConfig.h"
#include "StatisticsTypes.h"
#include "FirmwareVersion.h"

namespace {

constexpr uint32_t kMagic = 0x4D565354;  // MVST
constexpr uint16_t kSchema = FIRMWARE_STATISTICS_SCHEMA;
constexpr size_t kDailyCapacity = StatisticsConfig::kSummaryRetentionDays;
constexpr size_t kEventCapacity = 512;
constexpr uint32_t kSecondsPerDay = 86400UL;
constexpr uint32_t kRetentionSeconds =
    static_cast<uint32_t>(StatisticsConfig::kDetailRetentionDays) * kSecondsPerDay;
constexpr uint32_t kDailyRetentionSeconds =
    static_cast<uint32_t>(StatisticsConfig::kSummaryRetentionDays) * kSecondsPerDay;
// ESP32-C3 无后备电池时钟。高位表示低位保存本次开机 millis 秒数。
constexpr uint32_t kUnsyncedTimeFlag = 0x80000000UL;
constexpr uint32_t kUnsyncedTimeMask = ~kUnsyncedTimeFlag;
constexpr uint32_t kMillisWrapSeconds = 4294967UL;
constexpr char kPreferencesNamespace[] = "MimiVent";
// 新版使用独立 statistics 分区。单独的初始化标记避免误读旧版 SPIFFS 状态，
// 同时首次格式化不会清除 NVS 中的运行配置。
constexpr char kFilesystemReadyKey[] = "statsv3";
constexpr char kDataEpochKey[] = "dataepoch";
constexpr char kTimezoneOffsetKey[] = "tzoffset";
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
  uint16_t activityCount;
  uint16_t automaticCount;
  uint32_t runtimeBuckets[24];
};

struct EventRecord {
  uint32_t id;
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
  uint32_t nextEventId;
  EventRecord activeRun;
  uint32_t droppedThroughEpoch;
};

DailyRecord daily[kDailyCapacity]{};
EventRecord events[kEventCapacity]{};
size_t dailyCount = 0;
size_t eventCount = 0;
uint32_t generation = 0;
uint32_t nextEventId = 1;
char deviceIdHex[17] = {};
char dataEpochHex[33] = {};
char bootIdHex[17] = {};
bool ready = false;
bool clockReady = false;
int16_t timezoneOffsetMinutes = 0;
size_t activeEventIndex = kEventCapacity;
EventRecord activeRun{};
uint32_t droppedThroughEpoch = 0;
bool filesystemPreviouslyInitialized = false;

void updateDataEpoch(const uint8_t* epoch) {
  for (size_t i = 0; i < 16; ++i) {
    snprintf(dataEpochHex + i * 2, 3, "%02x", epoch[i]);
  }
}

bool createNewDataEpoch(Preferences* preferences) {
  uint8_t epoch[16]{};
  esp_fill_random(epoch, sizeof(epoch));
  if (preferences->putBytes(kDataEpochKey, epoch, sizeof(epoch)) != sizeof(epoch)) {
    return false;
  }
  updateDataEpoch(epoch);
  return true;
}

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
  filesystemPreviouslyInitialized = previouslyInitialized;
  if (SPIFFS.begin(false, "/statistics", 10, "statistics")) {
    // 首次挂载/升级到独立分区时建立新批次；App 会据此清除旧格式缓存。
    if (!previouslyInitialized && !createNewDataEpoch(&preferences)) {
      Serial.println("Statistics: failed to create data epoch on first mount");
      SPIFFS.end();
      preferences.end();
      return false;
    }
    if (!previouslyInitialized && preferences.putBool(kFilesystemReadyKey, true) != sizeof(bool)) {
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

  if (!SPIFFS.format() || !SPIFFS.begin(false, "/statistics", 10, "statistics")) {
    Serial.println("Statistics: first-use SPIFFS initialization failed");
    preferences.end();
    return false;
  }
  // 文件系统首次格式化后生成新批次，避免 App 把旧缓存误认为本批数据。
  if (!createNewDataEpoch(&preferences)) {
    Serial.println("Statistics: failed to persist data epoch after format");
    SPIFFS.end();
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

DailyRecord* ensureDaily(uint32_t dayKey);
uint64_t min64(uint64_t left, uint64_t right) { return left < right ? left : right; }
uint64_t max64(uint64_t left, uint64_t right) { return left > right ? left : right; }

// 按设备本地小时切分运行区间，小时桶同时作为图表和汇总的唯一来源。
void addRuntimeToDaily(uint32_t startEpoch, uint64_t durationSeconds) {
  if (!startEpoch || !durationSeconds) return;
  uint64_t cursor = startEpoch;
  const uint64_t end = cursor + durationSeconds;
  while (cursor < end && cursor <= UINT32_MAX) {
    const uint32_t cursorEpoch = static_cast<uint32_t>(cursor);
    const uint32_t dayKey = dayKeyForEpoch(cursorEpoch);
    const int64_t localEpoch = static_cast<int64_t>(cursorEpoch) +
        static_cast<int32_t>(timezoneOffsetMinutes) * 60;
    const int64_t nextLocalHour = (localEpoch / 3600 + 1) * 3600;
    const int64_t nextUtcHour = nextLocalHour -
        static_cast<int32_t>(timezoneOffsetMinutes) * 60;
    const uint64_t sliceEnd = min64(end, static_cast<uint64_t>(nextUtcHour));
    const uint64_t sliceSeconds = sliceEnd > cursor ? sliceEnd - cursor : end - cursor;
    DailyRecord* day = ensureDaily(dayKey);
    if (day) {
      const uint8_t hour = (localEpoch % kSecondsPerDay) / 3600;
      const uint64_t total = static_cast<uint64_t>(day->runtimeBuckets[hour]) + sliceSeconds;
      day->runtimeBuckets[hour] = total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
    }
    cursor += sliceSeconds;
  }
}

bool isFanRuntimeKind(uint8_t kind) {
  return kind == kAutomatic || kind == kManual || kind == kForce;
}

// 将区间裁剪到统计窗口后分配到实际覆盖的小时/日桶。
void addRuntimeToBuckets(uint32_t startEpoch, uint64_t durationSeconds,
                         uint32_t periodStart, uint32_t periodEnd,
                         uint8_t bucketSpanHours, uint8_t bucketCount,
                         uint64_t* buckets) {
  if (!startEpoch || !durationSeconds || periodEnd <= periodStart) return;
  uint64_t cursor = max64(startEpoch, periodStart);
  const uint64_t intervalEnd = static_cast<uint64_t>(startEpoch) + durationSeconds;
  const uint64_t end = min64(intervalEnd, periodEnd);
  const int32_t offsetSeconds = static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  const int64_t localPeriodDay =
      (static_cast<int64_t>(periodStart) + static_cast<int32_t>(offsetSeconds)) /
      kSecondsPerDay;
  while (cursor < end && cursor <= UINT32_MAX) {
    const uint32_t cursorEpoch = static_cast<uint32_t>(cursor);
    const int64_t localEpoch = static_cast<int64_t>(cursorEpoch) +
        static_cast<int32_t>(offsetSeconds);
    const int64_t localDay = localEpoch / kSecondsPerDay;
    const uint8_t hour = static_cast<uint8_t>((localEpoch % kSecondsPerDay) / 3600);
    const uint32_t bucketsPerDay = 24 / bucketSpanHours;
    const int64_t dayIndex = localDay - localPeriodDay;
    const int64_t bucketIndex = dayIndex * bucketsPerDay + hour / bucketSpanHours;
    const int64_t nextLocalBucket = localDay * kSecondsPerDay +
        (static_cast<int64_t>(hour / bucketSpanHours) + 1) * bucketSpanHours * 3600;
    const int64_t nextUtcBucket = nextLocalBucket - static_cast<int32_t>(offsetSeconds);
    const uint64_t sliceEnd = min64(end, static_cast<uint64_t>(nextUtcBucket));
    const uint64_t sliceSeconds = sliceEnd > cursor ? sliceEnd - cursor : end - cursor;
    if (bucketIndex >= 0 && bucketIndex < bucketCount) {
      buckets[bucketIndex] += sliceSeconds;
    }
    cursor += sliceSeconds;
  }
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
  daily[dailyCount] = {};
  daily[dailyCount].dayKey = dayKey;
  return &daily[dailyCount++];
}

bool readHeader(const char* path, StoreHeader* header) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  if (file.size() < sizeof(StoreHeader) ||
      file.read(reinterpret_cast<uint8_t*>(header), sizeof(StoreHeader)) != sizeof(StoreHeader)) {
    file.close();
    return false;
  }
  if (header->magic != kMagic || header->schema != kSchema ||
      header->dailyCount > kDailyCapacity || header->eventCount > kEventCapacity) {
    file.close();
    return false;
  }
  file.close();
  return true;
}

bool readSlot(const char* path, StoreHeader* header) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  if (file.size() < sizeof(StoreHeader) ||
      file.read(reinterpret_cast<uint8_t*>(header), sizeof(StoreHeader)) != sizeof(StoreHeader) ||
      header->magic != kMagic || header->schema != kSchema ||
      header->dailyCount > kDailyCapacity || header->eventCount > kEventCapacity) {
    file.close();
    return false;
  }
  uint32_t checksum = 0;
  const size_t eventBytes = header->eventCount * sizeof(EventRecord);
  const uint32_t expected = header->checksum;
  const size_t dailyBytes = header->dailyCount * sizeof(DailyRecord);
  const size_t headerBytes = sizeof(StoreHeader);
  if (file.size() != headerBytes + dailyBytes + eventBytes ||
      file.read(reinterpret_cast<uint8_t*>(daily), dailyBytes) != dailyBytes ||
      file.read(reinterpret_cast<uint8_t*>(events), eventBytes) != eventBytes) {
    file.close();
    return false;
  }
  header->checksum = 0;
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(header), sizeof(*header));
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(daily), dailyBytes) ^ checksum;
  checksum = checksumBytes(reinterpret_cast<uint8_t*>(events), eventBytes) ^ checksum;
  header->checksum = expected;
  file.close();
  return checksum == expected;
}

bool persist() {
  if (!ready) return false;
  StoreHeader header{kMagic, kSchema, static_cast<uint16_t>(dailyCount),
                     static_cast<uint16_t>(eventCount), 0, generation + 1, 0,
                     nextEventId, activeRun, droppedThroughEpoch};
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

void findActiveEvent();

void prune() {
  const uint32_t now = currentEpoch();
  if (!now) return;
  const uint32_t eventCutoff = now > kRetentionSeconds ? now - kRetentionSeconds : 0;
  size_t out = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    if (events[i].state == kActive || !events[i].unixSeconds || events[i].unixSeconds >= eventCutoff) {
      events[out++] = events[i];
    } else if (events[i].unixSeconds && !isUnsyncedTime(events[i].unixSeconds)) {
      droppedThroughEpoch = max(droppedThroughEpoch, events[i].unixSeconds);
    }
  }
  eventCount = out;
  findActiveEvent();

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
  // ID 作为分页游标必须单调且不可复用；耗尽时停止写入，等待显式重置数据批次。
  if (nextEventId == 0) {
    Serial.println("Statistics: event ID exhausted; refusing to reuse IDs");
    return;
  }
  size_t eviction = 0;
  if (eventCount == kEventCapacity) {
    // 待校时运行区间优先保留，避免 PIR 明细挤掉尚未归入小时桶的时长。
    while (eviction < eventCount && isFanRuntimeKind(events[eviction].kind) &&
           isUnsyncedTime(events[eviction].unixSeconds)) ++eviction;
    if (eviction == eventCount) {
      // 全部为待校时运行记录时，不再用活动明细替换它们。
      if (kind == kActivity) return;
      eviction = 0;
    }
    if (!isUnsyncedTime(events[eviction].unixSeconds)) {
      droppedThroughEpoch = max(droppedThroughEpoch, events[eviction].unixSeconds);
    }
    memmove(events + eviction, events + eviction + 1,
            (eventCount - eviction - 1) * sizeof(EventRecord));
    --eventCount;
  }
  const uint32_t timestamp = eventTimestamp();
  const uint32_t eventId = nextEventId;
  nextEventId = eventId == UINT32_MAX ? 0 : eventId + 1;
  events[eventCount] = {eventId, timestamp, durationSeconds, kind, state, 0};
  if (kind == kActivity) {
    DailyRecord* day = ensureDaily(currentDayKey());
    if (day && day->activityCount < 0xFFFF) ++day->activityCount;
  } else if (kind == kAutomatic) {
    DailyRecord* day = ensureDaily(currentDayKey());
    if (day && day->automaticCount < 0xFFFF) ++day->automaticCount;
  }
  if (state == kActive && isFanRuntimeKind(kind)) activeRun = events[eventCount];
  ++eventCount;
  prune();
  findActiveEvent();
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

void initializeDatasetIdentity() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceIdHex, sizeof(deviceIdHex), "%016llx",
           static_cast<unsigned long long>(mac));

  uint8_t epoch[16]{};
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    timezoneOffsetMinutes = preferences.getShort(kTimezoneOffsetKey, 0);
    const size_t storedLength = preferences.getBytesLength(kDataEpochKey);
    if (storedLength != sizeof(epoch) ||
        preferences.getBytes(kDataEpochKey, epoch, sizeof(epoch)) != sizeof(epoch)) {
      esp_fill_random(epoch, sizeof(epoch));
      if (preferences.putBytes(kDataEpochKey, epoch, sizeof(epoch)) != sizeof(epoch)) {
        Serial.println("Statistics: failed to persist data epoch");
        memset(epoch, 0, sizeof(epoch));
      }
    }
    preferences.end();
  } else {
    Serial.println("Statistics: NVS unavailable while initializing data epoch");
    memset(epoch, 0, sizeof(epoch));
  }
  updateDataEpoch(epoch);

  uint8_t bootId[8]{};
  esp_fill_random(bootId, sizeof(bootId));
  for (size_t i = 0; i < sizeof(bootId); ++i) {
    snprintf(bootIdHex + i * 2, 3, "%02x", bootId[i]);
  }
}

}  // namespace

uint16_t statisticsStoreSchemaVersion() { return kSchema; }

void statisticsStoreBegin() {
  initializeDatasetIdentity();
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
    nextEventId = selected.nextEventId;
    activeRun = selected.activeRun;
    droppedThroughEpoch = selected.droppedThroughEpoch;
  } else if (filesystemPreviouslyInitialized) {
    // 两个快照都无法校验时，把统计视为丢失并切换 dataEpoch；
    // App 不会把旧批次缓存或游标混入恢复后的空数据集。
    dailyCount = 0;
    eventCount = 0;
    generation = 0;
    nextEventId = 1;
    Preferences preferences;
    if (!preferences.begin(kPreferencesNamespace, false)) {
      Serial.println("Statistics: failed to rotate data epoch after snapshot loss");
      return;
    }
    const bool epochUpdated = createNewDataEpoch(&preferences);
    preferences.end();
    if (!epochUpdated) {
      Serial.println("Statistics: failed to rotate data epoch after snapshot loss");
      return;
    }
    Serial.println("Statistics: snapshot loss detected; starting an empty dataset");
  }
  // 重启后不能推算断电区间，独立运行记录也停止计时。
  activeRun = {};
  // 未校时事件保留但清除不可恢复的 uptime 时间戳；可信时间戳的运行事件
  // 保留并标记为重启中断，避免断电/重刷后制造虚假的运行时长。
  size_t out = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    if (isUnsyncedTime(events[i].unixSeconds)) events[i].unixSeconds = 0;
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
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  // 日汇总只保存日期键，不保存每条汇总的时区。首次可信校时后固定统计时区，
  // 避免手机时区变化让已有日期键被解释成另一个自然日。
  const bool timezoneAlreadySet = preferences.isKey(kTimezoneOffsetKey);
  const int16_t statisticsTimezone = timezoneAlreadySet
      ? preferences.getShort(kTimezoneOffsetKey, 0)
      : utcOffsetMinutes;
  timezoneOffsetMinutes = statisticsTimezone;
  const size_t timezoneBytes = timezoneAlreadySet
      ? sizeof(int16_t)
      : preferences.putShort(kTimezoneOffsetKey, statisticsTimezone);
  preferences.end();
  if (timezoneBytes != sizeof(int16_t)) {
    Serial.println("Statistics: failed to persist timezone offset");
    return false;
  }
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
    if (isFanRuntimeKind(event.kind) && event.state != kActive) {
      addRuntimeToDaily(event.unixSeconds, event.durationSeconds);
    }
  }
  if (activeRun.id && isUnsyncedTime(activeRun.unixSeconds)) {
    const uint32_t uptime = activeRun.unixSeconds & kUnsyncedTimeMask;
    const uint32_t elapsed = uptimeNow >= uptime ? uptimeNow - uptime
        : uptimeNow + kMillisWrapSeconds - uptime;
    activeRun.unixSeconds = elapsed < unixSeconds && unixSeconds - elapsed >= 1700000000UL
        ? unixSeconds - elapsed : 0;
  }
  prune();
  findActiveEvent();
  persist();
  return true;
}

bool statisticsStoreHasClock() { return clockReady || currentEpoch() != 0; }

const char* statisticsStoreDeviceId() { return deviceIdHex; }
const char* statisticsStoreDataEpoch() { return dataEpochHex; }
const char* statisticsStoreBootId() { return bootIdHex; }

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
  if (!ready || activeRun.id == 0) return;
  activeRun.state = kComplete;
  activeRun.durationSeconds = durationMs / 1000UL;
  if (!isUnsyncedTime(activeRun.unixSeconds)) {
    addRuntimeToDaily(activeRun.unixSeconds, activeRun.durationSeconds);
  }
  for (size_t i = 0; i < eventCount; ++i) {
    if (events[i].id == activeRun.id) events[i] = activeRun;
  }
  activeRun = {};
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
  const uint32_t periodEnd = range == "yesterday" ? todayStart : now + 1;
  uint64_t runtimeSeconds = 0;
  uint32_t activityCount = 0;
  uint32_t automaticCount = 0;
  uint64_t buckets[24]{};
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
        activityCount += daily[i].activityCount;
        automaticCount += daily[i].automaticCount;
        // 小时桶独立保存，明细淘汰不会改变图表与汇总。
        for (uint8_t hour = 0; hour < 24; ++hour) {
          buckets[dayIndex * (24 / bucketSpanHours) + hour / bucketSpanHours] +=
              daily[i].runtimeBuckets[hour];
        }
      }
    }
  }
  if (now && activeRunMs && activeRun.id) {
    const EventRecord& current = activeRun;
    if (isFanRuntimeKind(current.kind) && current.unixSeconds &&
        !isUnsyncedTime(current.unixSeconds)) {
      const uint64_t activeSeconds = activeRunMs / 1000UL;
      addRuntimeToBuckets(current.unixSeconds, activeSeconds,
                          periodStart, periodEnd, bucketSpanHours,
                          bucketCount, buckets);
    }
  }
  for (uint8_t bucket = 0; bucket < bucketCount; ++bucket) runtimeSeconds += buckets[bucket];
  uint32_t eventStart = 0;
  uint32_t eventEnd = 0;
  uint32_t nextCursor = 0;
  if (range == "today" || range == "yesterday") {
    // 今日和昨日都使用本地自然日边界；今日窗口从本地零点到本次响应时间。
    eventStart = range == "today" ? todayStart : periodStart;
    eventEnd = range == "today" ? now + 1 : todayStart;
    for (int i = static_cast<int>(eventCount) - 1; i >= 0; --i) {
      if (!now) break;
      EventRecord event = events[i];
      if (event.state == kActive && static_cast<size_t>(i) == activeEventIndex) event.durationSeconds = activeRunMs / 1000UL;
      if (!eventInRange(event, eventStart, eventEnd)) continue;
      if (eventTotal >= eventOffset && recentCount < 32) {
        if (recentCount++) eventsJson += ',';
        eventsJson += "{\"id\":" + String(event.id) +
                      ",\"time\":" + String(event.unixSeconds) + ",\"kind\":\"" + kindName(event.kind) +
                      "\",\"state\":\"" + stateName(event.state) + "\",\"durationSeconds\":" + String(event.durationSeconds) + "}";
        nextCursor = event.id;
      }
      if (eventTotal < 0xFFFF) ++eventTotal;
    }
  }
  eventsJson += ']';
  String json = "{\"deviceId\":\"" + String(statisticsStoreDeviceId()) +
      "\",\"dataEpoch\":\"" + String(statisticsStoreDataEpoch()) +
      "\",\"bootId\":\"" + String(statisticsStoreBootId()) +
      "\",\"timeSynced\":" + String(statisticsStoreHasClock() ? "true" : "false") +
      ",\"active\":" + String(fanOn ? "true" : "false") +
      ",\"range\":\"" + range + "\",\"runtimeSeconds\":" + String(runtimeSeconds) +
      ",\"activityCount\":" + String(activityCount) + ",\"automaticCount\":" + String(automaticCount) + ",\"bucketSeconds\":[";
  for (uint8_t i = 0; i < bucketCount; ++i) { if (i) json += ','; json += String(buckets[i]); }
  json += "],\"events\":" + eventsJson;
  const bool eventsHasMore = static_cast<uint32_t>(eventOffset) + recentCount < eventTotal;
  json += ",\"statisticsSchema\":" + String(kSchema);
  json += ",\"eventsTruncated\":" + String(droppedThroughEpoch && periodStart <= droppedThroughEpoch ? "true" : "false");
  json += ",\"eventsStart\":" + String(eventStart) +
          ",\"eventsEnd\":" + String(eventEnd) +
          ",\"eventsNextCursor\":" + String(eventsHasMore ? nextCursor : 0);
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
      uint64_t daySeconds = 0;
      const uint8_t bucketsPerDay = 24 / bucketSpanHours;
      for (uint8_t bucket = 0; bucket < bucketsPerDay; ++bucket) {
        daySeconds += buckets[dayIndex * bucketsPerDay + bucket];
      }
      json += "{\"date\":" + String(dayKey) + ",\"runtimeSeconds\":" + String(daySeconds) +
              ",\"activityCount\":" + String(recordIndex < kDailyCapacity ? daily[recordIndex].activityCount : 0) +
              ",\"automaticCount\":" + String(recordIndex < kDailyCapacity ? daily[recordIndex].automaticCount : 0) + "}";
    }
    json += ']';
  }
  json += '}';
  return json;
}

String statisticsStoreBuildEventsJson(uint32_t start, uint32_t end, const String& kind,
                                      uint16_t offset, uint8_t limit,
                                      unsigned long activeRunMs,
                                      uint32_t beforeId) {
  String eventsJson = "[";
  uint16_t total = 0;
  uint8_t count = 0;
  uint16_t eligible = 0;
  uint32_t nextCursor = 0;
  for (int i = static_cast<int>(eventCount) - 1; i >= 0; --i) {
    // OTA 软件重启后 RTC 仍可能有效，与汇总接口使用同一可信时钟判断。
    if (!statisticsStoreHasClock()) break;
    EventRecord event = events[i];
    if (!eventInRange(event, start, end)) continue;
    const String eventKind = kindName(event.kind);
    if (kind != "all" && kind != eventKind) continue;
    if (total < 0xFFFF) ++total;
    if (beforeId && event.id >= beforeId) continue;
    if (event.state == kActive && static_cast<size_t>(i) == activeEventIndex) {
      event.durationSeconds = activeRunMs / 1000UL;
    }
    ++eligible;
    if (eligible > offset && count < limit) {
      if (count++) eventsJson += ',';
      eventsJson += "{\"id\":" + String(event.id) +
                    ",\"time\":" + String(event.unixSeconds) +
                    ",\"kind\":\"" + eventKind +
                    "\",\"state\":\"" + stateName(event.state) +
                    "\",\"durationSeconds\":" + String(event.durationSeconds) + "}";
      nextCursor = event.id;
    }
  }
  eventsJson += ']';
  const uint16_t nextOffset = offset + count;
  const bool hasMore = eligible > offset + count;
  String json = "{\"deviceId\":\"" + String(statisticsStoreDeviceId()) +
                "\",\"dataEpoch\":\"" + String(statisticsStoreDataEpoch()) +
                "\",\"bootId\":\"" + String(statisticsStoreBootId()) +
                "\",\"eventsTruncated\":" + String(droppedThroughEpoch && start <= droppedThroughEpoch ? "true" : "false") +
                ",\"start\":" + String(start) + ",\"end\":" + String(end) +
                ",\"kind\":\"" + kind + "\",\"events\":" + eventsJson +
                ",\"offset\":" + String(offset) +
                ",\"nextOffset\":" + String(nextOffset) +
                ",\"total\":" + String(total) +
                ",\"nextCursor\":" + String(hasMore ? nextCursor : 0) +
                ",\"hasMore\":" + String(hasMore ? "true" : "false") + "}";
  return json;
}
