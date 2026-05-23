/**
 * @file   wifi_manager.cpp
 * @brief  Wi-Fi 连接状态机与 AP/STA 模式管理
 *
 * 支持两种运行模式：
 * - STA 模式：连接用户指定的 Wi-Fi 网络
 * - AP 模式（fallback）：当无有效凭据或 STA 连接超时时作为热点提供配置界面
 *
 * 内部使用标志位实现状态转换，通过 pendingReconfigure 机制
 * 确保配置变更在下一次主 loop 中安全应用（避免在 HTTP handler 中直接操作 Wi-Fi）。
 */

#include "wifi_manager.h"
#include "app_config.h"
#include "debug_log.h"
#include "tcp_uart_bridge.h"
#include "wifi_profiles.h"

#include <WiFi.h>

namespace wifi_uart {
namespace wifi_manager {

namespace {

/** @brief 最近一次 STA 重连尝试的时间 */
uint32_t lastWifiReconnectAttemptMs = 0;

/** @brief 是否正在使用 STA 模式 */
bool useStationModeFlag = false;

/** @brief AP 模式是否激活 */
bool accessPointActiveFlag = false;

/** @brief STA 此前是否曾成功连接（用于检测断线事件） */
bool stationWasConnected = false;

/** @brief AP fallback 模式下是否应后台重试激活的 STA profile */
bool retryStationFromAccessPointFlag = false;

/** @brief AP fallback 模式下是否已经发起 STA 重试 */
bool stationRetryAttemptActive = false;

/** @brief Wi-Fi 扫描是否进行中 */
bool scanInProgressFlag = false;

/** @brief 当前 Wi-Fi 扫描开始时间，用于超时恢复 */
uint32_t scanStartedAtMs = 0;

/** @brief 是否有待处理的 Wi-Fi 重配置请求 */
bool pendingReconfigure = false;

/** @brief 最近一次 Wi-Fi 扫描结果缓存 */
WiFiScanResult scanResults[kMaxWiFiProfiles] = {};

/** @brief 最近一次扫描的有效结果数量 */
size_t scanResultCountValue = 0;

/** @brief 状态变化回调（用于通知状态灯刷新） */
void (*statusUpdateCallback)() = nullptr;

/** @brief 触发状态更新回调 */
void updateStatus() {
  if (statusUpdateCallback != nullptr) {
    statusUpdateCallback();
  }
}

/** @brief 结束一次异步扫描并缓存结果 */
void finishScan(int16_t count) {
  scanResultCountValue = 0;

  if (count > 0) {
    const size_t cappedCount = min(static_cast<size_t>(count), static_cast<size_t>(kMaxWiFiProfiles));
    for (size_t i = 0; i < cappedCount; ++i) {
      scanResults[i].ssid = WiFi.SSID(i);
      scanResults[i].rssi = WiFi.RSSI(i);
      scanResults[i].channel = static_cast<uint8_t>(WiFi.channel(i));
      scanResults[i].encryption = static_cast<uint8_t>(WiFi.encryptionType(i));
    }
    scanResultCountValue = cappedCount;
  }

  WiFi.scanDelete();

  if (accessPointActiveFlag && !stationRetryAttemptActive) {
    WiFi.mode(WIFI_AP);
  }

  scanInProgressFlag = false;
  scanStartedAtMs = 0;
  updateStatus();
}

/** @brief Wi-Fi 模式切换前取消正在进行的异步扫描 */
void cancelScanIfNeeded() {
  if (!scanInProgressFlag) {
    return;
  }

  WiFi.scanDelete();
  scanInProgressFlag = false;
  scanStartedAtMs = 0;
  scanResultCountValue = 0;
  updateStatus();
}

/** @brief 停止 TCP Server（Wi-Fi 模式切换时调用） */
void stopTcpServer() {
  bridge::stopTcpServer();
}

/** @brief 断开 TCP 客户端（Wi-Fi 切换时调用） */
void disconnectTcpClient(const char *reason) {
  bridge::disconnectTcpClient(reason);
}

/**
 * @brief 启动 TCP Server
 *
 * 仅在网络已就绪（STA 已连接或 AP 已激活）时启动。
 */
void startTcpServer() {
  if (!isNetworkReady()) {
    return;
  }

  if (bridge::startTcpServer()) {
    debugPrintf("TCP bridge listening on port %d\n", TCP_BRIDGE_PORT);
  }
}

/** @brief 打印 STA 连接就绪信息 */
void logStationReady() {
  debugPrintf(
      "STA mode ready. IP: %s, TCP port: %d\n",
      WiFi.localIP().toString().c_str(),
      TCP_BRIDGE_PORT);
}

/**
 * @brief 启动 AP 模式（热点 fallback）
 *
 * 设备切换到 AP_SSID/AP_PASSWORD 热点，IP 固定为 192.168.4.1，
 * 允许用户连接并通过网页配置 Wi-Fi 凭据。
 */
void startAccessPoint() {
  cancelScanIfNeeded();
  useStationModeFlag = false;
  accessPointActiveFlag = false;
  retryStationFromAccessPointFlag = false;
  stationRetryAttemptActive = false;
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

/**
 * @brief 切换到 AP 前先清理 STA 状态
 *
 * 需要：关闭 autoReconnect、disconnect、切换到 NULL 模式，
 * 避免前后模式冲突导致 ESP32 Wi-Fi 栈异常。
 */
void stopStationBeforeAccessPoint() {
  useStationModeFlag = false;
  stationWasConnected = false;
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(WIFI_MODE_NULL);
  delay(100);
}

/**
 * @brief 启动 STA 模式并连接激活的 Wi-Fi Profile
 *
 * 若无激活 Profile（active()==nullptr）则直接返回。
 * 启动前先停止 TCP Server 并断开已有 TCP 客户端。
 */
void startStationMode() {
  const WiFiProfile *activeProfile = wifi_profiles::active();
  if (activeProfile == nullptr) {
    debugPrintln("No active Wi-Fi profile, unable to enter STA mode");
    return;
  }

  cancelScanIfNeeded();
  useStationModeFlag = true;
  accessPointActiveFlag = false;
  retryStationFromAccessPointFlag = false;
  stationRetryAttemptActive = false;
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

/**
 * @brief 等待 STA 首次连接结果
 *
 * 轮询 kWifiConnectTimeoutMs（15s）内是否连接成功：
 * - 成功：记录 stationWasConnected，启动 TCP Server
 * - 超时：切换到 AP fallback 模式
 */
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
    retryStationFromAccessPointFlag = wifi_profiles::active() != nullptr;
    lastWifiReconnectAttemptMs = millis();
    startTcpServer();
  }
}

/** @brief AP fallback 保持可访问，同时周期性后台重试激活的 STA profile */
void handleAccessPointStationRetry() {
  if (!accessPointActiveFlag || !retryStationFromAccessPointFlag) {
    return;
  }

  const WiFiProfile *activeProfile = wifi_profiles::active();
  if (activeProfile == nullptr) {
    retryStationFromAccessPointFlag = false;
    stationRetryAttemptActive = false;
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    debugPrintln("STA recovered from AP fallback");
    cancelScanIfNeeded();
    if (bridge::isTcpClientConnected()) {
      disconnectTcpClient("sta recovered from ap fallback");
    }
    stopTcpServer();
    WiFi.softAPdisconnect(true);
    delay(100);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    accessPointActiveFlag = false;
    useStationModeFlag = true;
    retryStationFromAccessPointFlag = false;
    stationRetryAttemptActive = false;
    stationWasConnected = true;
    bridge::markWifiReconnect();
    logStationReady();
    startTcpServer();
    updateStatus();
    return;
  }

  if (scanInProgressFlag || millis() - lastWifiReconnectAttemptMs < kWifiFallbackRetryIntervalMs) {
    return;
  }

  debugPrintln("Retrying WiFi connection from AP fallback");
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect(false, false);
  WiFi.begin(activeProfile->ssid.c_str(), activeProfile->password.c_str());
  stationRetryAttemptActive = true;
  lastWifiReconnectAttemptMs = millis();
}

}  // namespace

/**
 * @brief 初始化 Wi-Fi 管理器
 * @param callback 状态变化时调用的回调（可为 nullptr）
 *
 * 若存在激活的 Wi-Fi Profile 则启动 STA 模式并等待连接；
 * 否则直接启动 AP 模式。无论哪种模式，最终都会启动 TCP Server。
 */
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

/**
 * @brief STA 模式主循环处理
 *
 * 检测连接/断线事件：
 * - 断线后：停止 TCP Server，断开 TCP 客户端
 * - 断线期间：每 kWifiReconnectIntervalMs 尝试一次重连
 */
void handleStationMode() {
  if (!useStationModeFlag) {
    handleAccessPointStationRetry();
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

/**
 * @brief 执行待处理的 Wi-Fi 重配置
 *
 * 由 HTTP Server 的 Profile 保存/激活/删除操作触发。
 * 在主循环中执行，而非 HTTP handler 回调中（避免 Wi-Fi 操作阻塞 HTTP 响应）。
 */
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

/**
 * @brief 触发异步 Wi-Fi 扫描
 *
 * 扫描期间若 AP 已激活，则临时切换到 AP_STA 混合模式，
 * 扫描结束后恢复纯 AP 模式，保证设备在扫描期间仍可被访问。
 */
void scanNearby() {
  if (scanInProgressFlag) {
    return;
  }

  scanInProgressFlag = true;
  scanStartedAtMs = millis();
  scanResultCountValue = 0;
  updateStatus();

  if (accessPointActiveFlag) {
    WiFi.mode(WIFI_AP_STA);
  }

  const int16_t count = WiFi.scanNetworks(true, true);
  if (count == WIFI_SCAN_RUNNING) {
    return;
  }

  finishScan(count);
}

void pollScan() {
  if (!scanInProgressFlag) {
    return;
  }

  const int16_t count = WiFi.scanComplete();
  if (count == WIFI_SCAN_RUNNING) {
    if (millis() - scanStartedAtMs >= kWifiScanTimeoutMs) {
      debugPrintln("WiFi scan timed out");
      finishScan(WIFI_SCAN_FAILED);
    }
    return;
  }

  finishScan(count);
}

size_t scanResultCount() {
  return scanResultCountValue;
}

const WiFiScanResult &scanResult(size_t index) {
  return scanResults[index];
}

}
}
