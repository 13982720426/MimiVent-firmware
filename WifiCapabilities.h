#pragma once

// 返回编译期支持的能力；运行时在线状态由独立字段表达。
inline const char* wifiCapabilitiesJson(bool wifiEnabled, bool mqttEnabled) {
  if (!wifiEnabled) {
    return "[\"ble.control\",\"ble.stats\",\"ble.ota\"]";
  }
  if (!mqttEnabled) {
    return "[\"ble.control\",\"ble.stats\",\"ble.ota\",\"wifi.configure\"]";
  }
  return "[\"ble.control\",\"ble.stats\",\"ble.ota\",\"wifi.configure\",\"wifi.bind\",\"wifi.mqtt.control\",\"wifi.mqtt.query\",\"wifi.time-sync\"]";
}
