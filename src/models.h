#pragma once

#include <Arduino.h>
#include <cstdint>

namespace wifi_uart {

struct UartSettings {
  uint32_t baudRate;
  uint8_t dataBits;
  char parity;
  uint8_t stopBits;
};

struct WiFiProfile {
  bool inUse;
  String ssid;
  String password;
};

struct WiFiScanResult {
  String ssid;
  int32_t rssi;
  uint8_t channel;
  uint8_t encryption;
};

struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

struct StatusLedSnapshot {
  bool wifiConnected;
  bool accessPointActive;
  bool tcpClientConnected;
  bool wifiScanInProgress;
  uint32_t lastActivityAtMs;
};

}