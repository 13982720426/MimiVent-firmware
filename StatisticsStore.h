#pragma once

#include <Arduino.h>

#include "StatisticsTypes.h"

// 范围查询错误码。0 表示成功。
enum class StatisticsRangeError : uint8_t {
  kNone = 0,
  kInvalidRange = 1,
  kRangeTooLarge = 2,
  kRangeOutOfRetention = 3,
  kTimezoneMismatch = 4,
  kNotReady = 5,
};

enum class StatisticsClockSource : uint8_t {
  kPhone = 0,
  kNas = 1,
};

// 统一范围查询（schema 5 聚合视图）。失败时返回空串并用 errorCode 给出原因。
String statisticsStoreBuildRangeJson(uint32_t from, uint32_t to, int32_t tzOffsetSeconds,
                                     uint8_t* errorCode);

// 落盘格式版本（5）。OTA 与能力判断用它。
uint16_t statisticsStoreSchemaVersion();
// 本固件能读的最低落盘格式（6）。OTA 安装门槛用它判断能否迁移。
uint16_t statisticsStoreMinReadableSchemaVersion();
// 旧 App 兼容字段：/api/status 与 /api/stats 的 statisticsSchema 恒为 4。
uint16_t statisticsSchemaCompatVersion();

// 设备侧统计：独立 statistics 分区，明细最多 256 条且最长 10 天，小时汇总保留 30 天。
void statisticsStoreBegin();
// 让合并写入的脏快照在空闲时也能落盘。
void statisticsStoreTick();
bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes);
bool statisticsStoreSetClockFromSource(uint32_t unixSeconds, int16_t utcOffsetMinutes,
                                       StatisticsClockSource source);
bool statisticsStoreClockSyncSafe();
bool statisticsStoreHasClock();
// 最近一次在当前设备运行周期内成功接受的校时来源。
const char* statisticsStoreClockSyncSource();
// 统计时区是否已由手机/授权初始化流程固定；NAS 不得初始化该偏移。
bool statisticsStoreTimezoneInitialized();
// 设备固定时区偏移（秒）。App 首次查询 range 接口时必须带上它。
int32_t statisticsStoreTimezoneOffsetSeconds();
const char* statisticsStoreDeviceId();
const char* statisticsStoreDataEpoch();
const char* statisticsStoreBootId();
void statisticsStoreRecordActivity(uint16_t mergeGapSeconds);
void statisticsStoreRecordTimedPause(unsigned long durationMs);
void statisticsStoreRecordLockedOff();
void statisticsStoreRecordAutoResume();
void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs,
                               uint16_t configuredSeconds = 0);
void statisticsStoreFanStopped(
    unsigned long durationMs,
    StatisticsEndReason reason = StatisticsEndReason::COMPLETED);
String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs,
                                uint16_t eventOffset = 0,
                                uint8_t eventLimit = 32);
String statisticsStoreBuildEventsJson(uint32_t start, uint32_t end, const String& kind,
                                      uint16_t offset, uint8_t limit,
                                      unsigned long activeRunMs,
                                      uint32_t beforeId = 0);
