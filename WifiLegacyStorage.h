#pragma once
#include "WifiBindingStore.h"
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
String wifiLegacyStorageRequest(cJSON* root,const char* owner);
#endif
