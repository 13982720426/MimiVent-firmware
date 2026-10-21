#pragma once

#include <Arduino.h>

// 状态灯模块对主程序公开的接口。

// 初始化板载 WS2812 RGB 灯，并播放三次绿色启动提示。
void statusLedBegin();

// 根据风机状态刷新灯效：
// fanOn 为 false 时慢速蓝色呼吸；为 true 时快速、较亮的橙色呼吸。
// nowMs 传入 millis() 当前值，用于计算呼吸动画相位。
void statusLedUpdate(bool fanOn, unsigned long nowMs);
