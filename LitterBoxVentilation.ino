/*
  项目：LitterBoxVentilation
  用途：ESP32-C3-DevKitM-1 单 PIR 自动排风

  硬件连接：
  PIR OUT       -> GPIO4（检测到移动目标时通常输出 HIGH）
  Relay IN      -> GPIO5
  板载 WS2812 灯 -> GPIO8
  继电器触点控制 12V 风机供电
*/

#include <Adafruit_NeoPixel.h>
#include "StatusLed.h"

// 外设 GPIO 定义：如实际接线不同，修改这里的引脚号。
constexpr uint8_t PIR_PIN = 4;
constexpr uint8_t RELAY_PIN = 5;
// 继电器输入有效电平配置：
// true  = 低电平触发，GPIO 输出 LOW 时继电器吸合。
// false = 高电平触发，GPIO 输出 HIGH 时继电器吸合。
// 如果继电器动作和风机状态相反，先核对模块触发方式，再修改此值。
constexpr bool RELAY_ACTIVE_LOW = true;

// PIR 不再检测到移动后，风机继续运行的时间。
// 当前设为 10 秒便于调试；稳定后可改为 3–5 分钟。
constexpr unsigned long EXHAUST_DELAY_MS = 10UL * 1000UL;

// 当前风机逻辑状态。用于避免每轮 loop 都重复写继电器并重复打印日志。
bool fanOn = false;

// 最近一次检测到移动的 millis() 时间戳，用于计算延时排风时长。
unsigned long lastPresenceMs = 0;

// 返回继电器吸合时 GPIO 应输出的电平。
uint8_t relayOnLevel() {
  return RELAY_ACTIVE_LOW ? LOW : HIGH;
}

// 返回继电器释放时 GPIO 应输出的电平。
uint8_t relayOffLevel() {
  return RELAY_ACTIVE_LOW ? HIGH : LOW;
}

// 设置风机逻辑状态，并在状态变化时更新继电器和串口日志。
void setFan(bool turnOn) {
  // 状态没变化就不重复操作。
  if (fanOn == turnOn) {
    return;
  }

  fanOn = turnOn;
  // 根据继电器模块的有效电平，输出吸合或释放电平。
  digitalWrite(RELAY_PIN, fanOn ? relayOnLevel() : relayOffLevel());
  Serial.println(fanOn ? "Fan: ON" : "Fan: OFF");
}

void setup() {
  // 调试串口使用 115200 波特率。
  Serial.begin(115200);

  // 初始化板载 RGB 状态灯，并闪烁三次绿色作为启动提示。
  statusLedBegin();

  // 先预置继电器关闭电平，再切换为输出，尽量避免上电瞬间误吸合。
  digitalWrite(RELAY_PIN, relayOffLevel());
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayOffLevel());

  // PIR 输出作为数字输入；本程序约定 HIGH 表示检测到移动目标。
  pinMode(PIR_PIN, INPUT);

  Serial.println();
  Serial.println("LitterBoxVentilation ready");
  Serial.println("PIR: GPIO4, Relay: GPIO5");
  Serial.println("Status LED: GPIO8 WS2812");
  Serial.println("Waiting for presence...");
}

void loop() {
  // millis() 是从设备启动开始累计的毫秒数；无符号差值可安全处理计数回绕。
  const unsigned long now = millis();

  // 读取 PIR 当前电平。HIGH 时认为有移动，立即开启风机并刷新延时起点。
  const bool presenceDetected = digitalRead(PIR_PIN) == HIGH;

  if (presenceDetected) {
    lastPresenceMs = now;
    setFan(true);
  // PIR 已恢复 LOW，且从最后一次检测到移动起已超过延时时间，则关闭风机。
  } else if (fanOn && (now - lastPresenceMs >= EXHAUST_DELAY_MS)) {
    setFan(false);
  }

  // 将当前风机状态传给状态灯模块：待机慢速蓝色呼吸，运行快速橙色呼吸。
  statusLedUpdate(fanOn, now);

  // 降低轮询频率，避免空转占用 CPU；50ms 对 PIR 状态检测足够。
  delay(50);
}
