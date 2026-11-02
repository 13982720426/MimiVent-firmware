/*
  项目：MimiVent
  设备：ESP32-C3-DevKitM-1
  固件版本：见 FirmwareVersion.h

  主程序只负责模块初始化和循环调度：
  - FanController：PIR、继电器和排风模式
  - BluetoothManager：BLE 控制与 OTA
  - StatusLed：板载 WS2812 状态灯
*/

#include "AppConfig.h"
#include "BluetoothManager.h"
#include "FanController.h"
#include "FirmwareVersion.h"
#include "StatusLed.h"
#include "SettingsStore.h"
#include "StatisticsStore.h"
#include "OtaManager.h"
#include "ControlApi.h"
#include "WifiTlsResourceProbe.h"
#include "WifiDeviceIdentity.h"
#include "WifiHttpsServer.h"
#include <esp_partition.h>
#include <esp_ota_ops.h>

#if MIMIVENT_ENABLE_WIFI && (MIMIVENT_WIFI_TLS_PROBE || MIMIVENT_WIFI_READ_CONTROL)
// P3 实测镜像查询/验签调用链在 8 KiB loop 栈下仅余约 596 B。
// 仅实验构建增加 2 KiB，重新测量堆余量后再决定生产配置。
SET_LOOP_TASK_STACK_SIZE(10240);
#endif

// Arduino-ESP32 默认在 initArduino() 中无条件确认新镜像；延后到 setup 完整启动后再确认，
// 让启动早期崩溃仍可由 ESP-IDF 回滚到原应用槽。
extern "C" bool verifyRollbackLater(void) { return true; }

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // USB 接着但主机未读取时，诊断日志不能阻塞 NimBLE 回调或主循环。
  // 缓冲满则丢弃本次输出；正常控制不依赖串口监听器在线。
  Serial.setTxTimeoutMs(0);
#endif

  controlApiBegin();
  settingsStoreBegin();
  // 先把继电器输出设为关闭，避免 SPIFFS 挂载/校验期间继电器输入悬空。
  fanControllerBegin();
  statisticsStoreBegin();
  statusLedBegin();
  otaManagerBegin();

#if MIMIVENT_ENABLE_WIFI && (MIMIVENT_WIFI_IDENTITY_PROBE || MIMIVENT_WIFI_READ_CONTROL)
  wifiDeviceIdentityBegin();
#endif
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
  wifiBindingBegin();
#endif
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_TLS_PROBE
  wifiTlsResourceProbeBegin();
#endif
  bluetoothManagerBegin();
  otaManagerConfirmBoot();

  Serial.println();
  Serial.printf("MimiVent v%s ready\n", FIRMWARE_VERSION);
  Serial.println(FIRMWARE_IMAGE_MARKER);
  Serial.println(FIRMWARE_STATISTICS_SCHEMA_MARKER);
  Serial.println(FIRMWARE_PARTITION_PROFILE_MARKER);
  Serial.printf("BLE device: %s\n", AppConfig::kBleDeviceName);
  Serial.println("Partition table:");
  esp_partition_iterator_t partition = esp_partition_find(
      ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (partition) {
    const esp_partition_t* item = esp_partition_get(partition);
    if (item) Serial.printf("  app %-12s address=0x%06lx size=%lu\n",
                            item->label, static_cast<unsigned long>(item->address),
                            static_cast<unsigned long>(item->size));
    partition = esp_partition_next(partition);
  }
  partition = esp_partition_find(ESP_PARTITION_TYPE_DATA,
                                  ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (partition) {
    const esp_partition_t* item = esp_partition_get(partition);
    if (item) Serial.printf("  data %-11s address=0x%06lx size=%lu\n",
                            item->label, static_cast<unsigned long>(item->address),
                            static_cast<unsigned long>(item->size));
    partition = esp_partition_next(partition);
  }
  const esp_partition_t* nextUpdate = esp_ota_get_next_update_partition(nullptr);
  if (nextUpdate) {
    Serial.printf("OTA next slot: %s address=0x%06lx size=%lu\n", nextUpdate->label,
                  static_cast<unsigned long>(nextUpdate->address),
                  static_cast<unsigned long>(nextUpdate->size));
  } else {
    Serial.println("OTA next slot: unavailable; selected partition table is not OTA-capable");
  }
  const TimingSettings production = settingsStoreGet(TimingProfile::PRODUCTION);
  const TimingSettings dev = settingsStoreGet(TimingProfile::DEV);
  Serial.printf("Production timing: startup cooldown %lu ms; leave %lu ms; automatic exhaust %lu ms; manual %lu ms; pause %lu ms\n",
                production.startupCooldownMs, production.leaveConfirmMs, production.autoExhaustMs,
                production.manualExhaustMs, production.pauseMs);
  Serial.printf("DEV timing: startup cooldown %lu ms; leave %lu ms; automatic exhaust %lu ms; manual %lu ms; pause %lu ms\n",
                dev.startupCooldownMs, dev.leaveConfirmMs, dev.autoExhaustMs,
                dev.manualExhaustMs, dev.pauseMs);
}

void loop() {
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_READ_CONTROL
  wifiHttpsServerUpdate();
#endif
  // 先处理 BLE 命令，再读取时钟。命令处理器可能刚设置 overrideStartedMs；
  // 若使用处理请求之前保存的 nowMs，unsigned 差值会下溢并让定时模式立即到期。
  bluetoothManagerUpdate();
#if MIMIVENT_ENABLE_WIFI && MIMIVENT_WIFI_TLS_PROBE
  wifiTlsResourceProbeUpdate();
#endif

  const unsigned long nowMs = millis();
  controlApiLock();
  fanControllerUpdate(nowMs);
  controlApiUnlock();

  static unsigned long lastStatusLedUpdateMs = 0;
  if (nowMs - lastStatusLedUpdateMs >= 20UL) {
    lastStatusLedUpdateMs = nowMs;
    statusLedUpdate(fanControllerIsFanOn(), nowMs);
  }

  delay(1);  // 让出少量 CPU 时间，保持网页请求响应及时。
}
