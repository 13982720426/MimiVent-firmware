#pragma once

#include <Arduino.h>
#include "OtaCheckpointStore.h"

// OTA 写入与 BLE/未来 HTTP 传输解耦：各传输层只提交元数据和连续数据块。
void otaManagerBegin();
void otaManagerConfirmBoot();
void otaManagerUpdate();
void otaManagerOnConnect();
void otaManagerOnSecureSession(uint16_t connection);
void otaManagerOnDisconnect(uint16_t connection);
bool otaManagerIsActive();
bool otaManagerIsOwnerConnection(uint16_t connection);
String otaManagerStatusJson();
