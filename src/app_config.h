#pragma once

#include <Arduino.h>
#include <cstddef>
#include <cstdint>

#ifndef UART1_RX_PIN
#define UART1_RX_PIN 18
#endif

#ifndef UART1_TX_PIN
#define UART1_TX_PIN 17
#endif

#ifndef STATUS_LED_PIN
#if defined(RGB_BUILTIN)
#define STATUS_LED_PIN RGB_BUILTIN
#else
#define STATUS_LED_PIN 8
#endif
#endif

#ifndef USE_RGB_STATUS_LED
#define USE_RGB_STATUS_LED 0
#endif

#ifndef STATUS_RGB_BRIGHTNESS
#define STATUS_RGB_BRIGHTNESS 16
#endif

#ifndef STATUS_LED_ACTIVE_LEVEL
#define STATUS_LED_ACTIVE_LEVEL 0
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef AP_SSID
#define AP_SSID "ESP32C6-UART"
#endif

#ifndef AP_PASSWORD
#define AP_PASSWORD "12345678"
#endif

#ifndef TCP_BRIDGE_PORT
#define TCP_BRIDGE_PORT 6638
#endif

#ifndef CONFIGURE_WIFI_TX_POWER
#define CONFIGURE_WIFI_TX_POWER 0
#endif

#ifndef WIFI_TX_POWER
#define WIFI_TX_POWER WIFI_POWER_8_5dBm
#endif

constexpr uint32_t kDebugBaudRate = 115200;
constexpr uint32_t kUartBaudRate = 115200;
constexpr bool kEnableDebugLogs = false;

constexpr uint32_t kWifiConnectTimeoutMs = 15000;
constexpr uint32_t kWifiReconnectIntervalMs = 3000;
#if CONFIGURE_WIFI_TX_POWER
constexpr wifi_power_t kWifiTxPower = WIFI_TX_POWER;
#endif
constexpr uint32_t kUartBackpressureLogIntervalMs = 2000;
constexpr uint32_t kStatsLogIntervalMs = 10000;
constexpr uint32_t kDisconnectedBlinkPeriodMs = 140;
constexpr uint32_t kWifiBlinkPeriodMs = 700;
constexpr uint32_t kDataFlashWindowMs = 120;
constexpr uint32_t kActivityFlashWindowMs = 220;
constexpr uint32_t kActivityPulseMs = 45;
constexpr uint32_t kActivityPulseGapMs = 55;
constexpr uint8_t kUartRxFifoFullThreshold = 112;
constexpr size_t kIoChunkSize = 256;
constexpr size_t kPendingTcpToUartBytes = 12288;
constexpr size_t kPendingUartToTcpBytes = 20480;
constexpr size_t kUartDriverRxBufferSize = 8192;
constexpr size_t kUartDriverTxBufferSize = 4096;
constexpr uint8_t kMaxWiFiProfiles = 24;
constexpr uint16_t kHttpPort = 80;
