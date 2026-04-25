# ESP32-S3 WiFi UART Bridge

[English](README.md) | [简体中文](README.zh-CN.md)

An ESP32-S3 firmware project built with PlatformIO and the Arduino framework.
It bridges `UART1` and a raw TCP socket over Wi-Fi, and exposes a single WS2812 status LED on `IO48`.

## Features

- `UART1 <-> TCP` bidirectional bridge
- Web configuration page for UART and Wi-Fi profile management
- UART framing options: 5/6/7/8 data bits, parity N/E/O, stop bits 1/2
- Automatic fallback to AP mode if initial STA connection times out
- PSRAM-backed bridge buffers with SRAM fallback
- WS2812 status LED on `IO48`

## Pin Mapping

- `UART1 RX = IO13`
- `UART1 TX = IO14`
- Wiring reminder: `ESP32 RX <- peer TX`, `ESP32 TX -> peer RX`, and share `GND`

## Minimum Hardware Requirements

- Chip: recommended `ESP32-S3`; theoretical minimum `ESP32-S3`
- SRAM: recommended depends on traffic/load; theoretical minimum `>= 256 KB`
- Flash: recommended `8 MB` or above; theoretical minimum `>= 4 MB`
- PSRAM: recommended `8 MB`; theoretical minimum optional (smaller buffers / lower peak throughput)

The firmware is tested on **ESP32-S3-WROOM1-N16R8** (16 MB Flash / 8 MB PSRAM).
To target smaller boards, reduce bridge and UART driver buffer sizes in `src/main.cpp` if needed.

## Quick Start

1. Build and flash the firmware.
2. Open serial monitor at `115200` to read startup logs and IP information.
3. If no active STA profile exists, or the initial STA connection times out, connect to AP `ESP32S3-UART` / `12345678`.
4. Visit `http://192.168.4.1/` in AP mode, or `http://<device-ip>/` in STA mode.
5. Save or activate a Wi-Fi profile from the web page; the firmware reconfigures Wi-Fi immediately.

## Status LED

- Red: not connected to Wi-Fi and AP is not active
- Green: Wi-Fi connected, no TCP client
- Purple: TCP client connected
- Orange: AP active, no TCP client
- Blue double flash: UART/TCP data activity

## Network Behavior

- TCP bridge port: `6638`
- In STA mode, service listens on the station IP
- In AP mode, service listens on the softAP IP (default `192.168.4.1`)
- HTTP config page port: `80`

## HTTP API Quick Reference

- `GET /`: serve web configuration page (HTML)
- `GET /api/wifi`: return current Wi-Fi state and saved profiles
- `POST /api/wifi/scan`: trigger Wi-Fi scan and return nearby networks
- `POST /api/wifi/save`: save profile (`index`, `ssid`, optional `password`, optional `activate=1`)
- `POST /api/wifi/activate`: activate profile (`index`)
- `POST /api/wifi/delete`: delete profile (`index`)
- `GET /api/uart`: return current UART settings
- `POST /api/uart`: apply UART settings (`baudRate`, `dataBits`, `parity`, `stopBits`)

## Build

This project uses PlatformIO.

```bash
~/.platformio/penv/bin/platformio run
```

## Flash

Example:

```bash
~/.platformio/penv/bin/platformio run --target upload --upload-port /dev/ttyACM0
```

## Monitor Logs

```bash
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Linux-side Pseudo Serial Mapping

Example using `socat`:

```bash
sudo socat -d -d pty,raw,echo=0,mode=666,link=/dev/ttyESP32 tcp:<ESP32_IP>:6638
```

Then point your serial software to `/dev/ttyESP32`.

## Wi-Fi Configuration Notes

- Up to 24 Wi-Fi profiles are stored in NVS Preferences
- You can leave password empty to keep the existing password for that slot
- Build-time `WIFI_SSID` / `WIFI_PASSWORD` is used only for first-time seeding when no profile exists

## Common Troubleshooting

- Cannot open web page: check IP from serial log and confirm your PC is on the same network
- TCP client cannot connect: check firewall/routing and ensure port `6638` is reachable
- Garbled UART data: verify baud rate, data bits, parity, and stop bits match both ends
- Data loss under heavy load: reduce sender rate or increase buffer sizes when memory allows

## License

This project is licensed under the GNU General Public License v3.0.

See:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
