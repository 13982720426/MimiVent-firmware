#pragma once

#include <stdint.h>
#include <limits.h>

// 不依赖时钟或持久化，调用方负责结束后的保存、校时与断电恢复。
struct StatisticsActivitySegment {
  uint32_t first = 0;
  uint32_t last = 0;
  uint32_t count = 0;
  uint32_t gapSeconds = 0;
  bool active = false;
};

enum class StatisticsActivityResult { kStarted, kExtended, kNewSegment, kInvalid };

// 间隔恰好达到阈值仍属于同一段；时间回退则结束旧段并开启新段。
inline StatisticsActivityResult statisticsActivityAdd(
    StatisticsActivitySegment* segment, uint32_t timestamp, uint32_t gapSeconds,
    StatisticsActivitySegment* completed) {
  if (!segment || !completed || segment == completed || timestamp == 0 || gapSeconds == 0)
    return StatisticsActivityResult::kInvalid;
  *completed = {};
  if (!segment->active) {
    *segment = {timestamp, timestamp, 1, gapSeconds, true};
    return StatisticsActivityResult::kStarted;
  }
  if (timestamp >= segment->last && timestamp - segment->last <= segment->gapSeconds &&
      segment->count < UINT32_MAX) {
    segment->last = timestamp;
    ++segment->count;
    return StatisticsActivityResult::kExtended;
  }
  *completed = *segment;
  completed->active = false;
  *segment = {timestamp, timestamp, 1, gapSeconds, true};
  return StatisticsActivityResult::kNewSegment;
}

// 桶使用左闭右开时间窗口；返回本桶内实际覆盖的秒数。
inline uint32_t statisticsActivityOverlap(uint32_t first, uint32_t last,
                                          uint32_t start, uint32_t end) {
  if (last < first || end <= start) return 0;
  const uint32_t clippedStart = first > start ? first : start;
  const uint32_t clippedEnd = last < end ? last : end;
  return clippedEnd > clippedStart ? clippedEnd - clippedStart : 0;
}

// 将已观察到的跨度切入指定自然日的小时桶；调用方负责持久化和防止重复入账。
// 使用 64 位中间值，避免临近 uint32 时间上限时小时边界溢出。
inline bool statisticsActivityAccumulateDay(const StatisticsActivitySegment& segment,
                                             uint32_t dayStart,
                                             uint32_t* buckets,
                                             unsigned bucketCount) {
  if (!buckets || bucketCount != 24 || !segment.first || segment.last < segment.first)
    return false;
  const uint64_t dayBegin = dayStart;
  for (unsigned hour = 0; hour < 24; ++hour) {
    const uint64_t begin = dayBegin + static_cast<uint64_t>(hour) * 3600;
    const uint64_t end = begin + 3600;
    const uint64_t from = segment.first > begin ? segment.first : begin;
    const uint64_t to = segment.last < end ? segment.last : end;
    if (to <= from) continue;
    const uint64_t updated = static_cast<uint64_t>(buckets[hour]) + to - from;
    buckets[hour] = updated > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(updated);
  }
  return true;
}

// schema 8 起逐小时桶为 uint16_t。单小时最多 3600 秒，正常数据不会超限；
// 异常计时超限时饱和到 UINT16_MAX，避免回绕成小值。
inline bool statisticsActivityAccumulateDay(const StatisticsActivitySegment& segment,
                                            uint32_t dayStart,
                                            uint16_t* buckets,
                                            unsigned bucketCount) {
  if (!buckets || bucketCount != 24 || !segment.first || segment.last < segment.first)
    return false;
  const uint64_t dayBegin = dayStart;
  for (unsigned hour = 0; hour < 24; ++hour) {
    const uint64_t begin = dayBegin + static_cast<uint64_t>(hour) * 3600;
    const uint64_t end = begin + 3600;
    const uint64_t from = segment.first > begin ? segment.first : begin;
    const uint64_t to = segment.last < end ? segment.last : end;
    if (to <= from) continue;
    const uint64_t updated = static_cast<uint64_t>(buckets[hour]) + to - from;
    buckets[hour] = updated > UINT16_MAX ? static_cast<uint16_t>(UINT16_MAX)
                                         : static_cast<uint16_t>(updated);
  }
  return true;
}
