#include "StatusLed.h"

#include <Adafruit_NeoPixel.h>

namespace {

// 板载 WS2812 RGB 灯的数据引脚和灯珠数量。
constexpr uint8_t STATUS_LED_PIN = 8;
constexpr uint8_t STATUS_LED_COUNT = 1;

// 将 NeoPixel 对象限制在本文件内，避免主程序直接依赖灯带实现细节。
Adafruit_NeoPixel statusLed(STATUS_LED_COUNT, STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

}  // namespace

void statusLedBegin() {
  // 初始化 WS2812 控制器，并先清除灯珠状态。
  statusLed.begin();
  statusLed.clear();
  statusLed.show();

  // 上电后闪三次绿色，提供一个容易识别的启动/烧录成功提示。
  for (uint8_t i = 0; i < 3; ++i) {
    statusLed.setPixelColor(0, statusLed.Color(0, 160, 0));
    statusLed.show();
    delay(140);
    statusLed.clear();
    statusLed.show();
    delay(120);
  }
}

void statusLedUpdate(bool fanOn, unsigned long nowMs) {
  // 选择呼吸周期和最高亮度：风机运行时动画更快、亮度更高。
  const unsigned long periodMs = fanOn ? 700UL : 2400UL;
  const uint8_t peakBrightness = fanOn ? 190 : 55;

  // 把时间映射到一个三角波：亮度从 0 线性升到峰值，再降回 0。
  // phase 在每个 periodMs 周期内循环，halfPeriod 是升亮和变暗的分界点。
  const unsigned long phase = nowMs % periodMs;
  const unsigned long halfPeriod = periodMs / 2;
  const unsigned long triangle = phase < halfPeriod
      ? phase * 255UL / halfPeriod
      : (periodMs - phase) * 255UL / halfPeriod;
  const uint8_t brightness = static_cast<uint8_t>(triangle * peakBrightness / 255UL);

  // 待机选择蓝色，运行选择橙色；下面再按呼吸亮度缩放各颜色通道。
  const uint8_t red = fanOn ? 255 : 0;
  const uint8_t green = fanOn ? 70 : 35;
  const uint8_t blue = fanOn ? 0 : 200;

  // 分别缩放 RGB 三个通道，避免频繁调用 setBrightness 带来的像素缓冲缩放误差。
  statusLed.setPixelColor(
      0,
      statusLed.Color(
          static_cast<uint8_t>(red * brightness / 255),
          static_cast<uint8_t>(green * brightness / 255),
          static_cast<uint8_t>(blue * brightness / 255)));

  // 将计算好的 RGB 值发送到板载 WS2812 灯珠。
  statusLed.show();
}
