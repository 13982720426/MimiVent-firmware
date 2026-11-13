// BLE/网页共用的统计存储。采用 SPIFFS 双槽快照，避免频繁写 NVS 并保留 OTA 分区。
#include "StatisticsStore.h"

#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <time.h>
#include <sys/time.h>

#include "StatisticsConfig.h"
#include "StatisticsTypes.h"
#include "StatisticsActivityCore.h"
#include "FirmwareVersion.h"
#include "SettingsStore.h"

namespace {

constexpr uint32_t kMagic = 0x4D565354;  // MVST
constexpr uint16_t kSchema = FIRMWARE_STATISTICS_SCHEMA;   // 5，落盘格式
constexpr uint16_t kSchemaV4 = 4;                          // 只读，迁移用
constexpr uint16_t kSchemaV6 = 6;                          // 只读，迁移用（512 条事件）
constexpr uint16_t kSchemaV7 = 7;                          // 只读，迁移用（312 B 单日记录）
constexpr uint16_t kSchemaV8 = 8;                          // 只读，迁移用（收窄逐小时数组）
constexpr size_t kEventCapacityV6 = 512;                   // schema 6 的事件上限
constexpr size_t kDailyCapacity = StatisticsConfig::kSummaryRetentionDays;
constexpr size_t kEventCapacity = StatisticsConfig::kEventCapacity;
constexpr uint32_t kSecondsPerDay = 86400UL;
constexpr uint32_t kRetentionSeconds =
    static_cast<uint32_t>(StatisticsConfig::kDetailRetentionDays) * kSecondsPerDay;
constexpr uint32_t kDailyRetentionSeconds =
    static_cast<uint32_t>(StatisticsConfig::kSummaryRetentionDays) * kSecondsPerDay;
// ESP32-C3 无后备电池时钟。高位表示低位保存本次开机 millis 秒数。
// uptime 秒的 32 位回绕周期（约 49.7 天），校时回填时用来修正经过时间。
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

// ---------------------------------------------------------------------------
// schema 4 落盘布局：只读，仅用于迁移。布局一旦发布不得修改。
// ---------------------------------------------------------------------------
struct DailyRecordV4 {
  uint32_t dayKey;
  uint16_t activityCount;
  uint16_t automaticCount;
  uint32_t runtimeBuckets[24];
};

struct EventRecordV4 {
  uint32_t id;
  uint32_t unixSeconds;
  uint32_t durationSeconds;
  uint8_t kind;
  uint8_t state;
  uint16_t reserved;
};

struct StoreHeaderV4 {
  uint32_t magic;
  uint16_t schema;
  uint16_t dailyCount;
  uint16_t eventCount;
  uint16_t reserved;
  uint32_t generation;
  uint32_t checksum;
  uint32_t nextEventId;
  EventRecordV4 activeRun;
  uint32_t droppedThroughEpoch;
};

// ---------------------------------------------------------------------------
// schema 5 落盘布局：规范的内存与磁盘结构。
// ---------------------------------------------------------------------------
struct DailyRecordV5 {
  uint32_t dayKey;
  uint16_t activityCount;
  uint16_t activitySegmentCount;
  uint16_t automaticCount;

  uint32_t runtimeBuckets[24];
  uint32_t activitySpanBuckets[24];
  uint16_t activityTriggerBuckets[24];
  uint16_t activitySegmentBuckets[24];

  uint32_t activitySpanSeconds;
  uint32_t toiletDurationSeconds;
  uint16_t toiletSampleCount;

  uint8_t metricCoverage;
  uint8_t overflowFlags;
};

// schema 8：两个逐小时数组由 uint32_t 收窄为 uint16_t。
// 语义上单个小时桶最多 3600 秒，uint16 上限 65535 足够；
// 写入侧对异常值做饱和处理并把饱和记为 overflow，避免静默回绕。
// 布局变化使单日记录从 312 B 降到 216 B，30 天共省 2,880 B。
struct DailyRecordV6 {
  uint32_t dayKey;
  uint16_t activityCount;
  uint16_t activitySegmentCount;
  uint16_t automaticCount;

  uint16_t runtimeBuckets[24];
  uint16_t activitySpanBuckets[24];
  uint16_t activityTriggerBuckets[24];
  uint16_t activitySegmentBuckets[24];

  uint32_t activitySpanSeconds;
  uint32_t toiletDurationSeconds;
  uint16_t toiletSampleCount;

  uint8_t metricCoverage;
  uint8_t overflowFlags;
};

struct EventRecordV5 {
  uint32_t id;
  uint32_t unixSeconds;
  uint32_t durationSeconds;
  uint8_t kind;
  uint8_t state;
  uint8_t endReason;
  uint8_t flags;          // bit0 = timeUnsynced, bit2 = clockWentBackwards
  uint16_t reserved;
  uint16_t pad;
};

struct StoreHeaderV5 {
  uint32_t magic;
  uint16_t schema;
  uint16_t dailyCount;
  uint16_t eventCount;
  uint16_t flags;         // bit0 = migrationInProgress, bit2 = clockWentBackwards
  uint32_t generation;
  uint32_t checksum;
  uint32_t nextEventId;
  EventRecordV5 activeRun;
  uint32_t droppedThrough;   // 截至该时刻（含）的事件均已丢弃
  // 迁移完成时最大的 dayKey。该日及更早的日期缺少跨度/段数/入厕桶，
  // 后续写入不得把这些覆盖位从 0 提升为 1（否则"部分覆盖"会被当成完整）。
  uint32_t migratedThroughDayKey;
  int16_t tzOffsetMinutes;
  uint16_t tzVersion;
};

static_assert(sizeof(DailyRecordV4) == 104, "schema 4 布局已发布，不得修改");
static_assert(sizeof(EventRecordV4) == 16, "schema 4 布局已发布，不得修改");
static_assert(sizeof(StoreHeaderV4) == 44, "schema 4 布局已发布，不得修改");
static_assert(sizeof(DailyRecordV5) == 312, "布局变化必须同步提升 storage schema");
static_assert(sizeof(DailyRecordV6) == 216, "schema 8 布局已发布，不得修改");
static_assert(sizeof(EventRecordV5) == 20, "布局变化必须同步提升 storage schema");
static_assert(sizeof(StoreHeaderV5) == 56, "布局变化必须同步提升 storage schema");

// 规范结构：内存与磁盘一律使用 V5。
using DailyRecord = DailyRecordV6;
using EventRecord = EventRecordV5;
using StoreHeader = StoreHeaderV5;

// 头部标志位（与事件 flags 是不同字段，位编号各自独立）。
constexpr uint16_t kHeaderFlagMigrationInProgress = 1u << 0;
constexpr uint16_t kHeaderFlagClockWentBackwards = 1u << 2;

// schema 4 与 5 的头部前 24 字节布局完全相同，探测时先读这段公共前缀。
struct HeaderPrefix {
  uint32_t magic;
  uint16_t schema;
  uint16_t dailyCount;
  uint16_t eventCount;
  uint16_t reservedOrFlags;
  uint32_t generation;
  uint32_t checksum;
  uint32_t nextEventId;
};
static_assert(sizeof(HeaderPrefix) == 24, "头部公共前缀布局");

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
bool timezoneInitialized = false;
int16_t timezoneOffsetMinutes = 0;
size_t activeEventIndex = kEventCapacity;
EventRecord activeRun{};
uint32_t droppedThrough = 0;
// 迁移完成时的最大 dayKey：该日及更早的日期不得被提升为"跨度/段数/入厕完整"。
uint32_t migratedThroughDayKey = 0;
// 需要跨重启保留的头部标志（迁移中间态、时钟回拨等）。
uint16_t persistentFlags = 0;
uint16_t tzVersion = 1;
bool filesystemPreviouslyInitialized = false;
// 同一连续活动段只占一个事件槽；阈值在首个触发时锁定，重启后不继续合并。
uint32_t activeActivityId = 0;
// 活动跨度与合并间隔使用本次启动的单调毫秒，UTC 只映射事件和日历桶。
uint32_t activeActivityStartedAtMs = 0;
// 最近一次活动段的真实跨度（首末触发之差）。入厕时长用它而不是排风时长：
// 自动排风是"人走后再吹 3 分钟"，衡量的是设备行为，不是人在里面的时间。
uint32_t lastActivitySpanSeconds = 0;
// 本次自动排风开始时所对应的活动跨度快照。
// 排风期间 PIR 再触发会开新段并把 lastActivitySpanSeconds 清零，
// 结束时再读全局值就会拿到 0，白白漏掉这次入厕样本。
uint32_t pendingToiletSpanSeconds = 0;
// 活动段的开始时刻（秒）。入厕样本按它归属日期：跨午夜的活动整段算前一天，
// 与 addActivitySpanToDaily 的归属规则保持一致。
uint32_t lastActivityStartEpoch = 0;
uint32_t pendingToiletStartEpoch = 0;
uint16_t activeActivityGapSeconds = 0;
uint32_t lastActivityAtMs = 0;
uint32_t lastClockSyncAtMs = 0;
StatisticsClockSource lastClockSyncSource = StatisticsClockSource::kPhone;
bool hasClockSyncSource = false;

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

// 未校时状态只由事件 flags.bit0 表达，禁止借用 unixSeconds 高位：
// 真实 Unix 时间进入 2038 年后，高位为 1 的时间戳会被误判成未校时。
bool eventIsUnsynced(const EventRecord& event) {
  return (event.flags & kEventFlagTimeUnsynced) != 0;
}

struct EventTimestamp {
  uint32_t seconds = 0;  // 可信时是 Unix 秒；未校时是本次开机 uptime 秒
  bool unsynced = false;
  bool clockWentBackwards = false;
};

EventTimestamp eventTimestamp() {
  const uint32_t epoch = currentEpoch();
  if (epoch) {
    bool wentBackwards = false;
    if (eventCount != 0) {
      const EventRecord& previous = events[eventCount - 1];
      if (previous.unixSeconds && !eventIsUnsynced(previous)) {
        const uint64_t previousEnd = static_cast<uint64_t>(previous.unixSeconds) +
            ((previous.state == kActive) ? 0 : previous.durationSeconds);
        wentBackwards = epoch < previousEnd;
      }
    }
    return EventTimestamp{epoch, false, wentBackwards};
  }
  return EventTimestamp{static_cast<uint32_t>(millis() / 1000UL), true, false};
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
DailyRecord* ensureDailyTrusted(uint32_t dayKey);

// 本地小时与本地日零点（秒）。日期归属一律走这两个函数，避免各处手写偏移换算。
uint32_t localHourOfEpoch(uint32_t epoch) {
  const int64_t local = static_cast<int64_t>(epoch) +
                        static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  return static_cast<uint32_t>((local % kSecondsPerDay) / 3600);
}

uint32_t localDayStartOfEpoch(uint32_t epoch) {
  const int64_t local = static_cast<int64_t>(epoch) +
                        static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  const int64_t dayIndex = local / kSecondsPerDay;
  return static_cast<uint32_t>(dayIndex * kSecondsPerDay -
                               static_cast<int32_t>(timezoneOffsetMinutes) * 60);
}
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
    DailyRecord* day = ensureDailyTrusted(dayKey);
    if (day) {
      const uint8_t hour = (localEpoch % kSecondsPerDay) / 3600;
      const uint64_t total = static_cast<uint64_t>(day->runtimeBuckets[hour]) + sliceSeconds;
      if (total > UINT32_MAX) {
        day->runtimeBuckets[hour] = UINT32_MAX;
        day->overflowFlags |= kMetricRuntime;
        day->metricCoverage &= static_cast<uint8_t>(~kMetricRuntime);
      } else {
        day->runtimeBuckets[hour] = static_cast<uint32_t>(total);
        day->metricCoverage |= kMetricRuntime;
      }
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

void addActivityToBuckets(uint32_t periodStart, uint32_t periodEnd,
                         uint8_t bucketSpanHours, uint8_t bucketCount,
                         uint64_t* buckets, uint32_t* activityCounts) {
  const int32_t offsetSeconds = static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  const int64_t localPeriodDay =
      (static_cast<int64_t>(periodStart) + offsetSeconds) / kSecondsPerDay;
  const uint32_t bucketsPerDay = 24 / bucketSpanHours;
  for (size_t i = 0; i < eventCount;) {
    const EventRecord& first = events[i];
    if (first.kind != kActivity || !first.unixSeconds || eventIsUnsynced(first)) {
      ++i;
      continue;
    }

    // 只有带压缩标记的新版活动段用于连续感应时长；旧记录缺少历史阈值，不回算新指标。
    if (first.reserved > 1 || first.durationSeconds > 0) {
      addRuntimeToBuckets(first.unixSeconds, first.durationSeconds, periodStart,
                          periodEnd, bucketSpanHours, bucketCount, buckets);
    }
    // 单次触发的真实跨度为 0 秒，单独计入事件桶，让图表仍能标出触发时段。
    if (activityCounts && first.unixSeconds >= periodStart &&
        first.unixSeconds < periodEnd) {
      const int64_t localEpoch =
          static_cast<int64_t>(first.unixSeconds) + offsetSeconds;
      const int64_t dayIndex = localEpoch / kSecondsPerDay - localPeriodDay;
      const uint8_t hour = static_cast<uint8_t>(
          (localEpoch % kSecondsPerDay) / 3600);
      const int64_t bucketIndex =
          dayIndex * bucketsPerDay + hour / bucketSpanHours;
      if (bucketIndex >= 0 && bucketIndex < bucketCount &&
          activityCounts[bucketIndex] < UINT32_MAX) {
        ++activityCounts[bucketIndex];
      }
    }
    ++i;
  }
}

size_t findDaily(uint32_t dayKey) {
  for (size_t i = 0; i < dailyCount; ++i) {
    if (daily[i].dayKey == dayKey) return i;
  }
  return kDailyCapacity;
}

// 可信时钟下新建的日记录：所有指标从零开始就"已知"，覆盖位全置。
// 迁移与校时回填不走这里——那些日期的指标确实未知，必须保持不可用。
DailyRecord* ensureDailyTrusted(uint32_t dayKey) {
  const size_t index = findDaily(dayKey);
  if (index < kDailyCapacity) return &daily[index];
  DailyRecord* day = ensureDaily(dayKey);
  // 迁移覆盖日即使新建也不能标成完整：该日升级前的时段没有跨度/段数/入厕桶。
  if (day) day->metricCoverage = (dayKey <= migratedThroughDayKey) ? 0x03 : 0xFF;
  return day;
}

// 迁移日（含）不能被提升为"跨度/段数/入厕完整"：那天升级前的时段没有对应桶。
bool dayHasPreMigrationGap(uint32_t dayKey) {
  return migratedThroughDayKey != 0 && dayKey <= migratedThroughDayKey;
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

// 记一次触发：触发数归触发所在日/小时，段数归段起点日/小时。
// newSegment 为真表示这次触发开启了新段。
void addActivityTriggerToDaily(uint32_t epoch, bool newSegment) {
  DailyRecord* day = ensureDailyTrusted(dayKeyForEpoch(epoch));
  if (!day) return;
  const uint32_t hour = localHourOfEpoch(epoch);
  if (day->activityCount < UINT16_MAX) {
    ++day->activityCount;
    day->metricCoverage |= kMetricActivityCount;
  } else {
    day->overflowFlags |= kMetricActivityCount;
    day->metricCoverage &= static_cast<uint8_t>(~kMetricActivityCount);
  }
  if (hour < 24) {
    if (day->activityTriggerBuckets[hour] < UINT16_MAX) {
      ++day->activityTriggerBuckets[hour];
      day->metricCoverage |= kMetricActivityTriggerBuckets;
    } else {
      day->overflowFlags |= kMetricActivityTriggerBuckets;
      day->metricCoverage &= static_cast<uint8_t>(~kMetricActivityTriggerBuckets);
    }
  }
  if (!newSegment) return;
  // 迁移日以下：升级前的时段没有跨度/段数桶，整天保持不可用，不得提升覆盖位。
  const bool promotable = !dayHasPreMigrationGap(day->dayKey);
  // 新段的跨度从 0 开始，"零跨度"是确定的事实而不是未知值，
  // 因此这里就把跨度覆盖位置起来；否则单次触发当天会被读成"跨度不可用"。
  if (promotable) {
    day->metricCoverage |= static_cast<uint8_t>(kMetricActivitySpanSeconds | kMetricActivitySpanBuckets);
  }
  if (day->activitySegmentCount < UINT16_MAX) {
    ++day->activitySegmentCount;
    if (promotable) day->metricCoverage |= kMetricActivitySegmentCount;
  } else {
    day->overflowFlags |= kMetricActivitySegmentCount;
    day->metricCoverage &= static_cast<uint8_t>(~kMetricActivitySegmentCount);
  }
  if (hour < 24) {
    if (day->activitySegmentBuckets[hour] < UINT16_MAX) {
      ++day->activitySegmentBuckets[hour];
      if (promotable) day->metricCoverage |= kMetricActivitySegmentBuckets;
    } else {
      day->overflowFlags |= kMetricActivitySegmentBuckets;
      day->metricCoverage &= static_cast<uint8_t>(~kMetricActivitySegmentBuckets);
    }
  }
}

// 把 [from, to) 这段已确认的活动跨度按本地日/小时切开累加。
// 只累计增量，绝不重算整段，避免延长时重复入账。
void addActivitySpanToDaily(uint32_t from, uint32_t to) {
  if (from >= to) return;
  // 整段归入**开始日**：23:59 进入卫生间、00:01 再次触发，整段算前一天。
  // 按日切分会让日汇总的跨度与入厕样本落在不同日期，两个数字互相打架。
  const uint32_t dayStart = localDayStartOfEpoch(from);
  DailyRecord* day = ensureDailyTrusted(dayKeyForEpoch(from));
  if (!day) return;
  const uint64_t dayEnd = static_cast<uint64_t>(dayStart) + kSecondsPerDay;
  const uint32_t sliceEnd =
      to < dayEnd ? to : static_cast<uint32_t>(dayEnd);
  StatisticsActivitySegment slice{};
  slice.first = from;
  slice.last = sliceEnd;
  slice.active = true;
  statisticsActivityAccumulateDay(slice, dayStart, day->activitySpanBuckets, 24);
  // 跨午夜的部分并入当日最后一小时，保证桶和与总跨度一致。
  const uint32_t total = to - from;
  const uint32_t inDay = sliceEnd > from ? sliceEnd - from : 0;
  if (inDay < total) {
    day->activitySpanBuckets[23] =
        static_cast<uint32_t>(day->activitySpanBuckets[23]) + (total - inDay);
  }
  const bool promotable = !dayHasPreMigrationGap(day->dayKey);
  const uint64_t sum = static_cast<uint64_t>(day->activitySpanSeconds) + total;
  if (sum > UINT32_MAX) {
    day->activitySpanSeconds = UINT32_MAX;
    day->overflowFlags |= kMetricActivitySpanSeconds;
    day->metricCoverage &= static_cast<uint8_t>(~kMetricActivitySpanSeconds);
  } else {
    day->activitySpanSeconds = static_cast<uint32_t>(sum);
    if (promotable) day->metricCoverage |= kMetricActivitySpanSeconds;
  }
  if (promotable) day->metricCoverage |= kMetricActivitySpanBuckets;
}

// schema 5 的校验和：连续 CRC32 覆盖「header(checksum=0) + daily 块 + event 块」。
// schema 4 沿用旧的「分块 CRC 再异或」方案，只用于校验既有文件。
struct Crc32 {
  uint32_t state = 0xFFFFFFFFUL;
  void update(const uint8_t* bytes, size_t length) {
    for (size_t i = 0; i < length; ++i) {
      state ^= bytes[i];
      for (uint8_t bit = 0; bit < 8; ++bit) {
        state = (state >> 1) ^ (0xEDB88320UL & (0UL - (state & 1UL)));
      }
    }
  }
  uint32_t finish() const { return ~state; }
};

const char* slotPathForGeneration(uint32_t value) {
  return (value & 1U) ? kSlotA : kSlotB;
}

// 只读头部公共前缀：schema 4 与 5 的前 24 字节布局一致。
bool probeSlot(const char* path, HeaderPrefix* prefix) {
  if (!path) return false;
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  HeaderPrefix local{};
  const bool ok = file.size() >= sizeof(StoreHeaderV4) &&
      file.read(reinterpret_cast<uint8_t*>(&local), sizeof(local)) == sizeof(local) &&
      local.magic == kMagic &&
      (local.schema == kSchema || local.schema == kSchemaV7 ||
       local.schema == kSchemaV6 || local.schema == kSchemaV4) &&
      local.dailyCount <= kDailyCapacity &&
      // schema 6 允许多达 512 条事件，读取时会截断到新容量。
      local.eventCount <= (local.schema == kSchemaV6 ? kEventCapacityV6
                                                     : kEventCapacity);
  file.close();
  if (ok && prefix) *prefix = local;
  return ok;
}

// schema 4 → 5 的转换：无法还原的新指标一律填 0 并清覆盖位，不伪造。
// schema 4 → 当前布局。运行时桶在旧格式里是 uint32_t，收窄时饱和处理。
void convertDailyV4(const DailyRecordV4& source, DailyRecordV6* target) {
  *target = {};
  target->dayKey = source.dayKey;
  target->activityCount = source.activityCount;
  target->automaticCount = source.automaticCount;
  for (size_t hour = 0; hour < 24; ++hour) {
    const uint32_t value = source.runtimeBuckets[hour];
    target->runtimeBuckets[hour] =
        value > UINT16_MAX ? static_cast<uint16_t>(UINT16_MAX)
                           : static_cast<uint16_t>(value);
  }
  target->metricCoverage = kMetricRuntime | kMetricActivityCount;
}

void convertEventV4(const EventRecordV4& source, EventRecordV5* target) {
  *target = {};
  target->id = source.id;
  // schema 4 把未校时标志编在 unixSeconds 高位，迁移时剥离并转成事件 flags.bit0。
  if ((source.unixSeconds & 0x80000000UL) != 0) {
    target->unixSeconds = source.unixSeconds & 0x7FFFFFFFUL;
    target->flags = kEventFlagTimeUnsynced;
  } else {
    target->unixSeconds = source.unixSeconds;
    target->flags = 0;
  }
  target->durationSeconds = source.durationSeconds;
  target->kind = source.kind;
  target->state = source.state;
  // 旧记录没有结束原因：一律"未知"，不得填 COMPLETED。
  target->endReason = static_cast<uint8_t>(StatisticsEndReason::LEGACY_UNKNOWN);
  target->reserved = source.reserved;
}

// schema 7 → 8：两个逐小时数组由 uint32_t 收窄为 uint16_t。
// 单小时最多 3600 秒，正常数据不会超限；异常计时（时钟跳变等）超限时
// 饱和到 65535 并置 overflowFlags 的对应位，避免静默回绕成小值。
void convertDailyV5ToV6(const DailyRecordV5& source, DailyRecordV6* target) {
  if (!target) return;
  memset(target, 0, sizeof(*target));
  target->dayKey = source.dayKey;
  target->activityCount = source.activityCount;
  target->activitySegmentCount = source.activitySegmentCount;
  target->automaticCount = source.automaticCount;
  auto saturate = [](uint32_t value) -> uint16_t {
    return value > UINT16_MAX ? static_cast<uint16_t>(UINT16_MAX)
                              : static_cast<uint16_t>(value);
  };
  bool overflowed = false;
  for (size_t i = 0; i < 24; ++i) {
    if (source.runtimeBuckets[i] > UINT16_MAX) overflowed = true;
    if (source.activitySpanBuckets[i] > UINT16_MAX) overflowed = true;
    target->runtimeBuckets[i] = saturate(source.runtimeBuckets[i]);
    target->activitySpanBuckets[i] = saturate(source.activitySpanBuckets[i]);
    target->activityTriggerBuckets[i] = source.activityTriggerBuckets[i];
    target->activitySegmentBuckets[i] = source.activitySegmentBuckets[i];
  }
  target->activitySpanSeconds = source.activitySpanSeconds;
  target->toiletDurationSeconds = source.toiletDurationSeconds;
  target->toiletSampleCount = source.toiletSampleCount;
  target->metricCoverage = source.metricCoverage;
  target->overflowFlags = source.overflowFlags;
  (void)overflowed;
}

// 读取 schema 5 槽位：直接把记录读入规范内存数组，成功后回填 header。
bool readSlotV5(const char* path, StoreHeader* header, bool acceptSchemaV6) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  StoreHeader local{};
  const bool legacyLayout = false;
  if (file.size() < sizeof(local) ||
      file.read(reinterpret_cast<uint8_t*>(&local), sizeof(local)) != sizeof(local) ||
      local.magic != kMagic ||
      (local.schema != kSchema &&
       !(acceptSchemaV6 &&
         (local.schema == kSchemaV6 || local.schema == kSchemaV7))) ||
      local.dailyCount > kDailyCapacity) {
    file.close();
    return false;
  }
  // schema 6 扩容读取后保留最新记录；丢弃的头部仍参与 CRC。
  const size_t eventLimit =
      local.schema == kSchemaV6 ? kEventCapacityV6 : kEventCapacity;
  if (local.eventCount > eventLimit) {
    file.close();
    return false;
  }
  // schema 8 起单日记录收窄到 216 B；更早的槽位按 312 B 存放。
  // CRC 必须对文件里的原始字节计算，所以旧记录逐条读入局部变量，
  // 既更新 CRC 又转换到内存态，避免为整块旧数据再开一份缓冲。
  const bool compactDaily = local.schema >= kSchemaV8;
  const size_t recordBytes =
      compactDaily ? sizeof(DailyRecordV6) : sizeof(DailyRecordV5);
  const size_t dailyBytes = local.dailyCount * recordBytes;
  const size_t totalEvents = local.eventCount;
  const size_t keepEvents =
      totalEvents < kEventCapacity ? totalEvents : kEventCapacity;
  const size_t totalEventBytes = totalEvents * sizeof(EventRecord);
  const uint32_t expected = local.checksum;
  if (file.size() != sizeof(local) + dailyBytes + totalEventBytes) {
    file.close();
    return false;
  }
  local.checksum = 0;
  Crc32 crc;
  crc.update(reinterpret_cast<uint8_t*>(&local), sizeof(local));
  for (size_t i = 0; i < local.dailyCount; ++i) {
    if (compactDaily) {
      DailyRecordV6 record{};
      if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) !=
          sizeof(record)) {
        file.close();
        return false;
      }
      crc.update(reinterpret_cast<uint8_t*>(&record), sizeof(record));
      daily[i] = record;
    } else {
      DailyRecordV5 record{};
      if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) !=
          sizeof(record)) {
        file.close();
        return false;
      }
      crc.update(reinterpret_cast<uint8_t*>(&record), sizeof(record));
      convertDailyV5ToV6(record, &daily[i]);
    }
  }
  uint32_t migratedDroppedThrough = local.droppedThrough;
  for (size_t i = 0; i < totalEvents; ++i) {
    EventRecord record{};
    if (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) != sizeof(record)) {
      file.close();
      return false;
    }
    crc.update(reinterpret_cast<uint8_t*>(&record), sizeof(record));
    if (i < totalEvents - keepEvents) {
      if (record.unixSeconds && !eventIsUnsynced(record)) {
        migratedDroppedThrough = max(migratedDroppedThrough, record.unixSeconds);
      }
    } else {
      events[i - (totalEvents - keepEvents)] = record;
    }
  }
  file.close();
  local.checksum = expected;
  if (crc.finish() != expected) return false;
  local.droppedThrough = migratedDroppedThrough;
  (void)legacyLayout;
  if (header) *header = local;
  return true;
}

// 读取 schema 4 槽位：旧方案校验通过后转换成 V5，再回填 header。
bool readSlotV4(const char* path, StoreHeader* header) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  StoreHeaderV4 legacy{};
  const size_t size = file.size();
  if (size < sizeof(legacy) ||
      file.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy)) != sizeof(legacy) ||
      legacy.magic != kMagic || legacy.schema != kSchemaV4 ||
      legacy.dailyCount > kDailyCapacity || legacy.eventCount > kEventCapacity) {
    file.close();
    return false;
  }
  const size_t dailyBytes = legacy.dailyCount * sizeof(DailyRecordV4);
  const size_t eventBytes = legacy.eventCount * sizeof(EventRecordV4);
  if (size != sizeof(legacy) + dailyBytes + eventBytes) {
    file.close();
    return false;
  }
  // 旧数据最多 11 KB，用一次堆缓冲迁移，避免长期占用静态 RAM。
  uint8_t* raw = static_cast<uint8_t*>(malloc(dailyBytes + eventBytes + 1));
  if (!raw) {
    file.close();
    return false;
  }
  const bool read = file.read(raw, dailyBytes) == dailyBytes &&
                    file.read(raw + dailyBytes, eventBytes) == eventBytes;
  file.close();
  bool valid = false;
  if (read) {
    const uint32_t expected = legacy.checksum;
    legacy.checksum = 0;
    uint32_t crc = checksumBytes(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy));
    crc = checksumBytes(raw, dailyBytes) ^ crc;
    crc = checksumBytes(raw + dailyBytes, eventBytes) ^ crc;
    legacy.checksum = expected;
    valid = (crc == expected);
  }
  if (valid) {
    for (size_t i = 0; i < legacy.dailyCount; ++i) {
      DailyRecordV4 source{};
      memcpy(&source, raw + i * sizeof(DailyRecordV4), sizeof(source));
      convertDailyV4(source, &daily[i]);
    }
    for (size_t i = 0; i < legacy.eventCount; ++i) {
      EventRecordV4 source{};
      memcpy(&source, raw + dailyBytes + i * sizeof(EventRecordV4), sizeof(source));
      convertEventV4(source, &events[i]);
    }
    if (header) {
      *header = {};
      header->magic = kMagic;
      header->schema = kSchema;
      header->dailyCount = legacy.dailyCount;
      header->eventCount = legacy.eventCount;
      header->flags = 0;
      header->generation = legacy.generation;
      header->checksum = legacy.checksum;
      header->nextEventId = legacy.nextEventId;
      convertEventV4(legacy.activeRun, &header->activeRun);
      header->droppedThrough = legacy.droppedThroughEpoch;
      // 迁移日之后才能把跨度/段数/入厕视为完整。
      for (size_t i = 0; i < legacy.dailyCount; ++i) {
        DailyRecordV4 source{};
        memcpy(&source, raw + i * sizeof(DailyRecordV4), sizeof(source));
        if (source.dayKey > header->migratedThroughDayKey) {
          header->migratedThroughDayKey = source.dayKey;
        }
      }
      header->tzOffsetMinutes = timezoneOffsetMinutes;
      header->tzVersion = 1;
    }
  }
  free(raw);
  return valid;
}

// 统一入口：读任意 schema 的有效槽位（CRC 通过）。schema 4 会被转换成 V5。
bool loadSlot(const char* path, StoreHeader* header) {
  HeaderPrefix prefix{};
  if (!probeSlot(path, &prefix)) return false;
  if (prefix.schema == kSchemaV4) return readSlotV4(path, header);
  return readSlotV5(path, header, /*acceptSchemaV6=*/true);
}

// 组装一份 V5 快照头部（checksum 稍后补）。
StoreHeader buildSnapshotHeader(uint32_t value, uint16_t extraFlags) {
  StoreHeader header{};
  header.magic = kMagic;
  header.schema = kSchema;
  header.dailyCount = static_cast<uint16_t>(dailyCount);
  header.eventCount = static_cast<uint16_t>(eventCount);
  header.flags = static_cast<uint16_t>(persistentFlags | extraFlags);
  header.generation = value;
  header.checksum = 0;
  header.nextEventId = nextEventId;
  header.activeRun = activeRun;
  header.droppedThrough = droppedThrough;
  header.migratedThroughDayKey = migratedThroughDayKey;
  header.tzOffsetMinutes = timezoneOffsetMinutes;
  header.tzVersion = tzVersion;
  return header;
}

uint32_t snapshotChecksum(const StoreHeader& source) {
  StoreHeader header = source;
  header.checksum = 0;
  Crc32 crc;
  crc.update(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  crc.update(reinterpret_cast<const uint8_t*>(daily), dailyCount * sizeof(DailyRecord));
  crc.update(reinterpret_cast<const uint8_t*>(events), eventCount * sizeof(EventRecord));
  return crc.finish();
}

// 把当前内存数据集写入指定槽位；extraFlags 用于迁移中间态。
bool writeSnapshotToSlot(const char* path, uint32_t value, uint16_t extraFlags) {
  StoreHeader header = buildSnapshotHeader(value, extraFlags);
  header.checksum = snapshotChecksum(header);
  File file = SPIFFS.open(path, FILE_WRITE);
  if (!file) return false;
  const size_t dailyBytes = dailyCount * sizeof(DailyRecord);
  const size_t eventBytes = eventCount * sizeof(EventRecord);
  const bool written =
      file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
      file.write(reinterpret_cast<const uint8_t*>(daily), dailyBytes) == dailyBytes &&
      file.write(reinterpret_cast<const uint8_t*>(events), eventBytes) == eventBytes;
  file.flush();
  file.close();
  return written;
}

// 读回校验：只验头部、长度与 CRC，不改动内存数据集。
bool verifySnapshotInSlot(const char* path, uint32_t expectedGeneration) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) return false;
  StoreHeader header{};
  if (file.size() < sizeof(header) ||
      file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
      header.magic != kMagic || header.schema != kSchema ||
      header.generation != expectedGeneration) {
    file.close();
    return false;
  }
  const size_t dailyBytes = header.dailyCount * sizeof(DailyRecord);
  const size_t eventBytes = header.eventCount * sizeof(EventRecord);
  const uint32_t expected = header.checksum;
  if (file.size() != sizeof(header) + dailyBytes + eventBytes) {
    file.close();
    return false;
  }
  Crc32 crc;
  header.checksum = 0;
  crc.update(reinterpret_cast<const uint8_t*>(&header), sizeof(header));
  uint8_t chunk[128];
  for (size_t remaining = dailyBytes + eventBytes; remaining > 0;) {
    const size_t take = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (file.read(chunk, take) != take) {
      file.close();
      return false;
    }
    crc.update(chunk, take);
    remaining -= take;
  }
  file.close();
  return crc.finish() == expected;
}

bool persist() {
  if (!ready) return false;
  const uint32_t value = generation + 1;
  if (!writeSnapshotToSlot(slotPathForGeneration(value), value, 0)) return false;
  generation = value;
  return true;
}

bool persistDirty = false;
uint32_t persistDirtySinceMs = 0;

// 活动段延长只置脏标记，合并到最小间隔再落盘：快照约 19.7 KB，
// 每次 PIR 触发都全量重写会拖慢 PIR 处理并放大 Flash 磨损。
void persistDeferred() {
  const uint32_t nowMs = millis();
  if (!persistDirty) {
    persistDirty = true;
    persistDirtySinceMs = nowMs;
  }
  if (nowMs - persistDirtySinceMs >= StatisticsConfig::kPersistMinIntervalMs) {
    if (persist()) persistDirty = false;
  }
}

void persistIfDirty() {
  if (!persistDirty) return;
  if (millis() - persistDirtySinceMs < StatisticsConfig::kPersistMinIntervalMs) return;
  if (persist()) persistDirty = false;
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
    } else if (events[i].unixSeconds && !eventIsUnsynced(events[i])) {
      // 未校时事件不推进 droppedThrough：它的时间戳只是 uptime 相对量，
      // 直接参与比较会把全部历史日期误判为容量受限。
      droppedThrough = max(droppedThrough, events[i].unixSeconds);
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
    if (isFanRuntimeKind(events[i - 1].kind) && events[i - 1].state == kActive) {
      activeEventIndex = i - 1;
      return;
    }
  }
}

bool prepareEventSlot(uint8_t kind) {
  (void)kind;
  if (eventCount < kEventCapacity) return true;
  // 容量淘汰只从最老的事件（数组头部）开始：缺口只可能出现在头部，
  // 单个 droppedThrough 才能精确判定任意一天的详情完整性。
  const EventRecord victim = events[0];
  if (victim.unixSeconds && !eventIsUnsynced(victim)) {
    droppedThrough = max(droppedThrough, victim.unixSeconds);
  }
  if (victim.id == activeActivityId) {
    activeActivityId = 0;
    activeActivityGapSeconds = 0;
    activeActivityStartedAtMs = 0;
  }
  memmove(events, events + 1, (eventCount - 1) * sizeof(EventRecord));
  --eventCount;
  return true;
}

void addEvent(uint8_t kind, uint8_t state, uint32_t durationSeconds = 0) {
  // ID 作为分页游标必须单调且不可复用；耗尽时停止写入，等待显式重置数据批次。
  if (nextEventId == 0) {
    Serial.println("Statistics: event ID exhausted; refusing to reuse IDs");
    return;
  }
  if (!prepareEventSlot(kind)) return;
  if (kind != kActivity) {
    activeActivityId = 0;
    activeActivityGapSeconds = 0;
    activeActivityStartedAtMs = 0;
  }
  const EventTimestamp timestamp = eventTimestamp();
  const uint32_t eventId = nextEventId;
  nextEventId = eventId == UINT32_MAX ? 0 : eventId + 1;
  EventRecord& record = events[eventCount];
  record = {};
  record.id = eventId;
  record.unixSeconds = timestamp.seconds;
  record.durationSeconds = durationSeconds;
  record.kind = kind;
  record.state = state;
  // 新记录的结束原因未知；风机结束时由 statisticsStoreFanStopped 改写。
  record.endReason = static_cast<uint8_t>(StatisticsEndReason::LEGACY_UNKNOWN);
  record.flags = timestamp.unsynced ? kEventFlagTimeUnsynced :
      (timestamp.clockWentBackwards ? kEventFlagClockWentBackwards : 0);
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
  if (event.kind == kActivity && event.durationSeconds) {
    const uint64_t eventEnd = static_cast<uint64_t>(event.unixSeconds) + event.durationSeconds;
    return event.unixSeconds < end && eventEnd > start;
  }
  return event.unixSeconds >= start && event.unixSeconds < end;
}

void initializeDatasetIdentity() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(deviceIdHex, sizeof(deviceIdHex), "%016llx",
           static_cast<unsigned long long>(mac));

  uint8_t epoch[16]{};
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    timezoneInitialized = preferences.isKey(kTimezoneOffsetKey);
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


// ---------------------------------------------------------------------------
// 范围查询（schema 5 聚合视图）
// ---------------------------------------------------------------------------
constexpr size_t kMaximumRangeBuckets = 30;

struct RangeBucket {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t coveredSeconds = 0;
  uint8_t coverageBits = 0;
  bool hasData = false;
  uint64_t runtime = 0;
  uint64_t span = 0;
  uint64_t trigger = 0;
  uint64_t segment = 0;
};

struct RangeSummary {
  uint64_t runtime = 0;
  uint64_t activity = 0;
  uint64_t segment = 0;
  uint64_t span = 0;
  uint64_t automatic = 0;
  uint64_t toiletSamples = 0;
  uint64_t toiletDuration = 0;
  uint8_t coverage = 0;
};

struct DetailStatus {
  bool withinRetention = false;
  bool capacityLimited = false;
  bool complete = false;
  bool timeUntrusted = false;
  const char* status = "OUT_OF_RETENTION";
};

// 详情完整性单一判定：统计接口与事件接口都必须调用它。
DetailStatus statisticsDetailStatus(uint32_t dayStart, uint32_t now) {
  DetailStatus result{};
  const uint32_t retention = static_cast<uint32_t>(StatisticsConfig::kDetailRetentionDays) * kSecondsPerDay;
  const uint32_t cutoff = now > retention ? now - retention : 0;
  result.withinRetention = dayStart >= cutoff;
  result.timeUntrusted = (persistentFlags & kHeaderFlagClockWentBackwards) != 0;
  result.capacityLimited = result.withinRetention &&
      (result.timeUntrusted || droppedThrough >= dayStart);
  result.complete = result.withinRetention && !result.capacityLimited;
  result.status = result.complete ? "COMPLETE"
                : result.capacityLimited ? "CAPACITY_LIMITED"
                : "OUT_OF_RETENTION";
  return result;
}

int64_t localDayIndexAt(int64_t epoch, int32_t offsetSeconds) {
  return (epoch + offsetSeconds) / kSecondsPerDay;
}

uint64_t dailyRuntimeOf(const DailyRecord& day) {
  uint64_t total = 0;
  for (size_t hour = 0; hour < 24; ++hour) total += day.runtimeBuckets[hour];
  return total;
}

// dayKey（YYYYMMDD）→ 该本地日零点对应的 UTC 秒。用 Howard Hinnant 的 days-from-civil。
uint32_t dayStartOfKey(uint32_t dayKey) {
  if (!dayKey) return 0;
  int32_t year = static_cast<int32_t>(dayKey / 10000);
  const int32_t month = static_cast<int32_t>((dayKey / 100) % 100);
  const int32_t day = static_cast<int32_t>(dayKey % 100);
  year -= (month <= 2) ? 1 : 0;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yearOfEra = static_cast<uint32_t>(year - era * 400);
  const uint32_t dayOfYear =
      (153u * static_cast<uint32_t>(month + (month > 2 ? -3 : 9)) + 2u) / 5u + static_cast<uint32_t>(day) - 1u;
  const uint32_t dayOfEra = yearOfEra * 365u + yearOfEra / 4u - yearOfEra / 100u + dayOfYear;
  const int64_t days = static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
  const int64_t localMidnight = days * kSecondsPerDay;
  return static_cast<uint32_t>(localMidnight - static_cast<int32_t>(timezoneOffsetMinutes) * 60);
}

// 服务端唯一选桶。约束：柱数落在 10–30，且每根柱的小时数能整除 24。
//   1–4 天  → 按小时铺满 24 列（1h/2h/3h/4h）
//   5–14 天 → 半天桶 12h（近 7 天 = 14 列）
//   15 天以上 → 日桶（30 天 = 30 列）
void chooseBucketPlan(uint32_t dayCount, uint8_t* bucketHours, uint8_t* bucketCount) {
  if (dayCount <= 1) {
    *bucketHours = 1;
    *bucketCount = 24;
    return;
  }
  if (dayCount <= 4) {
    // dayCount 小时一根柱：2/3/4 都能整除 24，柱数恒为 24。
    *bucketHours = static_cast<uint8_t>(dayCount);
    *bucketCount = 24;
    return;
  }
  if (dayCount <= 14) {
    *bucketHours = 12;
    *bucketCount = static_cast<uint8_t>(dayCount * 2);
    return;
  }
  *bucketHours = 24;
  *bucketCount = static_cast<uint8_t>(dayCount);
}

String jsonNumberOrNull(bool available, uint64_t value) {
  return available ? String(static_cast<unsigned long>(value)) : String("null");
}

// 收集未校时期间的累计量：只统计本次开机、尚未归属自然日的事件。
void collectPendingUnsynced(uint64_t* runtime, uint64_t* activity, uint64_t* automatic) {
  *runtime = 0; *activity = 0; *automatic = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    const EventRecord& event = events[i];
    if (!eventIsUnsynced(event) || event.unixSeconds == 0) continue;
    if (isFanRuntimeKind(event.kind) && event.state != kActive) *runtime += event.durationSeconds;
    if (event.kind == kActivity) *activity += event.reserved ? event.reserved : 1;
    if (event.kind == kAutomatic) ++*automatic;
  }
}

String buildRangeJson(uint32_t from, uint32_t to, int32_t tzOffsetSeconds,
                      StatisticsRangeError* error) {
  if (error) *error = StatisticsRangeError::kNone;
  const int32_t deviceOffset = static_cast<int32_t>(timezoneOffsetMinutes) * 60;
  if (tzOffsetSeconds != deviceOffset) {
    if (error) *error = StatisticsRangeError::kTimezoneMismatch;
    return String();
  }
  const uint32_t now = currentEpoch();
  if (!ready) {
    if (error) *error = StatisticsRangeError::kNotReady;
    return String();
  }
  if (to <= from || from < 1700000000UL) {
    if (error) *error = StatisticsRangeError::kInvalidRange;
    return String();
  }
  // 多日范围的起点必须落在本地零点；否则桶无法与每日汇总对齐。
  if (from != localDayStartOfEpoch(from)) {
    if (error) *error = StatisticsRangeError::kInvalidRange;
    return String();
  }
  const uint32_t lastSecond = to - 1;
  const int64_t firstDay = localDayIndexAt(from, deviceOffset);
  const int64_t lastDay = localDayIndexAt(lastSecond, deviceOffset);
  const uint32_t dayCount = static_cast<uint32_t>(lastDay - firstDay + 1);
  if (dayCount == 0) {
    if (error) *error = StatisticsRangeError::kInvalidRange;
    return String();
  }
  if (dayCount > StatisticsConfig::kSummaryRetentionDays) {
    if (error) *error = StatisticsRangeError::kRangeTooLarge;
    return String();
  }
  uint8_t bucketHours = 1;
  uint8_t bucketCount = 24;
  chooseBucketPlan(dayCount, &bucketHours, &bucketCount);
  if (bucketCount > kMaximumRangeBuckets) {
    if (error) *error = StatisticsRangeError::kRangeTooLarge;
    return String();
  }

  // 未校时：不伪造按日统计，只回报"本次开机累计"（§10.8）。
  if (now == 0) {
    uint64_t pendingRuntime = 0, pendingActivity = 0, pendingAutomatic = 0;
    collectPendingUnsynced(&pendingRuntime, &pendingActivity, &pendingAutomatic);
    String json = "{";
    // 预分配：拼接式扩容会按 4/8/16/32 二进制增长，每次 realloc 都在旧位置
    // 留下空洞。统计查询是最高频的大分配之一，碎片会直接压低 largest block。
    json.reserve(1024);
    json += "\"apiVersion\":2";
    json += ",\"deviceId\":\"" + String(statisticsStoreDeviceId()) + "\"";
    json += ",\"dataEpoch\":\"" + String(statisticsStoreDataEpoch()) + "\"";
    json += ",\"storageSchema\":" + String(kSchema);
    json += ",\"statisticsSchema\":" + String(statisticsSchemaCompatVersion());
    json += ",\"from\":" + String(from);
    json += ",\"to\":" + String(to);
    json += ",\"tzOffsetSeconds\":" + String(static_cast<long>(tzOffsetSeconds));
    json += ",\"timeSynced\":false";
    json += ",\"timeSyncSource\":\"" + String(statisticsStoreClockSyncSource()) + "\"";
    json += ",\"dayCount\":" + String(dayCount);
    json += ",\"bucketHours\":" + String(bucketHours);
    json += ",\"bucketCount\":" + String(bucketCount);
    json += ",\"summary\":{\"runtimeSeconds\":null,\"activityCount\":null,"
            "\"activitySegmentCount\":null,\"activitySpanSeconds\":null,"
            "\"automaticCount\":null,\"toiletSampleCount\":null,"
            "\"toiletAverageSeconds\":null}";
    json += ",\"coverage\":{\"runtime\":false,\"activityCount\":false,"
            "\"activitySegmentCount\":false,\"activitySpan\":false,"
            "\"activityTriggerBuckets\":false,\"activitySegmentBuckets\":false,"
            "\"activitySpanBuckets\":false,\"toilet\":false}";
    json += ",\"buckets\":[]";
    json += ",\"pendingUnsynced\":{";
    json += "\"runtimeSeconds\":" + String(static_cast<unsigned long>(pendingRuntime));
    json += ",\"activityCount\":" + String(static_cast<unsigned long>(pendingActivity));
    json += ",\"automaticCount\":" + String(static_cast<unsigned long>(pendingAutomatic));
    json += "}}";
    return json;
  }

  const uint32_t summaryRetention =
      static_cast<uint32_t>(StatisticsConfig::kSummaryRetentionDays - 1) * kSecondsPerDay;
  const uint32_t summaryCutoff = localDayStartOfEpoch(now) - summaryRetention;
  if (from < summaryCutoff) {
    if (error) *error = StatisticsRangeError::kRangeOutOfRetention;
    return String();
  }

  static RangeBucket buckets[kMaximumRangeBuckets];
  for (size_t i = 0; i < bucketCount; ++i) buckets[i] = {};
  RangeSummary summary{};
  summary.coverage = 0xFF;

  const uint32_t bucketsPerDay = static_cast<uint32_t>(24 / bucketHours);

  for (uint32_t dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
    const uint32_t dayStart = from + dayIndex * kSecondsPerDay;
    const uint32_t dayKey = dayKeyForEpoch(dayStart);
    const size_t recordIndex = findDaily(dayKey);
    const DailyRecord* day = recordIndex < kDailyCapacity ? &daily[recordIndex] : nullptr;
    // 保留期内没有记录 = 当天确实没有事件（日汇总在当天首个事件时创建），
    // 按"已知为零"处理；真正的数据丢失会通过 dataEpoch 轮换与保留期边界体现。
    const uint8_t bits = day ? day->metricCoverage : 0xFF;
    summary.coverage &= bits;
    if (day) {
      summary.runtime += dailyRuntimeOf(*day);
      summary.activity += day->activityCount;
      summary.segment += day->activitySegmentCount;
      summary.span += day->activitySpanSeconds;
      summary.automatic += day->automaticCount;
      summary.toiletSamples += day->toiletSampleCount;
      summary.toiletDuration += day->toiletDurationSeconds;
    }
    for (uint32_t slot = 0; slot < bucketsPerDay; ++slot) {
      const size_t bucketIndex = dayIndex * bucketsPerDay + slot;
      if (bucketIndex >= bucketCount) break;
      RangeBucket& bucket = buckets[bucketIndex];
      if (!bucket.hasData) {
        bucket.hasData = true;
        bucket.coverageBits = bits;
      } else {
        bucket.coverageBits &= bits;
      }
      if (!day) continue;
      for (uint32_t hour = slot * bucketHours; hour < (slot + 1) * bucketHours && hour < 24; ++hour) {
        bucket.runtime += day->runtimeBuckets[hour];
        bucket.span += day->activitySpanBuckets[hour];
        bucket.trigger += day->activityTriggerBuckets[hour];
        bucket.segment += day->activitySegmentBuckets[hour];
      }
    }
  }
  // 首日即缺失时 coverageBits 初值为 0，需要按"有数据"重新计算。
  for (size_t i = 0; i < bucketCount; ++i) {
    if (!buckets[i].hasData) {
      buckets[i].coverageBits = 0;
      continue;
    }
    const uint32_t bucketStart = from + static_cast<uint32_t>(i) * bucketHours * 3600UL;
    const uint32_t bucketEnd = bucketStart + static_cast<uint32_t>(bucketHours) * 3600UL;
    buckets[i].start = bucketStart;
    buckets[i].end = bucketEnd;
    buckets[i].coveredSeconds = to <= bucketStart ? 0
        : (to >= bucketEnd ? static_cast<uint32_t>(bucketHours) * 3600UL : to - bucketStart);
  }

  const bool coverageRuntime = (summary.coverage & kMetricRuntime) != 0;
  const bool coverageActivity = (summary.coverage & kMetricActivityCount) != 0;
  const bool coverageSegment = (summary.coverage & kMetricActivitySegmentCount) != 0;
  const bool coverageSpan = (summary.coverage & kMetricActivitySpanSeconds) != 0;
  const bool coverageToilet = (summary.coverage & kMetricToilet) != 0;

  String json = "{";
  // 范围响应最大约 30 桶 × 80 B，预分配避免 6-8 次 realloc 造成碎片。
  json.reserve(3072);
  json += "\"apiVersion\":2";
  json += ",\"deviceId\":\"" + String(statisticsStoreDeviceId()) + "\"";
  json += ",\"dataEpoch\":\"" + String(statisticsStoreDataEpoch()) + "\"";
  json += ",\"storageSchema\":" + String(kSchema);
  json += ",\"statisticsSchema\":" + String(statisticsSchemaCompatVersion());
  json += ",\"from\":" + String(from);
  json += ",\"to\":" + String(to);
  json += ",\"tzOffsetSeconds\":" + String(static_cast<long>(tzOffsetSeconds));
  json += ",\"timeSynced\":true";
  json += ",\"timeSyncSource\":\"" + String(statisticsStoreClockSyncSource()) + "\"";
  json += ",\"dayCount\":" + String(dayCount);
  json += ",\"bucketHours\":" + String(bucketHours);
  json += ",\"bucketCount\":" + String(bucketCount);
  json += ",\"summary\":{";
  json += "\"runtimeSeconds\":" + jsonNumberOrNull(coverageRuntime, summary.runtime);
  json += ",\"activityCount\":" + jsonNumberOrNull(coverageActivity, summary.activity);
  json += ",\"activitySegmentCount\":" + jsonNumberOrNull(coverageSegment, summary.segment);
  json += ",\"activitySpanSeconds\":" + jsonNumberOrNull(coverageSpan, summary.span);
  json += ",\"automaticCount\":" + String(static_cast<unsigned long>(summary.automatic));
  json += ",\"toiletSampleCount\":" + jsonNumberOrNull(coverageToilet, summary.toiletSamples);
  json += ",\"toiletAverageSeconds\":" + jsonNumberOrNull(
      coverageToilet && summary.toiletSamples > 0,
      summary.toiletSamples ? summary.toiletDuration / summary.toiletSamples : 0);
  json += "}";
  json += ",\"coverage\":{";
  json += "\"runtime\":" + String(coverageRuntime ? "true" : "false");
  json += ",\"activityCount\":" + String(coverageActivity ? "true" : "false");
  json += ",\"activitySegmentCount\":" + String(coverageSegment ? "true" : "false");
  json += ",\"activitySpan\":" + String(coverageSpan ? "true" : "false");
  json += ",\"activityTriggerBuckets\":" + String((summary.coverage & kMetricActivityTriggerBuckets) ? "true" : "false");
  json += ",\"activitySegmentBuckets\":" + String((summary.coverage & kMetricActivitySegmentBuckets) ? "true" : "false");
  json += ",\"activitySpanBuckets\":" + String((summary.coverage & kMetricActivitySpanBuckets) ? "true" : "false");
  json += ",\"toilet\":" + String(coverageToilet ? "true" : "false");
  json += "}";
  json += ",\"buckets\":[";
  for (size_t i = 0; i < bucketCount; ++i) {
    const RangeBucket& bucket = buckets[i];
    if (i) json += ',';
    const bool bRuntime = (bucket.coverageBits & kMetricRuntime) != 0;
    const bool bSpan = (bucket.coverageBits & kMetricActivitySpanBuckets) != 0;
    const bool bTrigger = (bucket.coverageBits & kMetricActivityTriggerBuckets) != 0;
    const bool bSegment = (bucket.coverageBits & kMetricActivitySegmentBuckets) != 0;
    // 键名精简 + 不可用字段整段省略：range 响应在 WiFi 通道下单次拼接，
    // 设备空闲堆只有 ~22KB、最大连续块 ~9.7KB，原来的 5.1KB 会拼不出来。
    // 省略即代表"不可用"，与 null 语义一致，App 按缺失判定。
    json += "{\"start\":" + String(bucket.start);
    json += ",\"end\":" + String(bucket.end);
    json += ",\"cov\":" + String(bucket.coveredSeconds);
    json += ",\"bits\":" + String(bucket.coverageBits);
    if (bRuntime) json += ",\"rt\":" + String(static_cast<unsigned long>(bucket.runtime));
    if (bSpan) json += ",\"span\":" + String(static_cast<unsigned long>(bucket.span));
    if (bTrigger) json += ",\"trig\":" + String(static_cast<unsigned long>(bucket.trigger));
    if (bSegment) json += ",\"seg\":" + String(static_cast<unsigned long>(bucket.segment));
    json += "}";
  }
  json += "]";
  if (dayCount > 1) {
    json += ",\"days\":[";
    for (uint32_t dayIndex = 0; dayIndex < dayCount; ++dayIndex) {
      if (dayIndex) json += ',';
      const uint32_t dayStart = from + dayIndex * kSecondsPerDay;
      const uint32_t dayKey = dayKeyForEpoch(dayStart);
      const size_t recordIndex = findDaily(dayKey);
      const DailyRecord* day = recordIndex < kDailyCapacity ? &daily[recordIndex] : nullptr;
      const uint8_t bits = day ? day->metricCoverage : 0;
      // 同桶一样精简：短键 + 不可用字段省略。
      json += "{\"date\":" + String(dayKey);
      json += ",\"start\":" + String(dayStart);
      json += ",\"end\":" + String(dayStart + kSecondsPerDay);
      json += ",\"bits\":" + String(bits);
      if (day && (bits & kMetricRuntime)) {
        json += ",\"rt\":" + String(static_cast<unsigned long>(dailyRuntimeOf(*day)));
      }
      if (day && (bits & kMetricActivityCount)) {
        json += ",\"act\":" + String(static_cast<unsigned long>(day->activityCount));
      }
      if (day && (bits & kMetricActivitySegmentCount)) {
        json += ",\"seg\":" + String(static_cast<unsigned long>(day->activitySegmentCount));
      }
      if (day && (bits & kMetricActivitySpanSeconds)) {
        json += ",\"span\":" + String(static_cast<unsigned long>(day->activitySpanSeconds));
      }
      json += ",\"auto\":" + String(static_cast<unsigned long>(day ? day->automaticCount : 0));
      if (day && (bits & kMetricToilet)) {
        json += ",\"toilet\":" + String(static_cast<unsigned long>(day->toiletSampleCount));
      }
      json += "}";
    }
    json += "]";
  }
  if (dayCount == 1) {
    const DetailStatus detail = statisticsDetailStatus(from, now);
    json += ",\"detail\":{";
    json += "\"status\":\"" + String(detail.status) + "\"";
    json += ",\"complete\":" + String(detail.complete ? "true" : "false");
    json += ",\"withinRetention\":" + String(detail.withinRetention ? "true" : "false");
    json += ",\"capacityLimited\":" + String(detail.capacityLimited ? "true" : "false");
    json += ",\"timeUntrusted\":" + String(detail.timeUntrusted ? "true" : "false");
    json += ",\"storedEventCount\":" + String(static_cast<unsigned long>(eventCount));
    json += ",\"eventCapacity\":" + String(static_cast<unsigned long>(kEventCapacity));
    json += "}";
  }
  // 汇总数据的实际覆盖范围（不随查询变化）。
  if (dailyCount > 0) {
    uint32_t earliest = 0xFFFFFFFFUL;
    uint32_t latest = 0;
    for (size_t i = 0; i < dailyCount; ++i) {
      const uint32_t start = dayStartOfKey(daily[i].dayKey);
      if (!start) continue;
      if (start < earliest) earliest = start;
      if (start > latest) latest = start;
    }
    json += ",\"availableFrom\":" + String(earliest);
    json += ",\"availableThrough\":" + String(latest + kSecondsPerDay);
  }
  json += '}';
  return json;
}

}  // namespace

void statisticsStoreTick() { persistIfDirty(); }

String statisticsStoreBuildRangeJson(uint32_t from, uint32_t to, int32_t tzOffsetSeconds,
                                     uint8_t* errorCode) {
  StatisticsRangeError error = StatisticsRangeError::kNone;
  String body = buildRangeJson(from, to, tzOffsetSeconds, &error);
  if (errorCode) *errorCode = static_cast<uint8_t>(error);
  return body;
}

uint16_t statisticsStoreSchemaVersion() { return kSchema; }

uint16_t statisticsStoreMinReadableSchemaVersion() {
  return FIRMWARE_MIN_READABLE_STATISTICS_SCHEMA;
}

uint16_t statisticsSchemaCompatVersion() {
  return FIRMWARE_STATISTICS_SCHEMA_COMPAT;
}

// 迁移已写入一槽、第二槽未补齐时：只补另一槽，不重新迁移。
bool completePendingMigration() {
  const uint32_t value = generation + 1;
  const char* path = slotPathForGeneration(value);
  if (!writeSnapshotToSlot(path, value, 0)) return false;
  if (!verifySnapshotInSlot(path, value)) return false;
  generation = value;
  persistentFlags &= static_cast<uint16_t>(~kHeaderFlagMigrationInProgress);
  Serial.println("Statistics: pending migration completed");
  return true;
}

// schema 4 → 5：先写中间态槽并读回校验，再补齐另一槽；两步都通过才算完成。
bool migrateSnapshotToSchema5() {
  persistentFlags |= kHeaderFlagMigrationInProgress;
  // 记录本次迁移覆盖到的最大自然日，供后续写入判定"当天是否有升级前的空洞"。
  for (size_t i = 0; i < dailyCount; ++i) {
    if (daily[i].dayKey > migratedThroughDayKey) migratedThroughDayKey = daily[i].dayKey;
  }
  const uint32_t firstValue = generation + 1;
  const char* firstPath = slotPathForGeneration(firstValue);
  if (!writeSnapshotToSlot(firstPath, firstValue, 0)) return false;
  if (!verifySnapshotInSlot(firstPath, firstValue)) return false;
  const uint32_t secondValue = firstValue + 1;
  const char* secondPath = slotPathForGeneration(secondValue);
  if (!writeSnapshotToSlot(secondPath, secondValue, 0)) return false;
  if (!verifySnapshotInSlot(secondPath, secondValue)) return false;
  generation = secondValue;
  persistentFlags &= static_cast<uint16_t>(~kHeaderFlagMigrationInProgress);
  Serial.println("Statistics: migrated schema 4 -> 5");
  return true;
}

void statisticsStoreBegin() {
  activeActivityId = 0;
  activeActivityGapSeconds = 0;
  activeActivityStartedAtMs = 0;
  lastActivityAtMs = 0;
  hasClockSyncSource = false;
  timezoneInitialized = false;
  initializeDatasetIdentity();
  if (!mountStatisticsFilesystem()) {
    Serial.println("Statistics: SPIFFS unavailable; statistics disabled");
    return;
  }
  // 1) 探测两槽公共前缀，选出「有效且 generation 最大」的候选次序。
  HeaderPrefix infoA{}, infoB{};
  const bool headerA = probeSlot(kSlotA, &infoA);
  const bool headerB = probeSlot(kSlotB, &infoB);
  const bool preferA = headerA && (!headerB || infoA.generation >= infoB.generation);
  const char* attemptOrder[2] = {preferA ? kSlotA : kSlotB, preferA ? kSlotB : kSlotA};

  // 2) 依次尝试完整读取（含 CRC 校验）；schema 4 会被转换成 V5。
  bool loaded = false;
  uint16_t loadedSchema = 0;
  bool loadedMigrationPending = false;
  StoreHeader selected{};
  for (size_t attempt = 0; attempt < 2 && !loaded; ++attempt) {
    HeaderPrefix info{};
    if (!probeSlot(attemptOrder[attempt], &info)) continue;
    if (loadSlot(attemptOrder[attempt], &selected)) {
      loaded = true;
      loadedSchema = info.schema;
      loadedMigrationPending = (info.reservedOrFlags & kHeaderFlagMigrationInProgress) != 0;
    }
  }
  if (loaded) {
    dailyCount = selected.dailyCount;
    // schema 6 的快照可能带着 512 条事件，数组只有 kEventCapacity：
    // 读取阶段已保留最新记录、推进被丢弃旧记录的边界，此处提交新容量。
    eventCount = selected.eventCount < kEventCapacity
        ? selected.eventCount
        : static_cast<uint16_t>(kEventCapacity);
    generation = selected.generation;
    nextEventId = selected.nextEventId;
    activeRun = selected.activeRun;
    droppedThrough = selected.droppedThrough;
    migratedThroughDayKey = selected.migratedThroughDayKey;
    persistentFlags = static_cast<uint16_t>(selected.flags & kHeaderFlagClockWentBackwards);
    if (loadedSchema == kSchemaV6) {
      // schema 6 → 7：布局已随 kEventCapacity 收窄，截断后立即按新布局落盘，
      // 避免每次启动都走兼容读取路径。
      Serial.printf("Statistics: migrating schema 6 -> 7 (events %u -> %u)\n",
                    static_cast<unsigned>(selected.eventCount),
                    static_cast<unsigned>(eventCount));
      if (!persist()) {
        Serial.println("Statistics: schema 6 -> 7 persist failed; will retry next boot");
      }
    } else if (loadedSchema == kSchemaV4) {
      // 迁移失败不影响本次开机使用内存中的 V5 数据，下次启动继续收敛。
      if (!migrateSnapshotToSchema5()) {
        Serial.println("Statistics: migration incomplete; will retry on next boot");
      }
    } else if (loadedMigrationPending) {
      if (!completePendingMigration()) {
        Serial.println("Statistics: pending migration incomplete; will retry on next boot");
      }
    }
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
    if (eventIsUnsynced(events[i])) {
      // 重启后无法再把 uptime 换成真实时间：时间标记为不可用。
      events[i].unixSeconds = 0;
      events[i].flags = 0;
    }
    if (events[i].state == kActive) {
      events[i].state = kInterrupted;
      events[i].endReason = static_cast<uint8_t>(StatisticsEndReason::REBOOT_INTERRUPTED);
    }
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

bool statisticsStoreClockSyncSafe() {
  if (!statisticsStoreHasClock()) return true;
  if (activeRun.id != 0 || pendingToiletStartEpoch != 0) return false;
  if (activeActivityId != 0 && activeActivityGapSeconds != 0 &&
      uint32_t(millis() - lastActivityAtMs) <
          static_cast<uint32_t>(activeActivityGapSeconds) * 1000UL) return false;
  return true;
}

bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes) {
  return statisticsStoreSetClockFromSource(unixSeconds, utcOffsetMinutes,
                                           StatisticsClockSource::kPhone);
}

bool statisticsStoreSetClockFromSource(uint32_t unixSeconds, int16_t utcOffsetMinutes,
                                       StatisticsClockSource source) {
  if (unixSeconds < 1700000000UL || utcOffsetMinutes < -720 || utcOffsetMinutes > 840) return false;
  // NAS 只提供 UTC；统计偏移必须先由手机/授权初始化流程固定。
  if (source == StatisticsClockSource::kNas && !timezoneInitialized) {
    Serial.println("Statistics: NAS clock deferred until timezone is initialized");
    return false;
  }
  const bool hadClock = statisticsStoreHasClock();
  const uint32_t nowMs = millis();
  const uint32_t oldEpoch = currentEpoch();
  const bool clockWentBackwards = oldEpoch != 0 && unixSeconds < oldEpoch;
  if (source == StatisticsClockSource::kPhone && hasClockSyncSource &&
      lastClockSyncSource == StatisticsClockSource::kNas &&
      uint32_t(nowMs - lastClockSyncAtMs) < 60000UL && oldEpoch != 0) {
    const uint32_t delta = oldEpoch > unixSeconds ? oldEpoch - unixSeconds : unixSeconds - oldEpoch;
    if (delta > 5) {
      Serial.printf("Statistics: phone clock conflicts with recent NAS sample delta=%lu s\n",
                    static_cast<unsigned long>(delta));
      return false;
    }
  }
  if (hadClock && !statisticsStoreClockSyncSafe()) {
    Serial.println("Statistics: clock sync deferred until the active segment is complete");
    return false;
  }
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
  timezoneInitialized = true;
  clockReady = true;
  if (clockWentBackwards) {
    persistentFlags |= kHeaderFlagClockWentBackwards;
    Serial.printf("Statistics: UTC moved backwards by %lu seconds\n",
                  static_cast<unsigned long>(oldEpoch - unixSeconds));
  }
  lastClockSyncAtMs = nowMs;
  lastClockSyncSource = source;
  hasClockSyncSource = true;
  // 不把跨时间映射的活动触发合并为同一段。
  activeActivityId = 0;
  activeActivityGapSeconds = 0;
  activeActivityStartedAtMs = 0;

  // 回填本次开机以来发生的未校时事件，并把已结束排风的时长归入对应日期。
  const uint32_t uptimeNow = millis() / 1000UL;
  for (size_t i = 0; i < eventCount; ++i) {
    EventRecord& event = events[i];
    if (!eventIsUnsynced(event)) continue;
    const uint32_t eventUptime = event.unixSeconds;
    const uint32_t elapsed = uptimeNow >= eventUptime
        ? uptimeNow - eventUptime
        : uptimeNow + kMillisWrapSeconds - eventUptime;
    if (elapsed >= unixSeconds || unixSeconds - elapsed < 1700000000UL) {
      event.unixSeconds = 0;
      event.flags = 0;
      continue;
    }
    event.unixSeconds = unixSeconds - elapsed;
    event.flags = 0;
    DailyRecord* day = ensureDaily(dayKeyForEpoch(event.unixSeconds));
    if (!day) continue;
    if (event.kind == kActivity) {
      const uint32_t amount = event.reserved ? event.reserved : 1;
      const uint32_t total = static_cast<uint32_t>(day->activityCount) + amount;
      day->activityCount = total > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(total);
    } else if (event.kind == kAutomatic && day->automaticCount < 0xFFFF) {
      ++day->automaticCount;
    }
    if (event.kind == kAutomatic && event.state != kActive) {
      // 未校时期间完成的自动排风无法判断是否达到阈值：该日入厕汇总整体降级为不可用，
      // 不能用"部分可信 + 部分缺失"拼出一个看起来完整的平均值。
      day->metricCoverage &= static_cast<uint8_t>(~kMetricToilet);
    }
    if (isFanRuntimeKind(event.kind) && event.state != kActive) {
      addRuntimeToDaily(event.unixSeconds, event.durationSeconds);
    }
  }
  if (activeRun.id && eventIsUnsynced(activeRun)) {
    const uint32_t uptime = activeRun.unixSeconds;
    const uint32_t elapsed = uptimeNow >= uptime ? uptimeNow - uptime
        : uptimeNow + kMillisWrapSeconds - uptime;
    activeRun.unixSeconds = elapsed < unixSeconds && unixSeconds - elapsed >= 1700000000UL
        ? unixSeconds - elapsed : 0;
    activeRun.flags = 0;
  }
  prune();
  findActiveEvent();
  persist();
  return true;
}

bool statisticsStoreHasClock() { return clockReady || currentEpoch() != 0; }

const char* statisticsStoreClockSyncSource() {
  if (!hasClockSyncSource) return "none";
  return lastClockSyncSource == StatisticsClockSource::kNas ? "nas" : "phone";
}

bool statisticsStoreTimezoneInitialized() { return timezoneInitialized; }

int32_t statisticsStoreTimezoneOffsetSeconds() {
  return static_cast<int32_t>(timezoneOffsetMinutes) * 60;
}

const char* statisticsStoreDeviceId() { return deviceIdHex; }
const char* statisticsStoreDataEpoch() { return dataEpochHex; }
const char* statisticsStoreBootId() { return bootIdHex; }

void statisticsStoreRecordActivity(uint16_t mergeGapSeconds) {
  if (!ready || mergeGapSeconds == 0) return;
  const uint32_t nowMs = millis();
  const uint32_t previousActivityAtMs = lastActivityAtMs;
  lastActivityAtMs = nowMs;
  // 未校时时保留逐条 uptime 记录，校时后才能准确按日期回填，不能把跨午夜触发合并到首日。
  if (!currentEpoch()) {
    activeActivityId = 0;
    activeActivityGapSeconds = 0;
    activeActivityStartedAtMs = 0;
    addEvent(kActivity, kComplete);
    return;
  }
  const EventTimestamp timestamp = eventTimestamp();
  if (activeActivityId && eventCount && events[eventCount - 1].id == activeActivityId) {
    EventRecord& previous = events[eventCount - 1];
    const uint32_t previousDurationSeconds = previous.durationSeconds;
    const uint64_t previousEndEpoch =
        static_cast<uint64_t>(previous.unixSeconds) + previousDurationSeconds;
    const uint32_t sinceLastTriggerMs = uint32_t(nowMs - previousActivityAtMs);
    const uint32_t elapsedSeconds =
        uint32_t(nowMs - activeActivityStartedAtMs) / 1000UL;
    const uint64_t mappedTriggerEpoch =
        static_cast<uint64_t>(previous.unixSeconds) + elapsedSeconds;
    if (sinceLastTriggerMs <=
            static_cast<uint32_t>(activeActivityGapSeconds) * 1000UL &&
        elapsedSeconds < UINT32_MAX && previousEndEpoch <= UINT32_MAX &&
        mappedTriggerEpoch <= UINT32_MAX && previous.reserved < UINT16_MAX) {
      StatisticsActivitySegment segment{
          1, previousDurationSeconds + 1,
          static_cast<uint32_t>(previous.reserved ? previous.reserved : 1),
          activeActivityGapSeconds, true};
      StatisticsActivitySegment completed{};
      const auto result = statisticsActivityAdd(&segment, elapsedSeconds + 1,
                                                mergeGapSeconds, &completed);
      if (result == StatisticsActivityResult::kExtended) {
        previous.durationSeconds = segment.last - segment.first;
        previous.reserved = static_cast<uint16_t>(segment.count);
        if (timestamp.clockWentBackwards) {
          previous.flags |= kEventFlagClockWentBackwards;
          persistentFlags |= kHeaderFlagClockWentBackwards;
        }
        lastActivitySpanSeconds = previous.durationSeconds;
        // 只补 (上次末端, 本次触发) 的新增跨度，跨日跨小时切分。
        const uint32_t mappedEnd = static_cast<uint32_t>(mappedTriggerEpoch);
        addActivitySpanToDaily(static_cast<uint32_t>(previousEndEpoch), mappedEnd);
        addActivityTriggerToDaily(mappedEnd, false);
        prune();
        persistDeferred();
        return;
      }
    }
  }

  activeActivityId = 0;
  activeActivityGapSeconds = 0;
  activeActivityStartedAtMs = 0;
  if (nextEventId == 0) return;
  if (!prepareEventSlot(kActivity)) return;
  const uint32_t eventId = nextEventId;
  nextEventId = eventId == UINT32_MAX ? 0 : eventId + 1;
  EventRecord& record = events[eventCount++];
  record = {};
  record.id = eventId;
  record.unixSeconds = timestamp.seconds;
  record.kind = kActivity;
  record.state = kComplete;
  record.endReason = static_cast<uint8_t>(StatisticsEndReason::COMPLETED);
  record.flags = timestamp.unsynced ? kEventFlagTimeUnsynced :
      (timestamp.clockWentBackwards ? kEventFlagClockWentBackwards : 0);
  record.reserved = 1;
  activeActivityId = eventId;
  activeActivityGapSeconds = mergeGapSeconds;
  activeActivityStartedAtMs = nowMs;
  // 新段开始：上一段的跨度不再属于这次活动，并记录本次段的起点。
  lastActivitySpanSeconds = 0;
  lastActivityStartEpoch = timestamp.seconds;
  addActivityTriggerToDaily(timestamp.seconds, true);
  prune();
  persist();
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

void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs,
                               uint16_t configuredSeconds) {
  if (!ready || kind == kActivity) return;
  (void)nowMs;
  addEvent(kind, kActive);
  if (kind == kAutomatic) {
    pendingToiletSpanSeconds = lastActivitySpanSeconds;
    pendingToiletStartEpoch = lastActivityStartEpoch;
  }
  if (kind == kAutomatic && eventCount) {
    events[eventCount - 1].reserved = configuredSeconds;
    activeRun.reserved = configuredSeconds;
  }
}

void statisticsStoreFanStopped(unsigned long durationMs, StatisticsEndReason reason) {
  if (!ready || activeRun.id == 0) return;
  activeRun.state = kComplete;
  activeRun.durationSeconds = durationMs / 1000UL;
  activeRun.endReason = static_cast<uint8_t>(reason);
  // 入厕汇总：一次完整自动排风记一个样本，时长取**该次感应活动的跨度**。
  // 原来用排风时长，但排风是"人离开后再吹 N 分钟"，不等于人在卫生间的时间。
  // 拿不到活动跨度时不计样本——用排风时长冒充会让平均值系统性偏大。
  if (activeRun.kind == kAutomatic && reason == StatisticsEndReason::COMPLETED &&
      activeRun.unixSeconds && !eventIsUnsynced(activeRun)) {
    const uint32_t configured = activeRun.reserved
        ? activeRun.reserved
        : static_cast<uint32_t>(settingsStoreGet(TimingProfile::PRODUCTION).autoExhaustMs / 1000UL);
    const uint32_t effective = pendingToiletSpanSeconds;
    if (configured && activeRun.durationSeconds * 2ULL >= configured && effective > 0) {
      // 按活动开始日归属，而不是排风开始日（跨午夜时两者可能不同日）。
      const uint32_t attributionEpoch =
          pendingToiletStartEpoch ? pendingToiletStartEpoch : activeRun.unixSeconds;
      DailyRecord* day = ensureDailyTrusted(dayKeyForEpoch(attributionEpoch));
      if (day) {
        const uint64_t total = static_cast<uint64_t>(day->toiletDurationSeconds) + effective;
        day->toiletDurationSeconds = total > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(total);
        if (day->toiletSampleCount < UINT16_MAX) {
          ++day->toiletSampleCount;
          if (!dayHasPreMigrationGap(day->dayKey)) day->metricCoverage |= kMetricToilet;
        } else {
          day->overflowFlags |= kMetricToilet;
          day->metricCoverage &= static_cast<uint8_t>(~kMetricToilet);
        }
      }
    }
  }
  // 排风结束即消费掉快照，避免下一次排风误用上一次的跨度。
  if (activeRun.kind == kAutomatic) {
    pendingToiletSpanSeconds = 0;
    pendingToiletStartEpoch = 0;
  }
  if (!eventIsUnsynced(activeRun)) {
    addRuntimeToDaily(activeRun.unixSeconds, activeRun.durationSeconds);
  }
  bool updated = false;
  for (size_t i = 0; i < eventCount; ++i) {
    if (events[i].id == activeRun.id) {
      events[i] = activeRun;
      updated = true;
      break;
    }
  }
  if (!updated && prepareEventSlot(activeRun.kind)) {
    // 明细记录已被容量淘汰：补回一条完整记录，避免这次运行时长永久丢失。
    // 未校时记录不参与 §6.7 的时间顺序不变量，因此追加是允许的。
    events[eventCount++] = activeRun;
  }
  activeRun = {};
  activeEventIndex = kEventCapacity;
  persist();
}

uint16_t countValidToilets(uint32_t start, uint32_t end, unsigned long activeRunMs,
                           uint64_t* durationSeconds = nullptr,
                           uint16_t* sampleCount = nullptr) {
  uint16_t count = 0;
  uint64_t totalDuration = 0;
  uint16_t samples = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    const EventRecord& event = events[i];
    if (event.kind != kAutomatic || !eventInRange(event, start, end)) continue;
    // reserved 保存开始时的设定排风秒数；旧记录没有该字段时使用当前正式配置的设定值。
    const uint32_t configured = event.reserved
        ? event.reserved
        : settingsStoreGet(TimingProfile::PRODUCTION).autoExhaustMs / 1000UL;
    if (!configured) continue;
    const uint32_t effective = event.state == kActive &&
            static_cast<size_t>(i) == activeEventIndex
        ? activeRunMs / 1000UL
        : event.durationSeconds;
    if (effective * 2 >= configured) {
      ++count;
      totalDuration += effective;
      ++samples;
    }
  }
  if (durationSeconds) *durationSeconds = totalDuration;
  if (sampleCount) *sampleCount = samples;
  return count;
}

String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs,
                                uint16_t eventOffset, uint8_t eventLimit) {
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
  uint64_t activityBuckets[24]{};
  uint32_t activityCounts[24]{};
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
        !eventIsUnsynced(current)) {
      const uint64_t activeSeconds = activeRunMs / 1000UL;
      addRuntimeToBuckets(current.unixSeconds, activeSeconds,
                          periodStart, periodEnd, bucketSpanHours,
                          bucketCount, buckets);
    }
  }
  if (now) addActivityToBuckets(periodStart, periodEnd, bucketSpanHours,
                                bucketCount, activityBuckets, activityCounts);
  for (uint8_t bucket = 0; bucket < bucketCount; ++bucket) runtimeSeconds += buckets[bucket];
  uint32_t eventStart = 0;
  uint32_t eventEnd = 0;
  uint32_t nextCursor = 0;
  uint64_t toiletDurationSeconds = 0;
  uint16_t toiletSampleCount = 0;
  uint16_t toiletCount = countValidToilets(periodStart, periodEnd, activeRunMs,
                                           &toiletDurationSeconds,
                                           &toiletSampleCount);
  if (range == "today" || range == "yesterday") {
    // 今日和昨日都使用本地自然日边界；今日窗口从本地零点到本次响应时间。
    eventStart = range == "today" ? todayStart : periodStart;
    eventEnd = range == "today" ? now + 1 : todayStart;
    for (int i = static_cast<int>(eventCount) - 1; i >= 0; --i) {
      if (!now) break;
      EventRecord event = events[i];
      if (event.state == kActive && static_cast<size_t>(i) == activeEventIndex) event.durationSeconds = activeRunMs / 1000UL;
      if (!eventInRange(event, eventStart, eventEnd)) continue;
      if (eventTotal >= eventOffset && recentCount < eventLimit) {
        if (recentCount++) eventsJson += ',';
        eventsJson += "{\"id\":" + String(event.id) +
                      ",\"time\":" + String(event.unixSeconds) + ",\"kind\":\"" + kindName(event.kind) +
                      "\",\"state\":\"" + stateName(event.state) + "\",\"durationSeconds\":" + String(event.durationSeconds);
        if (event.kind == kActivity) {
          eventsJson += ",\"triggerCount\":" + String(event.reserved ? event.reserved : 1);
        }
        eventsJson += "}";
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
      ",\"timeSyncSource\":\"" + String(statisticsStoreClockSyncSource()) + "\"" +
      ",\"active\":" + String(fanOn ? "true" : "false") +
      ",\"range\":\"" + range + "\",\"runtimeSeconds\":" + String(runtimeSeconds) +
      ",\"activityCount\":" + String(activityCount) + ",\"automaticCount\":" + String(automaticCount) +
      ",\"toiletCount\":" + String(toiletCount) +
      ",\"toiletAverageSeconds\":" +
      String(toiletSampleCount ? static_cast<uint32_t>(toiletDurationSeconds / toiletSampleCount) : 0) +
      ",\"toiletSampleCount\":" + String(toiletSampleCount) +
      ",\"bucketSeconds\":[";
  for (uint8_t i = 0; i < bucketCount; ++i) { if (i) json += ','; json += String(buckets[i]); }
  json += "],\"activityBucketSeconds\":[";
  for (uint8_t i = 0; i < bucketCount; ++i) {
    if (i) json += ',';
    json += String(activityBuckets[i]);
  }
  json += "],\"activityBucketCount\":[";
  for (uint8_t i = 0; i < bucketCount; ++i) {
    if (i) json += ',';
    json += String(activityCounts[i]);
  }
  json += "],\"events\":" + eventsJson;
  const bool eventsHasMore = static_cast<uint32_t>(eventOffset) + recentCount < eventTotal;
  // 旧 App 的兼容判断依赖该字段恒为 4，不得暴露内部落盘 schema。
  json += ",\"statisticsSchema\":" + String(statisticsSchemaCompatVersion());
  json += ",\"eventsTruncated\":" + String(droppedThrough && periodStart <= droppedThrough ? "true" : "false");
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
                    "\",\"durationSeconds\":" + String(event.durationSeconds);
      if (event.kind == kActivity) {
        eventsJson += ",\"triggerCount\":" + String(event.reserved ? event.reserved : 1);
      }
      eventsJson += "}";
      nextCursor = event.id;
    }
  }
  eventsJson += ']';
  const uint16_t nextOffset = offset + count;
  const bool hasMore = eligible > offset + count;

  // 与统计接口共用同一套完整性判定：按 start 所在本地日判，跨多日时只报 MULTI_DAY。
  const uint32_t now = currentEpoch();
  const uint32_t dayStart = localDayStartOfEpoch(start);
  const uint32_t dayCount = (end > start && now)
      ? static_cast<uint32_t>(localDayIndexAt(end - 1, static_cast<int32_t>(timezoneOffsetMinutes) * 60) -
                               localDayIndexAt(start, static_cast<int32_t>(timezoneOffsetMinutes) * 60) + 1)
      : 0;
  const bool singleDay = dayCount == 1;
  const DetailStatus detail = statisticsDetailStatus(dayStart, now);
  uint32_t oldestAvailableAt = 0;
  uint32_t newestAvailableAt = 0;
  for (size_t i = 0; i < eventCount; ++i) {
    if (!events[i].unixSeconds || eventIsUnsynced(events[i])) continue;
    if (!oldestAvailableAt || events[i].unixSeconds < oldestAvailableAt) oldestAvailableAt = events[i].unixSeconds;
    if (events[i].unixSeconds > newestAvailableAt) newestAvailableAt = events[i].unixSeconds;
  }

  String json = "{\"deviceId\":\"" + String(statisticsStoreDeviceId()) +
                "\",\"dataEpoch\":\"" + String(statisticsStoreDataEpoch()) +
                "\",\"bootId\":\"" + String(statisticsStoreBootId()) +
                "\",\"start\":" + String(start) + ",\"end\":" + String(end) +
                ",\"kind\":\"" + kind + "\",\"events\":" + eventsJson +
                ",\"offset\":" + String(offset) +
                ",\"nextOffset\":" + String(nextOffset) +
                ",\"total\":" + String(total) +
                ",\"nextCursor\":" + String(hasMore ? nextCursor : 0) +
                ",\"hasMore\":" + String(hasMore ? "true" : "false");
  // 旧 App 兼容字段：语义收敛为"该日有事件被丢弃"。
  json += ",\"eventsTruncated\":" + String(detail.capacityLimited ? "true" : "false");
  // 详情完整性：与 /api/stats/range 的 detail 块同源，App 只认这一份。
  const char* status = singleDay ? detail.status : "MULTI_DAY";
  json += ",\"detailStatus\":\"" + String(status) + "\"";
  json += ",\"detailComplete\":" + String((singleDay && detail.complete) ? "true" : "false");
  json += ",\"withinRetention\":" + String(detail.withinRetention ? "true" : "false");
  json += ",\"capacityLimited\":" + String(detail.capacityLimited ? "true" : "false");
  json += ",\"timeUntrusted\":" + String(detail.timeUntrusted ? "true" : "false");
  json += ",\"storedEventCount\":" + String(static_cast<unsigned long>(eventCount));
  json += ",\"eventCapacity\":" + String(static_cast<unsigned long>(kEventCapacity));
  if (oldestAvailableAt) {
    json += ",\"oldestAvailableAt\":" + String(oldestAvailableAt);
    json += ",\"newestAvailableAt\":" + String(newestAvailableAt);
  }
  json += "}";
  return json;
}
