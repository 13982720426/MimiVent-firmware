#pragma once
#include "AppConfig.h"
#ifndef MIMIVENT_WIFI_READ_CONTROL
#define MIMIVENT_WIFI_READ_CONTROL 0
#endif
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
#include <cJSON.h>
struct WifiAuthorization { bool valid, pending; char tokenId[33]; };
void wifiBindingBegin();
void wifiBindingUpdate();
bool wifiBindingReady();
const char* wifiBindingState();
String wifiBindingRequest(cJSON* root,const char* owner);
WifiAuthorization wifiBindingAuthorize(const char* token,uint32_t now);
bool wifiBindingMarkVerified(const char* tokenId);
bool wifiBindingOwnerAllowed(const char* owner);
bool wifiBindingRevokeAllowed(const char* owner,const char* tokenId);
// 把当前有效绑定的 token 哈希附加到 MQTT 状态对象，供 NAS 校验只读授权；不发送明文 token。
bool wifiBindingAddAuthTokens(cJSON* state);
#endif
