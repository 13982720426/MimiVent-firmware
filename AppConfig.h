#pragma once

#include <Arduino.h>

// 项目级硬件与网络常量。用户可调时序由 SettingsStore 维护并保存到 NVS。
namespace AppConfig {

// GPIO 引脚
constexpr uint8_t kPirPin = 4;
constexpr uint8_t kRelayPin = 5;
constexpr uint8_t kStatusLedPin = 8;
constexpr bool kRelayActiveLow = false;  // 当前继电器板实测为高电平触发

// 25ms 轮询可及时响应 PIR，同时留出时间处理网页请求。
constexpr unsigned long kPirPollIntervalMs = 25UL;

// BLE GATT 服务标识。
constexpr char kBleDeviceName[] = "MimiVent";
constexpr char kBleServiceUuid[] = "6f9b0001-7d5a-4c3b-9a21-2f6d4d494d49";
constexpr char kBleCommandUuid[] = "6f9b0002-7d5a-4c3b-9a21-2f6d4d494d49";
constexpr char kBleResponseUuid[] = "6f9b0003-7d5a-4c3b-9a21-2f6d4d494d49";
constexpr char kBleOtaControlUuid[] = "6f9b0004-7d5a-4c3b-9a21-2f6d4d494d49";
constexpr char kBleOtaDataUuid[] = "6f9b0005-7d5a-4c3b-9a21-2f6d4d494d49";
constexpr char kBleOtaStatusUuid[] = "6f9b0006-7d5a-4c3b-9a21-2f6d4d494d49";

}  // namespace AppConfig
