/*
  项目：MimiVent
  设备：ESP32-C3-DevKitM-1
  固件版本：见 FirmwareVersion.h

  主程序只负责模块初始化和循环调度：
  - FanController：PIR、继电器和排风模式
  - WifiManager：ESP32 本地热点
  - WebDashboard：手机页面与 HTTP API
  - StatusLed：板载 WS2812 状态灯
*/

#include "AppConfig.h"
#include "FanController.h"
#include "FirmwareVersion.h"
#include "StatusLed.h"
#include "SettingsStore.h"
#include "WebDashboard.h"
#include "WifiManager.h"

void setup() {
  Serial.begin(115200);

  settingsStoreBegin();
  fanControllerBegin();
  statusLedBegin();

  const bool accessPointStarted = wifiManagerBegin();
  webDashboardBegin();

  Serial.println();
  Serial.printf("MimiVent v%s ready\n", FIRMWARE_VERSION);
  Serial.printf("AP: %s / %s\n", AppConfig::kApSsid, AppConfig::kApPassword);
  Serial.printf("Dashboard: http://%s\n", wifiManagerGetIp().c_str());
  Serial.printf("AP status: %s\n", accessPointStarted ? "started" : "failed");
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
  // 先处理网页命令，再读取时钟。命令处理器可能刚设置 overrideStartedMs；
  // 若使用处理请求之前保存的 nowMs，unsigned 差值会下溢并让定时模式立即到期。
  webDashboardUpdate();

  const unsigned long nowMs = millis();
  fanControllerUpdate(nowMs);

  static unsigned long lastStatusLedUpdateMs = 0;
  if (nowMs - lastStatusLedUpdateMs >= 20UL) {
    lastStatusLedUpdateMs = nowMs;
    statusLedUpdate(fanControllerIsFanOn(), nowMs);
  }

  delay(1);  // 让出少量 CPU 时间，保持网页请求响应及时。
}
