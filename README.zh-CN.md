# ESP32-S3 WiFi UART 桥接器

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 PlatformIO 和 Arduino 框架构建的 ESP32-S3 固件项目。
它将 `UART1` 与 Wi-Fi 上的原始 TCP 套接字进行桥接，并在 `IO48` 上使用一个 WS2812 状态灯。

## 功能特性

- `UART1 <-> TCP` 双向桥接
- 提供 Web 配置页面，可管理 UART 参数和 Wi-Fi 配置组
- UART 帧格式可选：5/6/7/8 数据位、N/E/O 校验位、1/2 停止位
- 若初始 STA 连接超时，自动回退到 AP 模式
- 桥接缓冲优先使用 PSRAM，并支持回退到 SRAM
- `IO48` 上提供 WS2812 状态灯

## 引脚映射

- `UART1 RX = IO13`
- `UART1 TX = IO14`
- 接线提醒：`ESP32 RX <- 对端 TX`、`ESP32 TX -> 对端 RX`，并确保共地 `GND`

## 最低硬件需求

- 芯片：推荐 `ESP32-S3`；理论最低 `ESP32-S3`
- SRAM：推荐值取决于流量和负载；理论最低 `>= 256 KB`
- Flash：推荐 `8 MB` 及以上；理论最低 `>= 4 MB`
- PSRAM：推荐 `8 MB`；理论最低为可选（缓冲更小、峰值吞吐更低）

该固件已在 **ESP32-S3-WROOM1-N16R8**（16 MB Flash / 8 MB PSRAM）上验证。
如需适配更小板型，可按需在 `src/main.cpp` 中下调桥接缓冲和 UART 驱动缓冲尺寸。

## 快速开始

1. 构建并烧录固件。
2. 以 `115200` 打开串口监视器，查看启动日志和 IP 信息。
3. 若没有可用 STA 配置，或启动时 STA 连接超时，连接 AP：`ESP32S3-UART` / `12345678`。
4. AP 模式访问 `http://192.168.4.1/`，STA 模式访问 `http://<设备IP>/`。
5. 在网页中保存或启用 Wi-Fi 配置组后，固件会立即重配 Wi-Fi。

## 状态灯

- 红：未连接 Wi-Fi 且 AP 未激活
- 绿：Wi-Fi 已连接，但没有 TCP 客户端
- 紫：TCP 客户端已连接
- 橙：AP 已启用，但没有 TCP 客户端
- 蓝色双闪：UART/TCP 正在传输数据

## 网络行为

- TCP 桥接端口：`6638`
- STA 模式下，服务监听 station IP
- AP 模式下，服务监听 softAP IP（默认 `192.168.4.1`）
- HTTP 配置页端口：`80`

## HTTP API 速查

- `GET /`：返回 Web 配置页面（HTML）
- `GET /api/wifi`：返回当前 Wi-Fi 状态和已保存配置组
- `POST /api/wifi/scan`：触发 Wi-Fi 扫描并返回附近网络列表
- `POST /api/wifi/save`：保存配置组（`index`、`ssid`、可选 `password`、可选 `activate=1`）
- `POST /api/wifi/activate`：启用配置组（`index`）
- `POST /api/wifi/delete`：删除配置组（`index`）
- `GET /api/uart`：返回当前 UART 参数
- `POST /api/uart`：应用 UART 参数（`baudRate`、`dataBits`、`parity`、`stopBits`）

## 构建

本项目使用 PlatformIO。

```bash
~/.platformio/penv/bin/platformio run
```

## 烧录

示例：

```bash
~/.platformio/penv/bin/platformio run --target upload --upload-port /dev/ttyACM0
```

## 查看日志

```bash
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Linux 端伪串口映射

使用 `socat` 的示例：

```bash
sudo socat -d -d pty,raw,echo=0,mode=666,link=/dev/ttyESP32 tcp:<ESP32_IP>:6638
```

然后在你的串口软件中使用 `/dev/ttyESP32` 进行通信。

## Wi-Fi 配置说明

- 最多可在 NVS Preferences 中保存 24 组 Wi-Fi 配置
- 保存时可将密码留空，以保留该槽位已有密码
- `WIFI_SSID` / `WIFI_PASSWORD` 构建宏仅在“无任何已存配置”时用于首次初始化

## 常见问题排查

- 网页打不开：先在串口日志确认 IP，并确保电脑与设备处于同一网段
- TCP 客户端连不上：检查防火墙/路由，并确认端口 `6638` 可达
- 串口乱码：确认两端波特率、数据位、校验位、停止位完全一致
- 高负载丢包：降低发送速率，或在内存允许时增大缓冲区

## 许可证

本项目采用 GNU General Public License v3.0 开源许可。

另请参阅：

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
