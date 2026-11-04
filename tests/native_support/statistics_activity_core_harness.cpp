#include <cassert>
#include "../../StatisticsActivityCore.h"
int main() {
  StatisticsActivitySegment s{}, done{};
  assert(statisticsActivityAdd(&s, 0, 120, &done) == StatisticsActivityResult::kInvalid);
  assert(statisticsActivityAdd(&s, 100, 120, &done) == StatisticsActivityResult::kStarted);
  assert(statisticsActivityAdd(&s, 220, 30, &done) == StatisticsActivityResult::kExtended);
  assert(s.gapSeconds == 120 && s.count == 2);
  assert(statisticsActivityAdd(&s, 341, 30, &done) == StatisticsActivityResult::kNewSegment);
  assert(done.first == 100 && done.last == 220 && done.count == 2 && !done.active);
  assert(s.first == 341 && s.gapSeconds == 30);
  assert(statisticsActivityAdd(&s, 300, 30, &done) == StatisticsActivityResult::kNewSegment);
  assert(done.first == 341 && s.first == 300);
  assert(statisticsActivityOverlap(3598, 3602, 0, 3600) == 2);
  assert(statisticsActivityOverlap(3598, 3602, 3600, 7200) == 2);
  assert(statisticsActivityOverlap(100, 100, 0, 1000) == 0);
  assert(statisticsActivityOverlap(200, 100, 0, 1000) == 0);
  uint32_t buckets[24]{};
  StatisticsActivitySegment crossing{3598, 3602, 2, 120, false};
  assert(statisticsActivityAccumulateDay(crossing, 0, buckets, 24));
  assert(buckets[0] == 2 && buckets[1] == 2 && buckets[2] == 0);
  assert(!statisticsActivityAccumulateDay(crossing, 0, buckets, 23));
  StatisticsActivitySegment midnight{86398, 86402, 2, 120, false};
  uint32_t firstDay[24]{}, secondDay[24]{};
  assert(statisticsActivityAccumulateDay(midnight, 0, firstDay, 24));
  assert(statisticsActivityAccumulateDay(midnight, 86400, secondDay, 24));
  assert(firstDay[23] == 2 && secondDay[0] == 2);
  buckets[0] = UINT32_MAX - 1;
  assert(statisticsActivityAccumulateDay(crossing, 0, buckets, 24));
  assert(buckets[0] == UINT32_MAX);
  StatisticsActivitySegment invalid{300, 200, 2, 120, false};
  assert(!statisticsActivityAccumulateDay(invalid, 0, buckets, 24));
}
