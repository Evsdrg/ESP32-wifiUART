#include "app.h"

#include "app_config.h"
#include "debug_log.h"
#include "http_server.h"
#include "status_led.h"
#include "tcp_uart_bridge.h"
#include "uart_port.h"
#include "wifi_manager.h"
#include "wifi_profiles.h"

#include <Arduino.h>
#include <WiFi.h>

namespace wifi_uart {
namespace app {

namespace {

void showStatusLed() {
  StatusLedSnapshot snapshot;
  snapshot.wifiConnected = wifi_manager::wifiConnected();
  snapshot.accessPointActive = wifi_manager::accessPointActive();
  snapshot.tcpClientConnected = bridge::isTcpClientConnected();
  snapshot.wifiScanInProgress = wifi_manager::scanInProgress();
  snapshot.lastActivityAtMs = bridge::lastActivityMs();
  updateStatusLed(snapshot);
}

}  // namespace

void setup() {
  if constexpr (kEnableDebugLogs) {
    Serial.begin(kDebugBaudRate);
  }
  if (!wifi_profiles::begin()) {
    debugPrintln("Failed to initialize Preferences. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  if (!bridge::beginBuffers()) {
    debugPrintln("Failed to allocate bridge buffers. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  if (!uart_port::applySettings(uart_port::settings())) {
    debugPrintln("Failed to initialize UART1. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  initStatusLed();
  showStatusLed();

  WiFi.setSleep(false);
#if CONFIGURE_WIFI_TX_POWER
  WiFi.setTxPower(kWifiTxPower);
#endif
  wifi_manager::begin(showStatusLed);
  http_server::begin();

  debugPrintf(
      "UART1 bridge ready. RX=%d, TX=%d, Baud=%" PRIu32 ", %u%c%u\n",
      UART1_RX_PIN,
      UART1_TX_PIN,
      uart_port::settings().baudRate,
      uart_port::settings().dataBits,
      uart_port::settings().parity,
      uart_port::settings().stopBits);
}

void loop() {
  wifi_manager::handleStationMode();

  http_server::handleClient();
  wifi_manager::applyPendingReconfigureIfNeeded();
  bridge::acceptClientIfNeeded();
  bridge::pullTcpIntoBuffer();
  bridge::flushTcpBufferToUart(uart_port::serial());
  bridge::pullUartIntoBuffer(uart_port::serial());
  bridge::flushUartBufferToTcp();
  bridge::logStatsIfNeeded(wifi_manager::wifiConnected(), wifi_manager::accessPointActive(), AP_SSID);
  showStatusLed();
  delay(1);
}

}
}
