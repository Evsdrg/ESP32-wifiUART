# ESP32-S3 WiFi UART Bridge
# ESP32-S3 WiFi UART 桥接器

An ESP32-S3 firmware project built with PlatformIO and the Arduino framework.
一个基于 PlatformIO 和 Arduino 框架构建的 ESP32-S3 固件项目。
It bridges `UART1` and a raw TCP socket over Wi-Fi, and exposes a single WS2812 status LED on `IO48`.
它将 `UART1` 与 Wi-Fi 上的原始 TCP 套接字进行桥接，并在 `IO48` 上使用一个 WS2812 状态灯。

## Features
## 功能特性

- `UART1 <-> TCP` bidirectional bridge
- `UART1 <-> TCP` 双向桥接
- Web configuration page for UART and Wi-Fi profile management
- 提供 Web 配置页面，可管理 UART 参数和 Wi-Fi 配置组
- UART framing options: 5/6/7/8 data bits, parity N/E/O, stop bits 1/2
- UART 帧格式可选：5/6/7/8 数据位、N/E/O 校验位、1/2 停止位
- Automatic fallback to AP mode if initial STA connection times out
- 若初始 STA 连接超时，自动回退到 AP 模式
- PSRAM-backed bridge buffers with SRAM fallback
- 桥接缓冲优先使用 PSRAM，并支持回退到 SRAM
- WS2812 status LED on `IO48`
- `IO48` 上提供 WS2812 状态灯

## Pin Mapping
## 引脚映射

- `UART1 RX = IO13`
- `UART1 RX = IO13`
- `UART1 TX = IO14`
- `UART1 TX = IO14`
- Wiring reminder: `ESP32 RX <- peer TX`, `ESP32 TX -> peer RX`, and share `GND`
- 接线提醒：`ESP32 RX <- 对端 TX`、`ESP32 TX -> 对端 RX`，并确保共地 `GND`

## Minimum Hardware Requirements
## 最低硬件需求

| Resource | Recommended | Theoretical Minimum |
|----------|-------------|---------------------|
| Chip | ESP32-S3 | ESP32-S3 |
| SRAM | Depends on traffic/load | >= 256 KB |
| Flash | 8 MB or above | >= 4 MB |
| PSRAM | 8 MB | Optional (smaller buffers / lower peak throughput) |

| 资源 | 推荐配置 | 理论最低需求 |
|------|---------|---------------|
| 芯片 | ESP32-S3 | ESP32-S3 |
| SRAM | 取决于流量和负载 | >= 256 KB |
| Flash | 8 MB 或以上 | >= 4 MB |
| PSRAM | 8 MB | 可选（缓冲更小、峰值吞吐更低） |

The firmware is tested on **ESP32-S3-WROOM1-N16R8** (16 MB Flash / 8 MB PSRAM).
该固件已在 **ESP32-S3-WROOM1-N16R8**（16 MB Flash / 8 MB PSRAM）上验证。
To target smaller boards, reduce bridge and UART driver buffer sizes in `src/main.cpp` if needed.
如需适配更小板型，可按需在 `src/main.cpp` 中下调桥接缓冲和 UART 驱动缓冲尺寸。

## Quick Start
## 快速开始

1. Build and flash the firmware.
1. 构建并烧录固件。
2. Open serial monitor at `115200` to read startup logs and IP information.
2. 以 `115200` 打开串口监视器，查看启动日志和 IP 信息。
3. If no active STA profile exists (or initial STA connect times out), connect to AP `ESP32S3-UART` / `12345678`.
3. 若没有可用 STA 配置（或启动时 STA 连接超时），连接 AP：`ESP32S3-UART` / `12345678`。
4. Visit `http://192.168.4.1/` (AP mode) or `http://<device-ip>/` (STA mode).
4. 访问 `http://192.168.4.1/`（AP 模式）或 `http://<设备IP>/`（STA 模式）。
5. Save or activate a Wi-Fi profile from the web page; the firmware reconfigures Wi-Fi immediately.
5. 在网页中保存或启用 Wi-Fi 配置组后，固件会立即重配 Wi-Fi。

## Status LED
## 状态灯

- Red: not connected to Wi-Fi and AP is not active
- 红：未连接 Wi-Fi 且 AP 未激活
- Green: Wi-Fi connected, no TCP client
- 绿：Wi-Fi 已连接，但没有 TCP 客户端
- Purple: TCP client connected
- 紫：TCP 客户端已连接
- Orange: AP active, no TCP client
- 橙：AP 已启用，但没有 TCP 客户端
- Blue double flash: UART/TCP data activity
- 蓝色双闪：UART/TCP 正在传输数据

## Network Behavior
## 网络行为

- TCP bridge port: `6638`
- TCP 桥接端口：`6638`
- In STA mode, service listens on station IP.
- 在 STA 模式下，服务监听 station IP。
- In AP mode, service listens on softAP IP (default `192.168.4.1`).
- 在 AP 模式下，服务监听 softAP IP（默认 `192.168.4.1`）。
- HTTP config page port: `80`
- HTTP 配置页端口：`80`

## HTTP API Quick Reference
## HTTP API 速查

- `GET /`: Serve web configuration page (HTML)
- `GET /`：返回 Web 配置页面（HTML）
- `GET /api/wifi`: Return current Wi-Fi state and saved profiles
- `GET /api/wifi`：返回当前 Wi-Fi 状态和已保存配置组
- `POST /api/wifi/scan`: Trigger Wi-Fi scan and return nearby networks
- `POST /api/wifi/scan`：触发 Wi-Fi 扫描并返回附近网络列表
- `POST /api/wifi/save`: Save profile (`index`, `ssid`, optional `password`, optional `activate=1`)
- `POST /api/wifi/save`：保存配置组（`index`、`ssid`、可选 `password`、可选 `activate=1`）
- `POST /api/wifi/activate`: Activate profile (`index`)
- `POST /api/wifi/activate`：启用配置组（`index`）
- `POST /api/wifi/delete`: Delete profile (`index`)
- `POST /api/wifi/delete`：删除配置组（`index`）
- `GET /api/uart`: Return current UART settings
- `GET /api/uart`：返回当前 UART 参数
- `POST /api/uart`: Apply UART settings (`baudRate`, `dataBits`, `parity`, `stopBits`)
- `POST /api/uart`：应用 UART 参数（`baudRate`、`dataBits`、`parity`、`stopBits`）

## Build
## 构建

This project uses PlatformIO.
本项目使用 PlatformIO。

```bash
~/.platformio/penv/bin/platformio run
```

## Flash
## 烧录

Example:
示例：

```bash
~/.platformio/penv/bin/platformio run --target upload --upload-port /dev/ttyACM0
```

## Monitor Logs
## 查看日志

```bash
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Linux-side Pseudo Serial Mapping
## Linux 端伪串口映射

Example using `socat`:
使用 `socat` 的示例：

```bash
sudo socat -d -d pty,raw,echo=0,mode=666,link=/dev/ttyESP32 tcp:<ESP32_IP>:6638
```

Then point your serial software to `/dev/ttyESP32`.
然后在你的串口软件中使用 `/dev/ttyESP32` 进行通信。

## Wi-Fi Configuration Notes
## Wi-Fi 配置说明

- Up to 24 Wi-Fi profiles are stored in NVS Preferences.
- 最多可在 NVS Preferences 中保存 24 组 Wi-Fi 配置。
- You can leave password empty to keep existing password for that slot.
- 保存时可将密码留空，以保留该槽位已有密码。
- Build-time `WIFI_SSID` / `WIFI_PASSWORD` is used only for first-time seeding when no profile exists.
- `WIFI_SSID` / `WIFI_PASSWORD` 构建宏仅在“无任何已存配置”时用于首次初始化。

## Common Troubleshooting
## 常见问题排查

- Cannot open web page: check IP from serial log and confirm your PC is in the same network.
- 网页打不开：先在串口日志确认 IP，并确保电脑与设备处于同一网段。
- TCP client cannot connect: check firewall/routing and ensure port `6638` is reachable.
- TCP 客户端连不上：检查防火墙/路由，并确认端口 `6638` 可达。
- Garbled UART data: verify baud rate, data bits, parity, and stop bits match both ends.
- 串口乱码：确认两端波特率、数据位、校验位、停止位完全一致。
- Data loss under heavy load: reduce sender rate or increase buffer sizes when memory allows.
- 高负载丢包：降低发送速率，或在内存允许时增大缓冲区。

## License
## 许可证

This project is licensed under the GNU General Public License v3.0.
本项目采用 GNU General Public License v3.0 开源许可。

See:
另请参阅：

- `LICENSE`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
- `THIRD_PARTY_NOTICES.md`

## Source Distribution Note
## 源代码分发说明

If you redistribute a firmware binary built from this project, you should also make the corresponding source code and build configuration available under the same GPLv3 terms.
若你重新分发基于本项目构建的固件二进制，也应按照 GPLv3 提供相应源代码和构建配置。

For this project, that should include at least:
对于本项目，至少应包含：

- Application source code
- 应用源代码
- `platformio.ini`
- `platformio.ini`
- Build and flashing instructions
- 构建与烧录说明
- Any distributed modifications to upstream code
- 你分发时对上游代码所做的任何修改

## Installation Information Note
## 安装信息说明

If this firmware is distributed preinstalled in a user product, GPLv3 may also require providing enough installation information for recipients to install a modified version themselves.
若此固件作为预装件随用户产品分发，GPLv3 还可能要求提供足够安装信息，以便接收方自行安装修改版本。
