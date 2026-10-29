#pragma once

#include <Arduino.h>
#include <NimBLEService.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace OtaBleAdapter {
constexpr size_t kMaxChunkBytes = 512;
constexpr size_t kControlBytes = 320;

enum class MessageType : uint8_t { CONTROL, DATA };
struct Message {
  MessageType type;
  uint16_t connection;
  uint16_t length;
  uint8_t bytes[kMaxChunkBytes];
};
static_assert(kControlBytes <= sizeof(Message::bytes),
              "OTA control message exceeds its backing buffer");

void begin();
void registerService(NimBLEService* service);
bool receiveControl(Message* message);
bool receiveData(Message* message);
UBaseType_t availableDataSlots();
void publishStatus(const String& status, uint16_t connection);
void publishStatusFromCallback(const char* status, uint16_t connection,
                               bool critical);
void drainCallbackStatuses();
void clearPendingCallbackStatuses();
void setReadableStatus(const String& status);
void setNegotiatedMtu(uint16_t mtu);
}  // namespace OtaBleAdapter
