#pragma once

#include <Arduino.h>

#include "SettingsStore.h"

// 设备业务请求类型。BLE 与后续 Wi-Fi 传输层映射到同一组接口。
struct ControlApiRequest {
  String method;
  String path;
  TimingProfile profile = TimingProfile::PRODUCTION;
  String mode;
  String stage;
  String eventStart;
  String eventEnd;
  String eventKind;
  String eventOffset;
  String eventBeforeId;
  String eventDataEpoch;
  String eventLimit;
  String rangeFrom;
  String rangeTo;
  String rangeTz;
  String body;
  bool replace = false;
  bool switchProfile = false;
  bool hasSettings = false;
  bool hasDeadline = false;
  uint32_t issuedAtMs = 0;
  uint32_t maxAgeMs = 0;
  TimingSettings settings{};
};

struct ControlApiResponse {
  uint16_t statusCode;
  String contentType;
  String body;
};

// 初始化 BLE/HTTP 共享的控制互斥锁；必须在任务启动前调用。
void controlApiBegin();
void controlApiLock();
void controlApiUnlock();
// 分发通用控制请求。所有状态变更、校验和错误响应集中在此处。
ControlApiResponse controlApiDispatch(const ControlApiRequest& request);
// 将 HTTP 表单/BLE 协议中的毫秒值严格解析为当前配置允许的时间选项。
bool controlApiParseTimeValue(const String& value, bool allowZero, unsigned long* parsedValue);
