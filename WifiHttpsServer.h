#pragma once
#include "WifiBindingStore.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
void wifiHttpsServerUpdate();
void wifiHttpsServerSuspendForOta();
bool wifiHttpsServerReady();
void wifiHttpsResetLease();
#endif
