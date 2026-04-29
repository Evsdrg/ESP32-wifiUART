# ESP WiFi UART Bridge

[English](README.md) | [简体中文](README.zh-CN.md)

ESP WiFi UART Bridge is a firmware project that exposes a TCP socket over Wi-Fi and bridges it to a hardware UART.
The repository keeps target-specific firmware in separate branches so board-level pin mapping, status LED wiring, and memory tuning can evolve independently.

## Branch Layout

| Branch | Target | UART Pins | Status LED |
|--------|--------|-----------|------------|
| `ESP32S3` | ESP32-S3 board configuration | `RX=IO13`, `TX=IO14` | WS2812 on `IO48` |
| `ESP32C3` | ESP32-C3 SuperMini configuration | `RX=IO3`, `TX=IO4` | Onboard LED on `IO8` |
| `ESP32C6` | Espressif ESP32-C6-DevKitC-1 configuration | `RX=IO18`, `TX=IO9` | Addressable RGB LED on `IO8` |

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
- Branch-specific buffer sizes, UART FIFO thresholds, LED behavior, and board settings are intentional and may differ between `ESP32S3`, `ESP32C3`, and `ESP32C6`

## Board Notes

### ESP32-S3

- The `ESP32S3` branch targets an ESP32-S3 board with the PlatformIO board ID `esp32s3_120_16_8-qio_opi`
- Default UART pins are `RX=IO13` and `TX=IO14`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The status LED is a WS2812 RGB LED on `IO48`, driven with FastLED
- The S3 build uses larger bridge buffers and prefers PSRAM allocation when available, falling back to internal SRAM

### ESP32-C3

- The `ESP32C3` branch targets Nologo ESP32-C3 SuperMini with the PlatformIO board ID `nologo_esp32c3_super_mini`
- Default UART pins are `RX=IO3` and `TX=IO4`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The status LED is the onboard single-color LED on `IO8`, configured as active-low
- The C3 build uses fixed static SRAM bridge buffers for predictable long-running behavior on boards without PSRAM

### ESP32-C6

- The `ESP32C6` branch targets Espressif ESP32-C6-DevKitC-1 with the PlatformIO board ID `esp32-c6-devkitc-1`
- Default UART pins are `RX=IO18` and `TX=IO9`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The onboard addressable RGB LED on `IO8` follows the ESP32-S3 status color scheme: red for disconnected, orange for AP mode, green for STA connected, purple for TCP connected, blue pulses for data activity, and green blinking during Wi-Fi scans
- ESP32-C6 exposes one FreeRTOS application core plus an LP core. The LP core is for low-power wake and simple monitoring workflows, not for running Arduino tasks or offloading the TCP/UART bridge
- The bridge buffers intentionally follow the ESP32-C3 static SRAM model because common ESP32-C6-DevKitC-1 boards do not provide PSRAM
- Wi-Fi TX power is not configured by default on ESP32-C6; enable `CONFIGURE_WIFI_TX_POWER=1` and set `WIFI_TX_POWER` in `platformio.ini` if your deployment needs an explicit power level

## License

This project is licensed under the GNU General Public License v3.0.

See:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
