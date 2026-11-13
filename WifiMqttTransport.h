#pragma once

#include "AppConfig.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include <cJSON.h>

void wifiMqttBegin();
void wifiMqttUpdate();
// OTA 准备阶段提前释放 MQTT 客户端占用的堆，便于申请有界解码工作区。
void wifiMqttSuspendForOta();
bool wifiMqttConfigured();
bool wifiMqttConnected();
String wifiMqttHandleRequest(cJSON* root, const char* owner);
#endif
