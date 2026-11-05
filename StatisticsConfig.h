#pragma once

#include <stddef.h>
#include <stdint.h>

namespace StatisticsConfig {

// 保留周期。详情与汇总各自独立，变更时必须重新执行满载验收。
constexpr uint8_t kDetailRetentionDays = 10;
constexpr uint8_t kSummaryRetentionDays = 30;

// 统计分区大小，与 partitions.csv 的 statistics 分区一致。
constexpr size_t kStatisticsPartitionBytes = 512UL * 1024UL;

// 事件明细容量：条数与保留天数谁先到就按谁截断。
// 512 条（10 KB 常驻）把 BLE+WiFi 双栈下的空闲堆压到 18 KB，正好卡在
// 一次 TLS 会话的临界点上，导致 WiFi 握手时成时败。256 条腾出 5 KB。
// 实际覆盖范围约 3 天（App 的明细入口也收敛到今天/昨天/前天）。
constexpr size_t kEventCapacity = 256;

// 写入节流：活动段延长走脏标记合并，避免每次 PIR 触发都全量重写快照。
constexpr uint32_t kPersistMinIntervalMs = 5000UL;

// 时钟回拨判定阈值：需大于 NTP 微调与秒级抖动，又要在人为改时间时及时捕获。
constexpr uint32_t kClockRollbackToleranceSeconds = 300UL;

// 分页
constexpr uint16_t kDefaultPageRows = 64;
constexpr uint16_t kMaximumPageRows = 128;
constexpr uint16_t kDefaultPageBytes = 4096;

}  // namespace StatisticsConfig
