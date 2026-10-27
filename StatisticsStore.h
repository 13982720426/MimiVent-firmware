#pragma once

#include <Arduino.h>

// 设备侧轻量统计：按日保存汇总 365 天，事件明细保留最近 30 天。
void statisticsStoreBegin();
bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes);
bool statisticsStoreHasClock();
void statisticsStoreRecordActivity();
void statisticsStoreRecordTimedPause(unsigned long durationMs);
void statisticsStoreRecordLockedOff();
void statisticsStoreRecordAutoResume();
void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs);
void statisticsStoreFanStopped(unsigned long durationMs);
String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs,
                                uint16_t eventOffset = 0);
String statisticsStoreBuildEventsJson(uint32_t start, uint32_t end, const String& kind,
                                      uint16_t offset, uint8_t limit,
                                      unsigned long activeRunMs);
