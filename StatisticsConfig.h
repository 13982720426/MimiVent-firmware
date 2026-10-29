#pragma once

#include <stddef.h>
#include <stdint.h>

namespace StatisticsConfig {

// 保留周期和预算集中配置，变更容量时必须重新执行满载验收。
constexpr uint8_t kDetailRetentionDays = 10;
constexpr uint8_t kSummaryRetentionDays = 30;
constexpr size_t kStatisticsPartitionBytes = 512UL * 1024UL;
constexpr size_t kLogicalTargetBytes = 320UL * 1024UL;
constexpr size_t kSingleFanEventBudgetBytes = 240UL * 1024UL;
constexpr size_t kMultiEndpointEventBudgetBytes = 192UL * 1024UL;
constexpr size_t kSegmentTargetBytes = 8UL * 1024UL;
constexpr uint32_t kRecoveryCheckpointIntervalMs = 30000UL;
constexpr uint8_t kStorageQueueCapacity = 64;
constexpr uint16_t kDefaultPageRows = 64;
constexpr uint16_t kMaximumPageRows = 128;
constexpr uint16_t kDefaultPageBytes = 4096;

}  // namespace StatisticsConfig
