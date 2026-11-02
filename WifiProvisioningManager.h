#pragma once

#include <Arduino.h>

#include "AppConfig.h"

#if MIMIVENT_ENABLE_WIFI
void wifiProvisioningBegin();
void wifiProvisioningUpdate();
String wifiProvisioningHandleRequest(const String& request,const String& owner = "");
bool wifiProvisioningIsReady();
#if MIMIVENT_WIFI_READ_CONTROL
void wifiProvisioningSuspendForOta();
#endif
#endif
