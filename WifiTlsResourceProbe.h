#pragma once
#include "AppConfig.h"
#ifndef MIMIVENT_WIFI_TLS_PROBE
#define MIMIVENT_WIFI_TLS_PROBE 0
#endif
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_TLS_PROBE
void wifiTlsResourceProbeBegin();
void wifiTlsResourceProbeUpdate();
void wifiTlsResourceProbeSuspendForOta();
#endif
