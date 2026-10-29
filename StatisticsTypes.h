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

constexpr SensorId kDefaultSensorId = 1;
constexpr FanId kDefaultFanId = 1;
constexpr ZoneId kDefaultZoneId = 1;
