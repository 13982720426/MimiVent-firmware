#include <cassert>
#include <cstring>

#include "../../WifiCapabilities.h"

int main() {
  assert(std::strcmp(wifiCapabilitiesJson(false, false),
                     "[\"ble.control\",\"ble.stats\",\"ble.ota\"]") == 0);
  assert(std::strcmp(wifiCapabilitiesJson(true, false),
                     "[\"ble.control\",\"ble.stats\",\"ble.ota\",\"wifi.configure\"]") == 0);
  const char* mqtt = wifiCapabilitiesJson(true, true);
  assert(std::strstr(mqtt, "wifi.bind") != nullptr);
  assert(std::strstr(mqtt, "wifi.mqtt.control") != nullptr);
  assert(std::strstr(mqtt, "wifi.mqtt.query") != nullptr);
  assert(std::strstr(mqtt, "wifi.time-sync") != nullptr);
  assert(std::strstr(mqtt, "wifi.read") == nullptr);
  return 0;
}
