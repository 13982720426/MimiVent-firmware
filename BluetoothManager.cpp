// 通过 BLE GATT 收发控制请求；回调只入队，状态变更留在主循环中执行。
#include "BluetoothManager.h"

#include <NimBLEDevice.h>

#include <cstring>

#include "AppConfig.h"
#include "ControlApi.h"
#include "SettingsStore.h"

namespace {

constexpr size_t kRequestBufferSize = 384;
constexpr size_t kResponseChunkSize = 18;  // 保持兼容默认 23 字节 ATT MTU。
constexpr unsigned long kResponseChunkIntervalMs = 8UL;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* responseCharacteristic = nullptr;
volatile bool clientConnected = false;
bool wasClientConnected = false;
portMUX_TYPE requestMux = portMUX_INITIALIZER_UNLOCKED;
char pendingRequest[kRequestBufferSize] = {};
bool requestPending = false;
String queuedResponse;
size_t queuedResponseOffset = 0;
unsigned long lastResponseChunkMs = 0;

String fieldAt(const String& packet, uint8_t wantedIndex, uint8_t* nextOffset) {
  uint8_t start = 0;
  for (uint8_t index = 0; index < wantedIndex; ++index) {
    const int separator = packet.indexOf('\n', start);
    if (separator < 0) return String();
    start = static_cast<uint8_t>(separator + 1);
  }
  const int separator = packet.indexOf('\n', start);
  if (separator < 0) return String();
  *nextOffset = static_cast<uint8_t>(separator + 1);
  return packet.substring(start, separator);
}

String formValue(const String& body, const char* key) {
  const String prefix = String(key) + "=";
  int start = 0;
  while (start < body.length()) {
    const int end = body.indexOf('&', start);
    const String item = end < 0 ? body.substring(start) : body.substring(start, end);
    if (item.startsWith(prefix)) return item.substring(prefix.length());
    if (end < 0) break;
    start = end + 1;
  }
  return String();
}

bool parsePacket(const String& packet, ControlApiRequest* request) {
  uint8_t offset = 0;
  request->method = fieldAt(packet, 0, &offset);
  request->path = fieldAt(packet, 1, &offset);
  const String profile = fieldAt(packet, 2, &offset);
  request->mode = fieldAt(packet, 3, &offset);
  request->stage = fieldAt(packet, 4, &offset);
  request->replace = fieldAt(packet, 5, &offset) == "1";
  request->switchProfile = fieldAt(packet, 6, &offset) == "1";
  const String body = packet.substring(offset);

  if (request->method != "GET" && request->method != "POST") return false;
  if (!settingsStoreParseProfile(profile, &request->profile)) return false;
  if (request->path == "/api/settings" && request->method == "POST") {
    const String startup = formValue(body, "startupCooldownMs");
    const String leave = formValue(body, "leaveConfirmMs");
    const String automatic = formValue(body, "autoExhaustMs");
    const String manual = formValue(body, "manualExhaustMs");
    const String pause = formValue(body, "pauseMs");
    TimingSettings parsedSettings{};
    if (!controlApiParseTimeValue(startup, false, &parsedSettings.startupCooldownMs) ||
        !controlApiParseTimeValue(leave, true, &parsedSettings.leaveConfirmMs) ||
        !controlApiParseTimeValue(automatic, false, &parsedSettings.autoExhaustMs) ||
        !controlApiParseTimeValue(manual, false, &parsedSettings.manualExhaustMs) ||
        !controlApiParseTimeValue(pause, false, &parsedSettings.pauseMs)) {
      return false;
    }
    request->settings = parsedSettings;
    request->hasSettings = true;
  }
  return true;
}

void queueResponse(const ControlApiResponse& response) {
  queuedResponse = String(response.statusCode) + '\n' + response.body + '\n';
  queuedResponseOffset = 0;
}

void sendNextResponseChunk(unsigned long nowMs) {
  if (!clientConnected || !queuedResponse.length()) return;
  if (queuedResponseOffset && nowMs - lastResponseChunkMs < kResponseChunkIntervalMs) return;

  const String chunk = queuedResponse.substring(queuedResponseOffset,
                                                queuedResponseOffset + kResponseChunkSize);
  responseCharacteristic->setValue(chunk.c_str());
  if (!responseCharacteristic->notify()) return;
  queuedResponseOffset += chunk.length();
  lastResponseChunkMs = nowMs;
  if (queuedResponseOffset >= queuedResponse.length()) {
    queuedResponse = "";
    queuedResponseOffset = 0;
  }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { clientConnected = true; }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override { clientConnected = false; }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const String incoming = characteristic->getValue();
    if (!incoming.length() || incoming.length() >= kRequestBufferSize) return;
    portENTER_CRITICAL(&requestMux);
    memcpy(pendingRequest, incoming.c_str(), incoming.length());
    pendingRequest[incoming.length()] = '\0';
    requestPending = true;
    portEXIT_CRITICAL(&requestMux);
  }
};

}  // namespace

void bluetoothManagerBegin() {
  NimBLEDevice::init(AppConfig::kBleDeviceName);
  NimBLEDevice::setMTU(185);
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new ServerCallbacks());

  NimBLEService* service = bleServer->createService(AppConfig::kBleServiceUuid);
  NimBLECharacteristic* command = service->createCharacteristic(
      AppConfig::kBleCommandUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  command->setCallbacks(new CommandCallbacks());

  responseCharacteristic = service->createCharacteristic(
      AppConfig::kBleResponseUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  responseCharacteristic->setValue("MimiVent BLE API v1");
  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  // 主广播包含服务 UUID；名称放入扫描响应，避免首次发现时只显示 BLE 地址。
  advertising->enableScanResponse(true);
  advertising->setName(AppConfig::kBleDeviceName);
  advertising->addServiceUUID(AppConfig::kBleServiceUuid);
  advertising->start();
  Serial.printf("BLE control ready: %s\n", AppConfig::kBleDeviceName);
}

void bluetoothManagerUpdate() {
  if (!clientConnected && wasClientConnected) {
    bleServer->startAdvertising();
    wasClientConnected = false;
    queuedResponse = "";
    queuedResponseOffset = 0;
  } else if (clientConnected && !wasClientConnected) {
    wasClientConnected = true;
  }

  char localRequest[kRequestBufferSize] = {};
  bool hasRequest = false;
  portENTER_CRITICAL(&requestMux);
  if (requestPending) {
    memcpy(localRequest, pendingRequest, sizeof(localRequest));
    pendingRequest[0] = '\0';
    requestPending = false;
    hasRequest = true;
  }
  portEXIT_CRITICAL(&requestMux);
  if (hasRequest && clientConnected) {
    ControlApiRequest request;
    ControlApiResponse result{};
    if (!parsePacket(String(localRequest), &request)) {
      result = {400, "application/json; charset=utf-8", "{\"error\":\"invalid BLE request\"}"};
    } else if (request.path == "/api/config") {
      // BLE 广播控制不传送 Wi-Fi 密码；账号凭证只在本地 AP 页面内显示。
      result = {403, "application/json; charset=utf-8", "{\"error\":\"Wi-Fi credentials are hidden over BLE\"}"};
    } else {
      result = controlApiDispatch(request);
    }
    queueResponse(result);
  }
  sendNextResponseChunk(millis());
}
