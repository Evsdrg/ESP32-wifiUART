#include "wifi_manager.h"
#include "app_config.h"
#include "debug_log.h"
#include "tcp_uart_bridge.h"
#include "wifi_profiles.h"

#include <WiFi.h>

namespace wifi_uart {
namespace wifi_manager {

namespace {

uint32_t lastWifiReconnectAttemptMs = 0;
bool useStationModeFlag = false;
bool accessPointActiveFlag = false;
bool stationWasConnected = false;
bool scanInProgressFlag = false;
bool pendingReconfigure = false;
WiFiScanResult scanResults[kMaxWiFiProfiles] = {};
size_t scanResultCountValue = 0;
void (*statusUpdateCallback)() = nullptr;

void updateStatus() {
  if (statusUpdateCallback != nullptr) {
    statusUpdateCallback();
  }
}

void stopTcpServer() {
  bridge::stopTcpServer();
}

void disconnectTcpClient(const char *reason) {
  bridge::disconnectTcpClient(reason);
}

void startTcpServer() {
  if (!isNetworkReady()) {
    return;
  }

  if (bridge::startTcpServer()) {
    debugPrintf("TCP bridge listening on port %d\n", TCP_BRIDGE_PORT);
  }
}

void logStationReady() {
  debugPrintf(
      "STA mode ready. IP: %s, TCP port: %d\n",
      WiFi.localIP().toString().c_str(),
      TCP_BRIDGE_PORT);
}

void startAccessPoint() {
  useStationModeFlag = false;
  accessPointActiveFlag = false;
  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    debugPrintln("Failed to start WiFi AP");
    return;
  }

  accessPointActiveFlag = true;
  debugPrintf(
      "AP mode ready. SSID: %s, password: %s, IP: %s, TCP port: %d\n",
      AP_SSID,
      AP_PASSWORD,
      WiFi.softAPIP().toString().c_str(),
      TCP_BRIDGE_PORT);
}

void stopStationBeforeAccessPoint() {
  useStationModeFlag = false;
  stationWasConnected = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_MODE_NULL);
  delay(100);
}

void startStationMode() {
  const WiFiProfile *activeProfile = wifi_profiles::active();
  if (activeProfile == nullptr) {
    debugPrintln("No active Wi-Fi profile, unable to enter STA mode");
    return;
  }

  useStationModeFlag = true;
  accessPointActiveFlag = false;
  stationWasConnected = false;
  stopTcpServer();
  if (bridge::isTcpClientConnected()) {
    disconnectTcpClient("wifi profile change");
  }
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(activeProfile->ssid.c_str(), activeProfile->password.c_str());
  lastWifiReconnectAttemptMs = millis();
  debugPrintf("Connecting to WiFi SSID: %s\n", activeProfile->ssid.c_str());
}

void waitForInitialStationConnection() {
  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < kWifiConnectTimeoutMs) {
    delay(250);
    debugPrint(".");
    updateStatus();
  }
  debugPrintln("");

  if (WiFi.status() == WL_CONNECTED) {
    stationWasConnected = true;
    bridge::markWifiReconnect();
    logStationReady();
    startTcpServer();
  } else {
    debugPrintln("Initial WiFi connect timed out, falling back to AP mode");
    stopStationBeforeAccessPoint();
    startAccessPoint();
    startTcpServer();
  }
}

}  // namespace

void begin(void (*callback)()) {
  statusUpdateCallback = callback;
  if (wifi_profiles::active() != nullptr) {
    startStationMode();
    waitForInitialStationConnection();
  } else {
    startAccessPoint();
    startTcpServer();
  }
}

void handleStationMode() {
  if (!useStationModeFlag) {
    return;
  }

  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !stationWasConnected) {
    stationWasConnected = true;
    bridge::markWifiReconnect();
    logStationReady();
    startTcpServer();
  }

  if (!connected && stationWasConnected) {
    stationWasConnected = false;
    debugPrintln("WiFi disconnected");
    stopTcpServer();
    if (bridge::isTcpClientConnected()) {
      disconnectTcpClient("wifi lost");
    }
  }

  if (!connected && millis() - lastWifiReconnectAttemptMs >= kWifiReconnectIntervalMs) {
    debugPrintln("Retrying WiFi connection");
    if (!WiFi.reconnect()) {
      const WiFiProfile *activeProfile = wifi_profiles::active();
      if (activeProfile != nullptr) {
        WiFi.disconnect(false, false);
        WiFi.begin(activeProfile->ssid.c_str(), activeProfile->password.c_str());
      }
    }
    lastWifiReconnectAttemptMs = millis();
  }
}

void requestReconfigure() {
  pendingReconfigure = true;
}

void applyPendingReconfigureIfNeeded() {
  if (!pendingReconfigure) {
    return;
  }

  pendingReconfigure = false;
  if (wifi_profiles::active() != nullptr) {
    startStationMode();
    waitForInitialStationConnection();
    return;
  }

  stopTcpServer();
  if (bridge::isTcpClientConnected()) {
    disconnectTcpClient("wifi profile removed");
  }
  stopStationBeforeAccessPoint();
  startAccessPoint();
  startTcpServer();
}

bool isNetworkReady() {
  return accessPointActiveFlag || WiFi.status() == WL_CONNECTED;
}

bool accessPointActive() {
  return accessPointActiveFlag;
}

bool scanInProgress() {
  return scanInProgressFlag;
}

bool useStationMode() {
  return useStationModeFlag;
}

bool wifiConnected() {
  return WiFi.status() == WL_CONNECTED;
}

String connectedSsid() {
  return wifiConnected() ? WiFi.SSID() : String();
}

String ipAddress() {
  return accessPointActiveFlag ? WiFi.softAPIP().toString() : (wifiConnected() ? WiFi.localIP().toString() : String());
}

String securityLabel(uint8_t encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN:
      return "Open";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
      return "WAPI";
    default:
      return "Unknown";
  }
}

void scanNearby() {
  scanInProgressFlag = true;
  updateStatus();

  if (accessPointActiveFlag) {
    WiFi.mode(WIFI_AP_STA);
  }

  scanResultCountValue = 0;
  int16_t count = WiFi.scanNetworks(true, true);
  while (count == WIFI_SCAN_RUNNING) {
    delay(30);
    updateStatus();
    count = WiFi.scanComplete();
  }
  if (count <= 0) {
    WiFi.scanDelete();
    if (accessPointActiveFlag) {
      WiFi.mode(WIFI_AP);
    }
    scanInProgressFlag = false;
    updateStatus();
    return;
  }

  const size_t cappedCount = min(static_cast<size_t>(count), static_cast<size_t>(kMaxWiFiProfiles));
  for (size_t i = 0; i < cappedCount; ++i) {
    scanResults[i].ssid = WiFi.SSID(i);
    scanResults[i].rssi = WiFi.RSSI(i);
    scanResults[i].channel = static_cast<uint8_t>(WiFi.channel(i));
    scanResults[i].encryption = static_cast<uint8_t>(WiFi.encryptionType(i));
  }
  scanResultCountValue = cappedCount;
  WiFi.scanDelete();

  if (accessPointActiveFlag) {
    WiFi.mode(WIFI_AP);
  }

  scanInProgressFlag = false;
  updateStatus();
}

size_t scanResultCount() {
  return scanResultCountValue;
}

const WiFiScanResult &scanResult(size_t index) {
  return scanResults[index];
}

}
}
