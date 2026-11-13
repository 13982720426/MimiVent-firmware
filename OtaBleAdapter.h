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

// 控制特征的入站包上限是 kControlBytes，但队列原本按 sizeof(Message)=518 B
// 分配，每格多占约 192 B。控制队列 8 格就是 1.5 KB 常驻浪费。
// 专用结构只影响队列存储；对上层仍通过 receiveControl(Message*) 交付，
// 由接收端按 length 拷贝到调用方的 Message 里，接口与行为不变。
struct ControlMessage {
  MessageType type;
  uint16_t connection;
  uint16_t length;
  uint8_t bytes[kControlBytes];
};
static_assert(kControlBytes <= sizeof(Message::bytes),
              "control payload must fit the shared message buffer");
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
