# LitterBoxVentilation

ESP32-C3-DevKitM-1 的单 PIR 自动排风基础项目：检测到人体后启动继电器和 12V 风机；PIR 输出恢复 LOW 后继续运行一段时间，再关闭风机。状态灯已拆到 `StatusLed.h/.cpp`，主程序通过 `statusLedBegin()` 和 `statusLedUpdate()` 调用。

## 引脚

| 信号 | ESP32-C3 引脚 |
|---|---:|
| PIR OUT | GPIO4 |
| 继电器 IN | GPIO5 |
| 板载 WS2812 RGB 灯 | GPIO8 |
| 公共地 | GND |

PIR 与 ESP32 共地。PIR 的 OUT 高电平必须在 ESP32 GPIO 可承受范围内（不高于 3.3V）。继电器 IN 必须确认支持 3.3V 控制，不能把 12V 接到 GPIO。

## 初次调试

1. 确认继电器模块线圈为 12V，触点接 `COM` 与 `NO`；`NC` 留空。确认风机电源关闭时继电器不吸合。
2. Arduino IDE 选择 `ESP32C3 Dev Module`、实际 USB 串口；Flash Size 设为 `4MB (32Mb)`，Partition Scheme 设为 `Default 4MB with spiffs`。
3. 打开 `LitterBoxVentilation.ino` 并上传。串口监视器设为 115200 baud。
4. 触发 PIR，串口应打印 `Fan: ON`，继电器吸合、风机启动。PIR 输出恢复 LOW 后约 10 秒，串口应打印 `Fan: OFF`，继电器释放。
5. 如果继电器开关逻辑相反，断电后核对继电器模块触发跳帽，并将代码中的 `RELAY_ACTIVE_LOW` 改为 `false` 或 `true` 后重烧。
6. 调试稳定后，将 `EXHAUST_DELAY_MS` 从 10 秒改为例如 `3UL * 60UL * 1000UL`（3 分钟）。

## 状态灯

- 每次启动先闪烁 3 次绿色，表示程序已开始运行。
- 待机时缓慢蓝色呼吸。
- PIR 触发、风机运行时快速且更亮的橙色呼吸；延时结束、风机关闭后恢复慢速蓝色呼吸。

## 供电注意

- 按你现在确认的接法，DCP3512 的 3.3V 输出接开发板 `3V3`，GND 共地。不要把 5V 接到 `3V3`。
- DCP3512 的 12V 输出需要能承受风机启动电流和继电器负载；12V 适配器也要有余量。
- 首次联调先不接风机负载，确认继电器控制逻辑及常态为 OFF 后再接入风机。

本版本不包含 Wi-Fi、蓝牙、第二个 PIR 或手机控制。
