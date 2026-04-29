# ESP WiFi UART 桥接器

[English](README.md) | [简体中文](README.zh-CN.md)

ESP WiFi UART Bridge 是一个通过 Wi-Fi 暴露 TCP 套接字，并将其桥接到硬件 UART 的固件项目。
仓库通过独立分支维护不同目标板的固件，这样板级引脚映射、状态灯接法和内存调优就可以分别演进。

## 分支布局

| 分支 | 目标平台 | UART 引脚 | 状态灯 |
|------|----------|-----------|--------|
| `ESP32S3` | ESP32-S3 配置 | `RX=IO13`、`TX=IO14` | `IO48` 上的 WS2812 |
| `ESP32C3` | ESP32-C3 SuperMini 配置 | `RX=IO3`、`TX=IO4` | `IO8` 板载 LED |
| `ESP32C6` | Espressif ESP32-C6-DevKitC-1 配置 | `RX=IO4`、`TX=IO5` | `IO8` 上的可寻址 RGB LED |

构建或烧录前，请先切换到与你硬件匹配的分支。

## 共通特性

- `UART1 <-> TCP` 双向桥接
- 提供 Web 配置页面，可管理 UART 参数和 Wi-Fi 配置组
- 支持 Wi-Fi STA 模式，启动时 STA 连接超时会自动回退到 AP 模式
- TCP 桥接服务端口为 `6638`
- 支持通过 `platformio.ini` 进行可选的构建时 Wi-Fi 初始化
- 支持通过 `socat` 在 Linux 下映射为伪串口

## 构建

本项目使用 PlatformIO。

1. 切换到目标硬件对应的分支。
2. 检查该分支中的 `platformio.ini`。
3. 执行构建：

```bash
~/.platformio/penv/bin/platformio run
```

## 烧录

示例：

```bash
~/.platformio/penv/bin/platformio run --target upload --upload-port /dev/ttyACM0
```

## 查看日志

示例：

```bash
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Linux 端伪串口映射

使用 `socat` 的示例：

```bash
sudo socat -d -d pty,raw,echo=0,mode=666,link=/dev/ttyESP32 tcp:<ESP32_IP>:6638
```

然后在你的串口软件中使用 `/dev/ttyESP32` 进行通信。

## 配置说明

- Wi-Fi 凭据可通过 Web 界面写入，并保存在 NVS Preferences 中
- 最多支持 24 组 Wi-Fi 配置
- 缓冲区大小、UART FIFO 阈值、状态灯行为和板级配置会因 `ESP32S3`、`ESP32C3` 与 `ESP32C6` 分支不同而有所区别，这些差异是有意保留的

## ESP32-C6 说明

- `ESP32C6` 分支面向 Espressif ESP32-C6-DevKitC-1，PlatformIO 板卡 ID 为 `esp32-c6-devkitc-1`
- 默认 UART 引脚为 `RX=IO4`、`TX=IO5`；如果接线不同，可在 `platformio.ini` 中调整 `UART1_RX_PIN` 和 `UART1_TX_PIN`
- `IO8` 上的板载可寻址 RGB LED 参考 ESP32-S3 分支的状态颜色：断网红色、AP 模式橙色、STA 已连接绿色、TCP 已连接紫色、数据活动蓝色脉冲、Wi-Fi 扫描时绿色闪烁
- ESP32-C6 对普通应用暴露 1 个 FreeRTOS 主核，另有 1 个 LP core。LP core 适合低功耗唤醒和简单监测，不适合运行 Arduino 任务或分担 TCP/UART 桥接
- 桥接缓冲区沿用 ESP32-C3 的静态 SRAM 模型，因为常见 ESP32-C6-DevKitC-1 板卡不带 PSRAM

## 许可证

本项目采用 GNU General Public License v3.0 开源许可。

另请参阅：

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
