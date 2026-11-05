#pragma once

#include <stdint.h>

// 统计协议稳定枚举。发布后不能改义或复用编码。
enum class StatisticsRecordType : uint8_t {
  PIR_TRIGGER = 1,
  OP_START = 2,
  OP_END = 3,
  RUN_START = 4,
  RUN_END = 5,
  AUTO_SELECTED = 6,
  CLOCK_ANCHOR = 7,
  BOOT = 8,
  SHUTDOWN_INTENT = 9,
};

enum class StatisticsMode : uint8_t {
  AUTOMATIC = 1,
  MANUAL = 2,
  PAUSE = 3,
  FORCE = 4,
  LOCKED_OFF = 5,
};

enum class StatisticsSource : uint8_t {
  AUTOMATIC = 1,
  BLE = 2,
  HTTP = 3,
  SYSTEM = 4,
};

enum class StatisticsProfile : uint8_t {
  PRODUCTION = 1,
  DEV = 2,
};

enum class StatisticsEndReason : uint8_t {
  // 迁移前的旧记录：语义是"未知"，不是"已完成"。零值必须是未知，
  // 因为结构体被 memset/默认构造时该字段自然是 0，否则会伪造一次成功完成。
  LEGACY_UNKNOWN = 0,
  COMPLETED = 1,
  REPLACED = 2,
  RESTARTED = 3,
  PIR_INTERRUPTED = 4,
  CANCELLED = 5,
  REBOOT_INTERRUPTED = 6,
  UPGRADE_RESTART = 7,
  OWNER_CHANGED = 8,
};

enum class StatisticsDurationQuality : uint8_t {
  UNKNOWN = 0,
  EXACT = 1,
  CONFIRMED_MINIMUM = 2,
  INFERRED = 3,
};

enum class StatisticsSubjectKind : uint8_t {
  SYSTEM = 0,
  SENSOR = 1,
  FAN = 2,
  ZONE = 3,
};

// 端点标识分别属于 sensor/fan/zone 命名空间，GPIO 只留在硬件适配层。
using SensorId = uint16_t;
using FanId = uint16_t;
using ZoneId = uint16_t;
using StatisticsEventId = uint32_t;
using StatisticsOperationId = uint32_t;
using StatisticsRunId = uint32_t;

// 事件级标志。与 StoreHeader 的 flags 是不同字段，位编号各自独立、不可混用。
// 未校时状态只能用这个位表达：禁止借用 unixSeconds 的高位，否则与 2038 年后的
// 真实时间戳冲突。
constexpr uint8_t kEventFlagTimeUnsynced = 1u << 0;

// 每日汇总的指标覆盖位与溢出位共用同一套位映射。
constexpr uint8_t kMetricRuntime = 1u << 0;
constexpr uint8_t kMetricActivityCount = 1u << 1;
constexpr uint8_t kMetricActivitySegmentCount = 1u << 2;
constexpr uint8_t kMetricActivitySpanSeconds = 1u << 3;
constexpr uint8_t kMetricActivityTriggerBuckets = 1u << 4;
constexpr uint8_t kMetricActivitySegmentBuckets = 1u << 5;
constexpr uint8_t kMetricActivitySpanBuckets = 1u << 6;
constexpr uint8_t kMetricToilet = 1u << 7;

constexpr SensorId kDefaultSensorId = 1;
constexpr FanId kDefaultFanId = 1;
constexpr ZoneId kDefaultZoneId = 1;
