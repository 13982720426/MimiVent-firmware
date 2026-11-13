#pragma once

#include "AppConfig.h"

#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include <Arduino.h>
#include <cstdint>
void wifiControlServiceBegin();
void wifiControlServiceUpdate();
bool wifiControlServiceCommand(const char* token, const char* requestId,
                               const char* path, const char* body,
                               uint32_t issuedAtMs, uint16_t* statusCode,
                               String* responseBody);
#endif
