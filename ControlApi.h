#pragma once

#include <Arduino.h>

#include "SettingsStore.h"

// 两种传输层共用的请求类型。WebDashboard 将 HTTP 参数映射到此结构；
// BluetoothManager 将 BLE 网页命令映射到同一结构。
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
  String eventLimit;
  String body;
  bool replace = false;
  bool switchProfile = false;
  bool hasSettings = false;
  TimingSettings settings{};
};

struct ControlApiResponse {
  uint16_t statusCode;
  String contentType;
  String body;
};

// 分发通用控制请求。所有状态变更、校验和错误响应集中在此处。
ControlApiResponse controlApiDispatch(const ControlApiRequest& request);
// 将 HTTP 表单/BLE 协议中的毫秒值严格解析为当前配置允许的时间选项。
bool controlApiParseTimeValue(const String& value, bool allowZero, unsigned long* parsedValue);
