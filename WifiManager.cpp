// 集中管理 ESP32 SoftAP 初始化；热点参数统一读取 AppConfig。
#include "WifiManager.h"

#include <WiFi.h>

#include "AppConfig.h"

bool wifiManagerBegin() {
  const IPAddress ip(AppConfig::kApAddress[0], AppConfig::kApAddress[1], AppConfig::kApAddress[2],
                     AppConfig::kApAddress[3]);
  const IPAddress subnet(AppConfig::kApSubnet[0], AppConfig::kApSubnet[1], AppConfig::kApSubnet[2],
                         AppConfig::kApSubnet[3]);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(ip, ip, subnet);
  return WiFi.softAP(AppConfig::kApSsid, AppConfig::kApPassword);
}

String wifiManagerGetIp() { return WiFi.softAPIP().toString(); }
