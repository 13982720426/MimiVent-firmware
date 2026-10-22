#pragma once

#include <Arduino.h>

// 启动 ESP32 自建控制热点，返回是否成功。
bool wifiManagerBegin();

// 返回设备在热点模式下的本地 IP。
String wifiManagerGetIp();
