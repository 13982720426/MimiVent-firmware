#include "OtaBleAdapter.h"

#include <NimBLECharacteristic.h>
#include <NimBLEDevice.h>

#include "AppConfig.h"
#include "OtaManager.h"

namespace OtaBleAdapter {
namespace {
constexpr size_t kDataQueueDepth = 12;
constexpr size_t kControlQueueDepth = 8;
constexpr size_t kCallbackStatusLength = 96;
QueueHandle_t controlQueue = nullptr;
QueueHandle_t dataQueue = nullptr;
NimBLECharacteristic* statusCharacteristic = nullptr;
portMUX_TYPE callbackStatusMux = portMUX_INITIALIZER_UNLOCKED;
struct PendingStatus {
  char text[kCallbackStatusLength];
  uint16_t connection;
  bool pending;
};
PendingStatus pendingCriticalStatus{};
PendingStatus pendingFlowStatus{};
volatile uint16_t negotiatedMtu = 23;

bool authorizeSecureConnection(NimBLEConnInfo& info) {
  if (!info.isEncrypted() || !info.isBonded()) {
    publishStatusFromCallback("ERROR|PAIRING_REQUIRED", info.getConnHandle(), true);
    return false;
  }
  otaManagerOnSecureSession(info.getConnHandle());
  return true;
}

class ControlCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!authorizeSecureConnection(info)) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (!controlQueue) return;
    if (value.size() > kControlBytes) {
      publishStatusFromCallback("ERROR|CONTROL_TOO_LONG", info.getConnHandle(), true);
      return;
    }
    Message message{};
    message.type = MessageType::CONTROL;
    message.connection = info.getConnHandle();
    message.length = value.size();
    memcpy(message.bytes, value.data(), value.size());
    if (xQueueSend(controlQueue, &message, 0) != pdTRUE) {
      Serial.println("OTA control queue full");
      publishStatusFromCallback("FLOW|CONTROL_QUEUE_FULL", info.getConnHandle(), false);
    }
  }
};

class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& info) override {
    if (!authorizeSecureConnection(info)) return;
    const NimBLEAttValue value = characteristic->getValue();
    if (!dataQueue || value.size() < 9 || value.size() > kMaxChunkBytes) return;
    Message message{};
    message.type = MessageType::DATA;
    message.connection = info.getConnHandle();
    message.length = value.size();
    memcpy(message.bytes, value.data(), value.size());
    if (xQueueSend(dataQueue, &message, 0) != pdTRUE) {
      Serial.println("OTA flow control: data queue full");
      publishStatusFromCallback("FLOW|QUEUE_FULL", info.getConnHandle(), false);
    }
  }
};
}  // namespace

void begin() {
  controlQueue = xQueueCreate(kControlQueueDepth, sizeof(Message));
  dataQueue = xQueueCreate(kDataQueueDepth, sizeof(Message));
}

void registerService(NimBLEService* service) {
  auto* control = service->createCharacteristic(
      AppConfig::kBleOtaControlUuid,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  control->setCallbacks(new ControlCallbacks());
  auto* data = service->createCharacteristic(
      AppConfig::kBleOtaDataUuid,
      NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::WRITE_NR);
  data->setCallbacks(new DataCallbacks());
  statusCharacteristic = service->createCharacteristic(
      AppConfig::kBleOtaStatusUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusCharacteristic->setValue("LOCKED");
}

bool receiveControl(Message* message) {
  return controlQueue && xQueueReceive(controlQueue, message, 0) == pdTRUE;
}

bool receiveData(Message* message) {
  return dataQueue && xQueueReceive(dataQueue, message, 0) == pdTRUE;
}

UBaseType_t availableDataSlots() {
  return dataQueue ? uxQueueSpacesAvailable(dataQueue) : 0;
}

void publishStatus(const String& status, uint16_t connection) {
  if (!statusCharacteristic) return;
  // 可读值始终保留完整状态；MTU 不足时只通知短事件，App 再读取完整值。
  statusCharacteristic->setValue(status.c_str());
  const size_t maxNotifyBytes = negotiatedMtu > 3 ? negotiatedMtu - 3 : 20;
  if (status.length() <= maxNotifyBytes) {
    statusCharacteristic->notify(connection);
    return;
  }
  const int separator = status.indexOf('|');
  const String phase = separator < 0 ? status : status.substring(0, separator);
  const String event = String("EVT|") + phase;
  if (event.length() <= maxNotifyBytes) {
    statusCharacteristic->notify(
        reinterpret_cast<const uint8_t*>(event.c_str()), event.length(), connection);
  }
}

void publishStatusFromCallback(const char* status, uint16_t connection,
                               bool critical) {
  if (!status) return;
  PendingStatus* slot = critical ? &pendingCriticalStatus : &pendingFlowStatus;
  portENTER_CRITICAL(&callbackStatusMux);
  snprintf(slot->text, sizeof(slot->text), "%s", status);
  slot->connection = connection;
  slot->pending = true;
  portEXIT_CRITICAL(&callbackStatusMux);
}

void drainCallbackStatuses() {
  PendingStatus critical{};
  PendingStatus flow{};
  portENTER_CRITICAL(&callbackStatusMux);
  critical = pendingCriticalStatus;
  flow = pendingFlowStatus;
  pendingCriticalStatus.pending = false;
  pendingFlowStatus.pending = false;
  portEXIT_CRITICAL(&callbackStatusMux);
  if (critical.pending) publishStatus(String(critical.text), critical.connection);
  if (flow.pending && otaManagerIsActive() &&
      otaManagerIsOwnerConnection(flow.connection)) {
    publishStatus(String(flow.text), flow.connection);
  }
}

void clearPendingCallbackStatuses() {
  portENTER_CRITICAL(&callbackStatusMux);
  pendingCriticalStatus.pending = false;
  pendingFlowStatus.pending = false;
  portEXIT_CRITICAL(&callbackStatusMux);
}

void setReadableStatus(const String& status) {
  if (statusCharacteristic) statusCharacteristic->setValue(status.c_str());
}

void setNegotiatedMtu(uint16_t mtu) {
  negotiatedMtu = mtu < 23 ? 23 : mtu;
}
}  // namespace OtaBleAdapter
