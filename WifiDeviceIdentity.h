#pragma once
#include "AppConfig.h"
#include <stddef.h>
#include <stdint.h>

#ifndef MIMIVENT_WIFI_IDENTITY_PROBE
#define MIMIVENT_WIFI_IDENTITY_PROBE 0
#endif

#if MIMIVENT_ENABLE_WIFI && (MIMIVENT_WIFI_IDENTITY_PROBE || MIMIVENT_WIFI_READ_CONTROL)
// P3-A 持久化实验，仅在无线栈启动前调用；不提供业务授权或重置接口。
void wifiDeviceIdentityBegin();
bool wifiDeviceIdentityReady();
const char* wifiDeviceIdentityState();
const char* wifiDeviceId();
const char* wifiDevicePin();
const uint8_t* wifiDeviceCertificate(size_t* length);
const uint8_t* wifiDevicePrivateKey(size_t* length);
#endif
