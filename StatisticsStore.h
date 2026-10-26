#pragma once

#include <Arduino.h>

// 设备侧轻量统计：按日保存汇总 365 天，事件明细保留最近 30 天。
void statisticsStoreBegin();
bool statisticsStoreSetClock(uint32_t unixSeconds, int16_t utcOffsetMinutes);
bool statisticsStoreHasClock();
void statisticsStoreRecordActivity();
void statisticsStoreFanStarted(uint8_t kind, unsigned long nowMs);
void statisticsStoreFanStopped(unsigned long durationMs);
String statisticsStoreBuildJson(const String& range, bool fanOn, unsigned long activeRunMs);
