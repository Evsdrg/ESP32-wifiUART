# ESP WiFi UART Bridge

[English](README.md) | [简体中文](README.zh-CN.md)

ESP WiFi UART Bridge is a firmware project that exposes a TCP socket over Wi-Fi and bridges it to a hardware UART.
The repository keeps target-specific firmware in separate branches so board-level pin mapping, status LED wiring, and memory tuning can evolve independently.

## Branch Layout

| Branch | Target | UART Pins | Status LED |
|--------|--------|-----------|------------|
| `ESP32S3` | ESP32-S3 board configuration | `RX=IO13`, `TX=IO14` | WS2812 on `IO48` |
| `ESP32C3` | ESP32-C3 SuperMini configuration | `RX=IO3`, `TX=IO4` | Onboard LED on `IO8` |

Check out the branch that matches your hardware before building or flashing.

## Common Features

- `UART1 <-> TCP` bidirectional bridge
- Web configuration page for UART parameters and Wi-Fi profile management
- Wi-Fi station mode with automatic fallback to AP mode when the initial STA connection times out
- TCP bridge service on port `6638`
- Optional build-time Wi-Fi seeding through `platformio.ini`
- Linux pseudo-serial access through `socat`

## Build

This project uses PlatformIO.

1. Check out the target branch you want to use.
2. Review `platformio.ini` for that branch.
3. Build the firmware:

```bash
~/.platformio/penv/bin/platformio run
```

## Flash

Example:

```bash
~/.platformio/penv/bin/platformio run --target upload --upload-port /dev/ttyACM0
```

## Monitor Logs

Example:

```bash
~/.platformio/penv/bin/platformio device monitor --baud 115200
```

## Linux-side Pseudo Serial Mapping

Example using `socat`:

```bash
sudo socat -d -d pty,raw,echo=0,mode=666,link=/dev/ttyESP32 tcp:<ESP32_IP>:6638
```

Then point your serial software to `/dev/ttyESP32`.

## Configuration Notes

- Wi-Fi credentials can be provisioned from the web UI and are stored in NVS Preferences
- Up to 24 Wi-Fi profiles are supported
- Branch-specific buffer sizes, UART FIFO thresholds, LED behavior, and board settings are intentional and may differ between `ESP32S3` and `ESP32C3`

## ESP32-C3 Notes

- The `ESP32C3` branch targets common ESP32-C3 SuperMini boards with the PlatformIO board ID `nologo_esp32c3_super_mini`
- Wi-Fi TX power is limited by default with `WIFI_POWER_8_5dBm`; this is intentional for typical C3 SuperMini boards to reduce heat and power draw and improve long-running stability
- If an ESP32-C3 SuperMini cannot connect to Wi-Fi, or if its fallback AP cannot be seen by nearby devices, inspect the original onboard antenna. On problematic boards, removing the original antenna and using a better external antenna path may be necessary

## License

This project is licensed under the GNU General Public License v3.0.

See:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
