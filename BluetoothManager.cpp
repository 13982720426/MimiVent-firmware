// 通过 BLE GATT 收发控制请求；回调只入队，状态变更留在主循环中执行。
#include "BluetoothManager.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "AppConfig.h"
#include "ControlApi.h"
#include "FirmwareVersion.h"
#include "SettingsStore.h"
#include "OtaManager.h"
#include "OtaBleAdapter.h"
#include "WifiProvisioningManager.h"

namespace {

constexpr size_t kRequestBufferSize = 384;
constexpr uint8_t kBleFrameVersion = 2;
constexpr size_t kFramePrefixSize = 4;
constexpr size_t kFirstFrameHeaderSize = 12;
constexpr uint16_t kMinimumAttMtu = 23;
// 先保留保守发送节奏；每包大小按实际协商 MTU 计算。
constexpr unsigned long kResponseChunkIntervalMs = 20UL;
constexpr size_t kWifiRequestBufferSize = 512;

NimBLEServer* bleServer = nullptr;
NimBLECharacteristic* responseCharacteristic = nullptr;
volatile bool clientConnected = false;
volatile bool responseBusy = false;
bool wasClientConnected = false;
portMUX_TYPE requestMux = portMUX_INITIALIZER_UNLOCKED;
char pendingRequest[kRequestBufferSize] = {};
uint16_t pendingRequestId = 0;
bool requestPending = false;
bool pendingRequestLegacy = false;
uint16_t activeRequestId = 0;
uint16_t peerMtu = kMinimumAttMtu;
String queuedResponse;
bool queuedResponseLegacy = false;
size_t queuedResponseOffset = 0;
unsigned long lastResponseChunkMs = 0;
unsigned long responseQueuedMs = 0;
unsigned long responseLastProgressMs = 0;
uint16_t responseNotifyFailures = 0;
uint16_t queuedResponseChunkCount = 0;
NimBLECharacteristic* wifiResponseCharacteristic = nullptr;
char pendingWifiRequest[kWifiRequestBufferSize] = {};
uint16_t pendingWifiRequestId = 0;
bool wifiRequestPending = false;
bool wifiRequestEncrypted = false;
char pendingWifiOwner[18]{};
volatile uint32_t wifiGeneration=0;
uint32_t pendingWifiGeneration=0,responseWifiGeneration=0;
bool wifiResponseBusy = false;
portMUX_TYPE wifiRequestMux = portMUX_INITIALIZER_UNLOCKED;
String queuedWifiResponse;
size_t queuedWifiResponseOffset = 0;
uint16_t queuedWifiResponseChunkCount = 0;
uint16_t activeWifiRequestId = 0;
unsigned long lastWifiResponseChunkMs = 0;
unsigned long wifiResponseQueuedMs = 0;
unsigned long wifiResponseLastProgressMs = 0;
uint16_t wifiResponseNotifyFailures = 0;

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
  request->body = body;
  if (request->path == "/api/stats") {
    request->eventOffset = formValue(body, "offset");
  }
  if (request->path == "/api/stats/range") {
    request->rangeFrom = formValue(body, "from");
    request->rangeTo = formValue(body, "to");
    request->rangeTz = formValue(body, "tzOffsetSeconds");
  }
  if (request->path == "/api/events") {
    request->eventStart = formValue(body, "start");
    request->eventEnd = formValue(body, "end");
    request->eventKind = formValue(body, "kind");
    request->eventOffset = formValue(body, "offset");
    request->eventBeforeId = formValue(body, "beforeId");
    request->eventDataEpoch = formValue(body, "dataEpoch");
    request->eventLimit = formValue(body, "limit");
  }

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

uint32_t crc32(const uint8_t* bytes, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= bytes[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

void writeU16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value & 0xFFU);
  destination[1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
  destination[2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

void queueResponse(const ControlApiResponse& response, uint16_t requestId,
                   bool legacy = false) {
  queuedResponse = String(response.statusCode) + '\n' + response.body + '\n';
  queuedResponseLegacy = legacy;
  activeRequestId = requestId;
  queuedResponseOffset = 0;
  queuedResponseChunkCount = 0;
  responseQueuedMs = millis();
  responseLastProgressMs = responseQueuedMs;
  responseNotifyFailures = 0;
  portENTER_CRITICAL(&requestMux);
  responseBusy = true;
  portEXIT_CRITICAL(&requestMux);
}

void sendNextResponseChunk(unsigned long nowMs) {
  if (!clientConnected || !queuedResponse.length()) {
    if (responseBusy || queuedResponse.length()) {
      queuedResponse = "";
      queuedResponseOffset = 0;
      responseNotifyFailures = 0;
      portENTER_CRITICAL(&requestMux);
      responseBusy = false;
      portEXIT_CRITICAL(&requestMux);
    }
    return;
  }
  if (queuedResponseOffset && nowMs - lastResponseChunkMs < kResponseChunkIntervalMs) return;

  if (queuedResponseLegacy) {
    const size_t mtuPayload = peerMtu > 3 ? peerMtu - 3 : kMinimumAttMtu - 3;
    const size_t remaining = queuedResponse.length() - queuedResponseOffset;
    const size_t payloadLength = remaining < mtuPayload ? remaining : mtuPayload;
    if (!responseCharacteristic->notify(
            reinterpret_cast<const uint8_t*>(queuedResponse.c_str() + queuedResponseOffset),
            payloadLength)) {
      ++responseNotifyFailures;
      if (nowMs - responseLastProgressMs > 2000UL || responseNotifyFailures >= 20) {
        Serial.printf("BLE notification failed, dropping response request=%u failures=%u\n",
                      activeRequestId, responseNotifyFailures);
        queuedResponse = "";
        queuedResponseOffset = 0;
        queuedResponseChunkCount = 0;
        responseNotifyFailures = 0;
        portENTER_CRITICAL(&requestMux);
        responseBusy = false;
        portEXIT_CRITICAL(&requestMux);
      }
      return;
    }
    queuedResponseOffset += payloadLength;
    lastResponseChunkMs = nowMs;
    responseLastProgressMs = nowMs;
    responseNotifyFailures = 0;
    if (queuedResponseOffset >= queuedResponse.length()) {
      queuedResponse = "";
      queuedResponseOffset = 0;
      queuedResponseLegacy = false;
      portENTER_CRITICAL(&requestMux);
      responseBusy = false;
      portEXIT_CRITICAL(&requestMux);
    }
    return;
  }

  const bool firstFragment = queuedResponseOffset == 0;
  const size_t mtuPayload = peerMtu > 3 ? peerMtu - 3 : kMinimumAttMtu - 3;
  const size_t headerSize = firstFragment ? kFirstFrameHeaderSize : kFramePrefixSize;
  if (mtuPayload <= headerSize || queuedResponse.length() > 0xFFFFU) {
    Serial.println("BLE response framing error: invalid MTU or response length");
    queuedResponse = "";
    portENTER_CRITICAL(&requestMux);
    responseBusy = false;
    portEXIT_CRITICAL(&requestMux);
    return;
  }
  const size_t availablePayload = mtuPayload - headerSize;
  const size_t remainingPayload = queuedResponse.length() - queuedResponseOffset;
  const size_t payloadLength = availablePayload < remainingPayload
      ? availablePayload
      : remainingPayload;
  uint8_t frame[517] = {};
  writeU16(frame, activeRequestId);
  writeU16(frame + 2, queuedResponseChunkCount);
  size_t frameLength = kFramePrefixSize;
  if (firstFragment) {
    frame[4] = kBleFrameVersion;
    frame[5] = 0;
    writeU16(frame + 6, static_cast<uint16_t>(queuedResponse.length()));
    writeU32(frame + 8, crc32(reinterpret_cast<const uint8_t*>(queuedResponse.c_str()),
                              queuedResponse.length()));
    frameLength = kFirstFrameHeaderSize;
  }
  memcpy(frame + frameLength, queuedResponse.c_str() + queuedResponseOffset, payloadLength);
  frameLength += payloadLength;
  if (!responseCharacteristic->notify(frame, frameLength)) {
    ++responseNotifyFailures;
    if (nowMs - responseLastProgressMs > 2000UL || responseNotifyFailures >= 20) {
      Serial.printf("BLE notification failed, dropping response request=%u failures=%u\n",
                    activeRequestId, responseNotifyFailures);
      queuedResponse = "";
      queuedResponseOffset = 0;
      queuedResponseChunkCount = 0;
      responseNotifyFailures = 0;
      portENTER_CRITICAL(&requestMux);
      responseBusy = false;
      portEXIT_CRITICAL(&requestMux);
    }
    return;
  }
  queuedResponseOffset += payloadLength;
  ++queuedResponseChunkCount;
  lastResponseChunkMs = nowMs;
  responseLastProgressMs = nowMs;
  responseNotifyFailures = 0;
  if (queuedResponseOffset >= queuedResponse.length()) {
    Serial.printf("BLE response sent: request=%u, fragments=%u, mtu=%u, bytes=%u\n",
                  activeRequestId, queuedResponseChunkCount, peerMtu,
                  static_cast<unsigned>(queuedResponse.length()));
    queuedResponse = "";
    queuedResponseOffset = 0;
    portENTER_CRITICAL(&requestMux);
    responseBusy = false;
    portEXIT_CRITICAL(&requestMux);
  }
}

void queueWifiResponse(uint16_t requestId, uint16_t statusCode,
                       const String& body) {
  queuedWifiResponse = String(statusCode) + '\n' + body + '\n';
  queuedWifiResponseOffset = 0;
  queuedWifiResponseChunkCount = 0;
  activeWifiRequestId = requestId;
  wifiResponseQueuedMs = millis();
  wifiResponseLastProgressMs = wifiResponseQueuedMs;
  wifiResponseNotifyFailures = 0;
  portENTER_CRITICAL(&wifiRequestMux);
  wifiResponseBusy = true;
  portEXIT_CRITICAL(&wifiRequestMux);
}

void sendNextWifiResponseChunk(unsigned long nowMs) {
  if (!clientConnected || !wifiResponseCharacteristic ||
      !queuedWifiResponse.length()) {
    if (wifiResponseBusy || queuedWifiResponse.length()) {
      queuedWifiResponse = "";
      queuedWifiResponseOffset = 0;
      wifiResponseNotifyFailures = 0;
      portENTER_CRITICAL(&wifiRequestMux);
      wifiResponseBusy = false;
      portEXIT_CRITICAL(&wifiRequestMux);
    }
    return;
  }
  if (queuedWifiResponseOffset &&
      nowMs - lastWifiResponseChunkMs < kResponseChunkIntervalMs) return;
  const size_t mtuPayload = peerMtu > 3 ? peerMtu - 3 : kMinimumAttMtu - 3;
  const bool first = queuedWifiResponseOffset == 0;
  const size_t headerSize = first ? kFirstFrameHeaderSize : kFramePrefixSize;
  if (mtuPayload <= headerSize || queuedWifiResponse.length() > 0xFFFFU) {
    queuedWifiResponse = "";
    return;
  }
  const size_t available = mtuPayload - headerSize;
  const size_t remaining = queuedWifiResponse.length() - queuedWifiResponseOffset;
  const size_t payloadLength = std::min(available, remaining);
  uint8_t frame[517] = {};
  writeU16(frame, activeWifiRequestId);
  writeU16(frame + 2, queuedWifiResponseChunkCount);
  size_t frameLength = kFramePrefixSize;
  if (first) {
    frame[4] = kBleFrameVersion;
    frame[5] = 0;
    writeU16(frame + 6, static_cast<uint16_t>(queuedWifiResponse.length()));
    writeU32(frame + 8,
             crc32(reinterpret_cast<const uint8_t*>(queuedWifiResponse.c_str()),
                   queuedWifiResponse.length()));
    frameLength = kFirstFrameHeaderSize;
  }
  memcpy(frame + frameLength,
         queuedWifiResponse.c_str() + queuedWifiResponseOffset, payloadLength);
  frameLength += payloadLength;
  if (!wifiResponseCharacteristic->notify(frame, frameLength)) {
    ++wifiResponseNotifyFailures;
    if (nowMs - wifiResponseLastProgressMs > 2000UL || wifiResponseNotifyFailures >= 20) {
      Serial.printf("WiFi BLE notification failed, dropping response request=%u failures=%u\n",
                    activeWifiRequestId, wifiResponseNotifyFailures);
      queuedWifiResponse = "";
      queuedWifiResponseOffset = 0;
      queuedWifiResponseChunkCount = 0;
      wifiResponseNotifyFailures = 0;
      portENTER_CRITICAL(&wifiRequestMux);
      wifiResponseBusy = false;
      portEXIT_CRITICAL(&wifiRequestMux);
    }
    return;
  }
  queuedWifiResponseOffset += payloadLength;
  ++queuedWifiResponseChunkCount;
  lastWifiResponseChunkMs = nowMs;
  wifiResponseLastProgressMs = nowMs;
  wifiResponseNotifyFailures = 0;
  if (queuedWifiResponseOffset >= queuedWifiResponse.length()) {
    queuedWifiResponse = "";
    queuedWifiResponseOffset = 0;
    queuedWifiResponseChunkCount = 0;
    portENTER_CRITICAL(&wifiRequestMux);
    wifiResponseBusy = false;
    portEXIT_CRITICAL(&wifiRequestMux);
  }
}

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo& info) override {
    portENTER_CRITICAL(&wifiRequestMux);++wifiGeneration;wifiRequestPending=false;portEXIT_CRITICAL(&wifiRequestMux);
    clientConnected = true;
    peerMtu = kMinimumAttMtu;
    OtaBleAdapter::setNegotiatedMtu(peerMtu);
    otaManagerOnConnect();
    Serial.println("BLE client connected");
#if defined(MIMIVENT_BLE_PAIRING_PROBE) && MIMIVENT_BLE_PAIRING_PROBE
    // 仅电脑配对诊断构建主动协商；普通连接不因此强制配对。
    int result = 0;
    const bool started = NimBLEDevice::startSecurity(info.getConnHandle(), &result);
    Serial.printf("[ble-pairing-probe] requested=%d result=%d\n", started, result);
#endif
  }
  void onAuthenticationComplete(NimBLEConnInfo& info) override {
    Serial.printf("BLE security complete; encrypted=%d bonded=%d authenticated=%d\n",
                  info.isEncrypted(), info.isBonded(), info.isAuthenticated());
  }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo& info, int reason) override {
    portENTER_CRITICAL(&wifiRequestMux);++wifiGeneration;wifiRequestPending=false;portEXIT_CRITICAL(&wifiRequestMux);
    clientConnected = false;
    otaManagerOnDisconnect(info.getConnHandle());
    Serial.printf("BLE client disconnected; reason=0x%02X\n", reason);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo&) override {
    peerMtu = mtu < kMinimumAttMtu ? kMinimumAttMtu : mtu;
    OtaBleAdapter::setNegotiatedMtu(peerMtu);
    Serial.printf("BLE MTU negotiated: %u\n", peerMtu);
  }
};

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    const NimBLEAttValue incoming = characteristic->getValue();
    if (incoming.size() < 3) return;
    const uint8_t* bytes = incoming.data();
    const bool v2 = incoming.size() >= 6 &&
        ((bytes[2] == 'G' && bytes[3] == 'E' && bytes[4] == 'T' && bytes[5] == '\n') ||
         (incoming.size() >= 7 && bytes[2] == 'P' && bytes[3] == 'O' &&
          bytes[4] == 'S' && bytes[5] == 'T' && bytes[6] == '\n'));
    const bool legacy = !v2 &&
        ((bytes[0] == 'G' && bytes[1] == 'E' && bytes[2] == 'T') ||
         (incoming.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'O' &&
          bytes[2] == 'S' && bytes[3] == 'T'));
    const size_t prefixLength = legacy ? 0 : 2;
    if (incoming.size() - prefixLength >= kRequestBufferSize) return;
    if (incoming.size() <= prefixLength) return;
    portENTER_CRITICAL(&requestMux);
    if (requestPending || responseBusy) {
      portEXIT_CRITICAL(&requestMux);
      Serial.println("BLE command rejected: request queue is busy");
      return;
    }
    pendingRequestId = legacy ? 0 : static_cast<uint16_t>(bytes[0]) |
                       (static_cast<uint16_t>(bytes[1]) << 8U);
    const size_t requestLength = incoming.size() - prefixLength;
    memcpy(pendingRequest, bytes + prefixLength, requestLength);
    pendingRequest[requestLength] = '\0';
    pendingRequestLegacy = legacy;
    requestPending = true;
    portEXIT_CRITICAL(&requestMux);
  }
};

class WifiCommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    const NimBLEAttValue incoming = characteristic->getValue();
    if (!info.isEncrypted() || !info.isBonded() || incoming.size() < 3 ||
        incoming.size() >= kWifiRequestBufferSize) return;
    const uint8_t* bytes = incoming.data();
    const size_t requestLength = incoming.size() - 2;
    String owner=info.getIdAddress().toString().c_str();
    owner.replace(":","");owner.toLowerCase();
    // 区分 public/random 身份地址；同一 MAC 的不同类型不是同一个 bond。
    owner=String(info.getIdAddress().getType()&1)+"-"+owner;
    portENTER_CRITICAL(&wifiRequestMux);
    if (!wifiRequestPending && !wifiResponseBusy) {
      pendingWifiRequestId = static_cast<uint16_t>(bytes[0]) |
          (static_cast<uint16_t>(bytes[1]) << 8U);
      memcpy(pendingWifiRequest, bytes + 2, requestLength);
      pendingWifiRequest[requestLength] = '\0';
      wifiRequestEncrypted = true;
      strncpy(pendingWifiOwner,owner.c_str(),sizeof(pendingWifiOwner)-1);
      pendingWifiGeneration=wifiGeneration;
      wifiRequestPending = true;
    }
    portEXIT_CRITICAL(&wifiRequestMux);
  }
};

}  // namespace

void bluetoothManagerBegin() {
  NimBLEDevice::init(AppConfig::kBleDeviceName);
  NimBLEDevice::setMTU(247);
  // OTA 属性要求已配对并加密；控制连接仍可先保持未加密以供发现与配对。
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
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
  responseCharacteristic->setValue("MimiVent BLE API v2");
  OtaBleAdapter::begin();
  OtaBleAdapter::registerService(service);
  service->start();

  NimBLEService* wifiService = nullptr;
#if MIMIVENT_ENABLE_WIFI
  wifiService = bleServer->createService(AppConfig::kBleWifiServiceUuid);
  NimBLECharacteristic* wifiCommand = wifiService->createCharacteristic(
      AppConfig::kBleWifiCommandUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  wifiCommand->setCallbacks(new WifiCommandCallbacks());
  wifiResponseCharacteristic = wifiService->createCharacteristic(
      AppConfig::kBleWifiResponseUuid,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY,
      1024);
  wifiResponseCharacteristic->setValue("{}");
  wifiService->start();
  wifiProvisioningBegin();
#endif

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advertisementData;
  advertisementData.setFlags(0x06);
  advertisementData.addServiceUUID(AppConfig::kBleServiceUuid);
  NimBLEAdvertisementData scanResponseData;
#if MIMIVENT_ENABLE_WIFI
  const uint8_t gattVersion = 2;
  scanResponseData.setServiceData(
      NimBLEUUID(AppConfig::kBleServiceUuid), &gattVersion, sizeof(gattVersion));
#else
  const uint8_t gattVersion = 1;
  scanResponseData.setServiceData(
      NimBLEUUID(AppConfig::kBleServiceUuid), &gattVersion, sizeof(gattVersion));
#endif
  scanResponseData.setName(AppConfig::kBleDeviceName);
  if (!advertising->setAdvertisementData(advertisementData) ||
      !advertising->setScanResponseData(scanResponseData)) {
    Serial.println("BLE advertising payload exceeds the 31-byte legacy limit");
  }
  advertising->start();
  Serial.printf("BLE control ready: %s\n", AppConfig::kBleDeviceName);
}

void bluetoothManagerUpdate() {
  otaManagerUpdate();
#if MIMIVENT_ENABLE_WIFI
  wifiProvisioningUpdate();
#endif
  if (!clientConnected && wasClientConnected) {
    bleServer->startAdvertising();
    wasClientConnected = false;
    queuedResponse = "";
    queuedResponseOffset = 0;
    portENTER_CRITICAL(&requestMux);
    responseBusy = false;
    portEXIT_CRITICAL(&requestMux);
  } else if (clientConnected && !wasClientConnected) {
    wasClientConnected = true;
  }

  char localRequest[kRequestBufferSize] = {};
  uint16_t localRequestId = 0;
  bool hasRequest = false;
  bool legacyRequest = false;
  portENTER_CRITICAL(&requestMux);
  if (requestPending) {
    memcpy(localRequest, pendingRequest, sizeof(localRequest));
    localRequestId = pendingRequestId;
    legacyRequest = pendingRequestLegacy;
    pendingRequest[0] = '\0';
    requestPending = false;
    pendingRequestLegacy = false;
    hasRequest = true;
  }
  portEXIT_CRITICAL(&requestMux);
  if (hasRequest && clientConnected) {
    ControlApiRequest request;
    ControlApiResponse result{};
    if (legacyRequest) {
      result = {426, "application/json; charset=utf-8",
                String("{\"error\":\"APP_UPDATE_REQUIRED\",\"minimumAppVersion\":\"") +
                    FIRMWARE_MINIMUM_APP_VERSION + "\"}"};
    } else if (!parsePacket(String(localRequest), &request)) {
      result = {400, "application/json; charset=utf-8", "{\"error\":\"invalid BLE request\"}"};
    } else if (request.path == "/api/config") {
      // BLE 广播控制不传送 Wi-Fi 密码；账号凭证只在本地 AP 页面内显示。
      result = {403, "application/json; charset=utf-8", "{\"error\":\"Wi-Fi credentials are hidden over BLE\"}"};
    } else if (otaManagerIsActive()) {
      result = {423, "application/json; charset=utf-8", "{\"error\":\"firmware update in progress\"}"};
    } else {
      result = controlApiDispatch(request);
    }
    queueResponse(result, localRequestId, legacyRequest);
  }
#if MIMIVENT_ENABLE_WIFI
  char localWifiRequest[kWifiRequestBufferSize] = {};
  char localWifiOwner[18]{};uint32_t requestGeneration=0;
  uint16_t localWifiRequestId = 0;
  bool hasWifiRequest = false;
  bool wifiRequestWasEncrypted = false;
  portENTER_CRITICAL(&wifiRequestMux);
  if (wifiRequestPending) {
    memcpy(localWifiRequest, pendingWifiRequest, sizeof(localWifiRequest));
    localWifiRequestId = pendingWifiRequestId;
    wifiRequestWasEncrypted = wifiRequestEncrypted;
    memcpy(localWifiOwner,pendingWifiOwner,sizeof(localWifiOwner));requestGeneration=pendingWifiGeneration;
    pendingWifiRequest[0] = '\0';
    wifiRequestPending = false;
    wifiRequestEncrypted = false;
    hasWifiRequest = true;
  }
  portEXIT_CRITICAL(&wifiRequestMux);
  if (hasWifiRequest && clientConnected && requestGeneration==wifiGeneration) {
    const String body = wifiRequestWasEncrypted
        ? wifiProvisioningHandleRequest(String(localWifiRequest),String(localWifiOwner))
        : String("{\"error\":\"PAIRING_REQUIRED\"}");
    queueWifiResponse(localWifiRequestId, 200, body);
    responseWifiGeneration=requestGeneration;
  }
  if(responseWifiGeneration!=wifiGeneration){
    queuedWifiResponse="";queuedWifiResponseOffset=0;
    portENTER_CRITICAL(&wifiRequestMux);wifiResponseBusy=false;portEXIT_CRITICAL(&wifiRequestMux);
  }
  sendNextWifiResponseChunk(millis());
#endif
  sendNextResponseChunk(millis());
}
