#pragma once

#include <Arduino.h>

uint16_t statisticsStoreSchemaVersion();

// 设备侧统计：独立 statistics 分区，明细最多 512 条且最长 10 天，小时汇总保留 30 天。
void statisticsStoreBegin();
bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes);
bool statisticsStoreHasClock();
const char* statisticsStoreDeviceId();
const char* statisticsStoreDataEpoch();
const char* statisticsStoreBootId();
void statisticsStoreRecordActivity(uint16_t mergeGapSeconds);
void statisticsStoreRecordTimedPause(unsigned long durationMs);
void statisticsStoreRecordLockedOff();
void statisticsStoreRecordAutoResume();
void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs,
                               uint16_t configuredSeconds = 0);
void statisticsStoreFanStopped(unsigned long durationMs);
String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs,
                                uint16_t eventOffset = 0);
String statisticsStoreBuildEventsJson(uint32_t start, uint32_t end, const String& kind,
                                      uint16_t offset, uint8_t limit,
                                      unsigned long activeRunMs,
                                      uint32_t beforeId = 0);
