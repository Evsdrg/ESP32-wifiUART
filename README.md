# ESP WiFi UART Bridge

[English](README.md) | [简体中文](README.zh-CN.md)

ESP WiFi UART Bridge is a firmware project that exposes a TCP socket over Wi-Fi and bridges it to a hardware UART.
The repository keeps target-specific firmware in separate branches so board-level pin mapping, status LED wiring, and memory tuning can evolve independently.

## Branch Layout

| Branch | PlatformIO env | Target | UART Pins | UART FIFO tier | Status LED |
|--------|----------------|--------|-----------|----------------|------------|
| `ESP32S3` | `esp32s3_120_16_8-qio_opi` | ESP32-S3 board configuration | `RX=IO13`, `TX=IO14` | HIGH, threshold `96` | WS2812 on `IO48` via Arduino RGB LED helper |
| `ESP32C3` | `nologo_esp32c3_super_mini` | ESP32-C3 SuperMini configuration | `RX=IO3`, `TX=IO4` | MID, threshold `32` | Onboard LED on `IO8`, active-low |
| `ESP32C6` | `esp32_c6_devkitc_1` | Espressif ESP32-C6-DevKitC-1 configuration | `RX=IO10`, `TX=IO11` | HIGH, threshold `96` | Addressable RGB LED on `IO8` |

Check out the branch that matches your hardware before building or flashing.

## Common Features

- `UART1 <-> TCP` bidirectional bridge
- Web configuration page for UART parameters and Wi-Fi profile management
- Wi-Fi station mode with automatic fallback to AP mode when the initial STA connection times out
- TCP bridge service on port `6638`
- RFC2217 remote serial service on port `2217` for tools that need DTR/RTS, such as `esptool.py`
- Optional build-time Wi-Fi seeding through `platformio.ini`
- Linux pseudo-serial access through `socat`

## Build

This project uses PlatformIO.

1. Check out the target branch you want to use.
2. Review the board-specific section in `platformio.ini`.
3. Build the firmware:

```bash
~/.platformio/penv/bin/platformio run -e <platformio-env>
```

Examples:

```bash
~/.platformio/penv/bin/platformio run -e esp32s3_120_16_8-qio_opi
~/.platformio/penv/bin/platformio run -e nologo_esp32c3_super_mini
~/.platformio/penv/bin/platformio run -e esp32_c6_devkitc_1
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

`socat` over the raw TCP port only transports UART bytes. It does not forward DTR/RTS modem-control events.

## RFC2217 Remote Flashing

The firmware also exposes an RFC2217 endpoint on port `2217`. Use it directly with pyserial-aware tools instead of wrapping it in `socat`:

```bash
esptool.py --chip esp32c3 --port rfc2217://<ESP32_IP>:2217 --baud 460800 write_flash 0x0 firmware.bin
```

For ESP auto-reset wiring, connect the bridge board and target board like this:

- Bridge `TX` -> target `RX`
- Bridge `RX` -> target `TX`
- Bridge `DTR` GPIO -> target `GPIO0` / `BOOT`
- Bridge `RTS` GPIO -> target `EN` / `RST`
- Common ground between both boards

The RFC2217 service is enabled by default, but DTR/RTS output pins default to `-1` and do not drive any GPIO until configured. Set these build flags in `platformio.ini` for your wiring:

```ini
; -D UART_BRIDGE_DTR_PIN=0
; -D UART_BRIDGE_RTS_PIN=1
; -D UART_BRIDGE_CONTROL_ACTIVE_LOW=1
```

If a specific client ignores normal RFC2217 control acknowledgements poorly, pyserial also supports:

```bash
esptool.py --chip esp32c3 --port rfc2217://<ESP32_IP>:2217?ign_set_control --baud 460800 write_flash 0x0 firmware.bin
```

## Configuration Notes

- Wi-Fi credentials can be provisioned from the web UI and are stored in NVS Preferences
- Up to 24 Wi-Fi profiles are supported
- Bridge buffers are `12KB` for TCP->UART and `20KB` for UART->TCP; UART driver buffers are `8KB` RX and `4KB` TX
- RFC2217 uses separate `4KB` TCP->UART and `4KB` UART->TCP buffers so flashing/control sessions do not share raw TCP bridge buffers
- `platformio.ini` is intentionally split into separate S3/C3/C6 environment sections instead of relying on implicit branch-only settings
- UART RX FIFO threshold is configurable with `UART_FIFO_THRESHOLD`; current presets are MID=`32` for C3 and HIGH=`96` for S3/C6

## HTTP API Notes

- `POST /api/wifi/scan` starts a non-blocking Wi-Fi scan and returns `{ "scanning": true, "networks": [] }` while the scan is running
- `GET /api/wifi/scan` returns the current scan state and the most recent scan result list
- `POST /api/wifi/save` keeps the previous password for an existing slot when `password` is empty and `keepPassword` is omitted or set to `1`; pass `keepPassword=0` to save an empty password for open networks
- Define `ENABLE_HTTP_AUTH=1` and set `HTTP_AUTH_PASSWORD` in `platformio.ini` to protect the web UI and JSON APIs with HTTP Basic Auth

## UART FIFO Tiers

The ESP32 UART FIFO is 128 bytes. A higher `UART_FIFO_THRESHOLD` reduces interrupt frequency but increases packetization delay.

| Tier | Threshold | 115200 baud | 460800 baud | 921600 baud | Intended use |
|------|-----------|-------------|-------------|-------------|--------------|
| LOW | `16` | ~720 interrupts/s, ~1.4 ms | ~2880 interrupts/s, ~0.35 ms | ~5760 interrupts/s, ~0.17 ms | Lowest latency |
| MID | `32` | ~360 interrupts/s, ~2.8 ms | ~1440 interrupts/s, ~0.69 ms | ~2880 interrupts/s, ~0.35 ms | Balanced response and interrupts |
| HIGH | `96` | ~120 interrupts/s, ~8.3 ms | ~480 interrupts/s, ~2.1 ms | ~960 interrupts/s, ~1.0 ms | Lower interrupt rate at high baud rates |

## Board Notes

### ESP32-S3

- The `ESP32S3` branch targets an ESP32-S3 board with the PlatformIO board ID `esp32s3_120_16_8-qio_opi`
- Default UART pins are `RX=IO13` and `TX=IO14`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The default UART FIFO threshold is HIGH (`UART_FIFO_THRESHOLD=96`), suitable for `460800`/`921600` use with reduced interrupt rate
- The status LED is a WS2812 RGB LED on `IO48`, driven through the Arduino-ESP32 RGB LED helper instead of FastLED
- The ESP32-S3 branch uses static SRAM bridge buffers by default and can opt into PSRAM allocation with `USE_PSRAM_BRIDGE_BUFFERS=1`
- Wi-Fi TX power is not configured by default on ESP32-S3; enable `CONFIGURE_WIFI_TX_POWER=1` and set `WIFI_TX_POWER` in `platformio.ini` if your deployment needs an explicit power level

### ESP32-C3

- The `ESP32C3` branch targets common ESP32-C3 SuperMini boards with the PlatformIO board ID `nologo_esp32c3_super_mini`
- Default UART pins are `RX=IO3` and `TX=IO4`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The status LED is the onboard single-color LED on `IO8`, configured as active-low
- The default UART FIFO threshold is MID (`UART_FIFO_THRESHOLD=32`) to keep `9600`/`115200` responsive while avoiding excessive interrupts
- The default app partition is sufficient for the current firmware; `huge_app.csv` is not used
- The C3 build uses fixed static SRAM bridge buffers for predictable long-running behavior on boards without PSRAM
- Wi-Fi TX power is limited by default with `WIFI_POWER_8_5dBm`; this is intentional for typical C3 SuperMini boards to reduce heat and power draw and improve long-running stability
- Wi-Fi signal issues are common on black-PCB ESP32-C3 SuperMini boards. If the board cannot connect to Wi-Fi, or if its fallback AP cannot be seen by nearby devices, removing the original onboard antenna will usually make the device work again
- Removing the original antenna can make the signal strongly directional. It is acceptable for temporary use, but for long-term deployments, prefer another development board with a more reasonable antenna design

### ESP32-C6

- The `ESP32C6` branch targets Espressif ESP32-C6-DevKitC-1 with the PlatformIO board ID `esp32-c6-devkitc-1`
- Default UART pins are `RX=IO10` and `TX=IO11`; adjust `UART1_RX_PIN` and `UART1_TX_PIN` in `platformio.ini` if your wiring differs
- The default UART FIFO threshold is HIGH (`UART_FIFO_THRESHOLD=96`), matching the ESP32-S3 high-baud profile
- The onboard addressable RGB LED on `IO8` follows the ESP32-S3 status color scheme: red for disconnected, orange for AP mode, green for STA connected, purple for TCP connected, blue pulses for data activity, and green blinking during Wi-Fi scans
- ESP32-C6 exposes one FreeRTOS application core plus an LP core. The LP core is for low-power wake and simple monitoring workflows, not for running Arduino tasks or offloading the TCP/UART bridge
- The bridge buffers intentionally follow the ESP32-C3 static SRAM model because common ESP32-C6-DevKitC-1 boards do not provide PSRAM
- Wi-Fi TX power is not configured by default on ESP32-C6; enable `CONFIGURE_WIFI_TX_POWER=1` and set `WIFI_TX_POWER` in `platformio.ini` if your deployment needs an explicit power level

## License

This project is licensed under the GNU General Public License v3.0.

See:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`
