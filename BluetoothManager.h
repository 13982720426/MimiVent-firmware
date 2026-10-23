#pragma once

// 初始化 BLE GATT 服务，供 HTTPS 网页客户端连接。
void bluetoothManagerBegin();

// 在主循环处理 BLE 请求，业务逻辑与 Wi-Fi HTTP API 共用 ControlApi。
void bluetoothManagerUpdate();
