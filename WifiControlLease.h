#pragma once

#include "AppConfig.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include <stdint.h>

// HTTPS 与 MQTT 共用的单设备 WiFi 控制租约。
bool wifiControlLeaseAcquire(const char* tokenId, uint32_t nowMs);
void wifiControlLeaseRelease(const char* tokenId);
void wifiControlLeaseReset();
#endif
