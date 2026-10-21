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
// 依据当前实测（COM 接 12V+、NO 接风机正极，但风机状态与 RGB 指示相反），
// 先按高电平触发配置；重新测试后若仍相反，再核对继电器模块说明和 IN 指示灯。
constexpr bool RELAY_ACTIVE_LOW = false;

// 时序配置：当前先用 DEMO 模式做快速联调。
// DEMO_MODE = true：每 10ms 检查一次 PIR；PIR 恢复 LOW 后继续吹 10 秒，且至少运行 10 秒。
// 正式使用时改为 false：每 50ms 检查一次；PIR 恢复 LOW 后继续吹 3 分钟，且至少运行 30 秒。
// 注意：PIR 模块自身输出 HIGH 的保持时长由模块上的电位器决定，代码不能缩短它。
constexpr bool DEMO_MODE = true;
constexpr unsigned long PIR_POLL_INTERVAL_MS = DEMO_MODE ? 10UL : 50UL;
constexpr unsigned long EXHAUST_DELAY_MS = DEMO_MODE ? 10UL * 1000UL : 3UL * 60UL * 1000UL;
constexpr unsigned long FAN_MIN_RUN_MS = DEMO_MODE ? 10UL * 1000UL : 30UL * 1000UL;

// 当前风机逻辑状态。用于避免每轮 loop 都重复写继电器并重复打印日志。
bool fanOn = false;

// 最近一次检测到移动的 millis() 时间戳，用于计算延时排风时长。
unsigned long lastPresenceMs = 0;

// 风机最近一次启动的时间戳，用于保证最短运行时间。
unsigned long fanStartedMs = 0;

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
  if (fanOn) {
    fanStartedMs = millis();
  }

  // 根据继电器模块的有效电平，输出吸合或释放电平。
  digitalWrite(RELAY_PIN, fanOn ? relayOnLevel() : relayOffLevel());
  Serial.printf("[%lu ms] Fan: %s\n", millis(), fanOn ? "ON" : "OFF");
}

void setup() {
  // 调试串口使用 115200 波特率。
  Serial.begin(115200);

  // 尽早将继电器设为关闭状态，避免启动提示灯闪烁期间继电器输入悬空。
  // 先写关闭电平，再设为输出，可尽量避免 GPIO 切换时的误动作。
  digitalWrite(RELAY_PIN, relayOffLevel());
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, relayOffLevel());

  // PIR 输出作为数字输入；本程序约定 HIGH 表示检测到移动目标。
  pinMode(PIR_PIN, INPUT);

  // 初始化板载 RGB 状态灯，并闪烁三次绿色作为启动提示。
  statusLedBegin();

  Serial.println();
  Serial.println("LitterBoxVentilation ready");
  Serial.println("PIR: GPIO4, Relay: GPIO5");
  Serial.println("Status LED: GPIO8 WS2812");
  Serial.println("Relay initialized to OFF");
  Serial.printf("Timing mode: %s; exhaust delay: %lu ms; minimum run: %lu ms\n",
                DEMO_MODE ? "DEMO" : "PRODUCTION",
                EXHAUST_DELAY_MS,
                FAN_MIN_RUN_MS);
  Serial.println("Waiting for presence...");
}

void loop() {
  // millis() 是从设备启动开始累计的毫秒数；无符号差值可安全处理计数回绕。
  const unsigned long now = millis();

  // 读取 PIR 当前电平。HIGH 时认为有移动，立即开启风机并刷新延时起点。
  const bool presenceDetected = digitalRead(PIR_PIN) == HIGH;
  static bool previousPresenceDetected = false;

  // 只在 PIR 电平变化时打印，便于从串口判断是否发生了重复触发。
  if (presenceDetected != previousPresenceDetected) {
    Serial.printf("[%lu ms] PIR: %s\n", now, presenceDetected ? "HIGH" : "LOW");
    previousPresenceDetected = presenceDetected;
  }

  if (presenceDetected) {
    lastPresenceMs = now;
    setFan(true);
  // PIR 已恢复 LOW 后，满足延时排风和最短运行时间两个条件才关闭风机。
  } else if (fanOn &&
             (now - lastPresenceMs >= EXHAUST_DELAY_MS) &&
             (now - fanStartedMs >= FAN_MIN_RUN_MS)) {
    setFan(false);
  }

  // 将当前风机状态传给状态灯模块：待机慢速蓝色呼吸，运行快速橙色呼吸。
  statusLedUpdate(fanOn, now);

  // 按调试/正式模式配置的间隔轮询 PIR，避免无意义地高速占用 CPU。
  delay(PIR_POLL_INTERVAL_MS);
}
