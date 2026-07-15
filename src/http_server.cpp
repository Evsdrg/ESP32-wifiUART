/**
 * @file   http_server.cpp
 * @brief  HTTP 服务器实现
 *
 * 提供：
 * - GET / : 嵌入式配置网页（HTML）
 * - RESTful JSON API：Wi-Fi Profile 管理、UART 参数配置、Wi-Fi 扫描
 *
 * Wi-Fi 变更操作（保存/激活/删除 Profile）并不立即执行，
 * 而是通过 requestReconfigure() 标记 pendingReconfigure=true，
 * 由主循环中的 applyPendingReconfigureIfNeeded() 统一下发，
 * 避免在 HTTP handler 中直接操作 Wi-Fi 栈导致的死锁或异常。
 */

#include "http_server.h"
#include "app_config.h"
#include "debug_log.h"
#include "uart_port.h"
#include "web_page.h"
#include "wifi_manager.h"
#include "wifi_profiles.h"

#include <ArduinoJson.h>
#include <WebServer.h>

namespace wifi_uart {
namespace http_server {

namespace {

WebServer server(kHttpPort);

/**
 * @brief 发送 JSON 响应
 * @param statusCode HTTP 状态码
 * @param doc        ArduinoJson 文档（由调用者管理生命周期）
 */
void sendJsonDocument(int statusCode, JsonDocument &doc) {
  String response;
  serializeJson(doc, response);
  server.send(statusCode, "application/json", response);
}

/**
 * @brief 发送 JSON 错误响应
 * @param statusCode HTTP 状态码
 * @param message     错误描述字符串
 */
void sendJsonError(int statusCode, const String &message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJsonDocument(statusCode, doc);
}

/**
 * @brief 向 JSON 文档填充 Wi-Fi Profile 信息
 *
 * 包含：激活索引、连接状态、AP 状态、IP、SSID、以及所有已用槽位列表。
 */
void addWiFiProfilesJson(JsonDocument &doc) {
  const bool wifiConnected = wifi_manager::wifiConnected();
  doc["activeIndex"] = wifi_profiles::activeIndex();
  doc["connected"] = wifiConnected;
  doc["apActive"] = wifi_manager::accessPointActive();
  doc["connectedSsid"] = wifi_manager::connectedSsid();
  doc["ip"] = wifi_manager::ipAddress();

  JsonArray profiles = doc["profiles"].to<JsonArray>();
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (!wifi_profiles::profileInUse(i)) {
      continue;
    }

    JsonObject profile = profiles.add<JsonObject>();
    profile["index"] = i;
    profile["ssid"] = wifi_profiles::profile(i).ssid;
    profile["active"] = wifi_profiles::activeIndex() == static_cast<int8_t>(i);
  }
}

/** @brief 向 JSON 文档填充最近一次 Wi-Fi 扫描结果 */
void addWiFiScanResultsJson(JsonDocument &doc) {
  doc["scanning"] = wifi_manager::scanInProgress();
  JsonArray networks = doc["networks"].to<JsonArray>();
  for (size_t i = 0; i < wifi_manager::scanResultCount(); ++i) {
    const WiFiScanResult &scanResult = wifi_manager::scanResult(i);
    JsonObject network = networks.add<JsonObject>();
    network["ssid"] = scanResult.ssid;
    network["rssi"] = scanResult.rssi;
    network["channel"] = scanResult.channel;
    network["security"] = wifi_manager::securityLabel(scanResult.encryption);
    network["open"] = scanResult.encryption == WIFI_AUTH_OPEN;
  }
}

/** @brief 向 JSON 文档填充当前 UART 参数 */
void addUartSettingsJson(JsonDocument &doc) {
  const UartSettings &settings = uart_port::settings();
  doc["baudRate"] = settings.baudRate;
  doc["dataBits"] = settings.dataBits;
  char parity[2] = {settings.parity, '\0'};
  doc["parity"] = parity;
  doc["stopBits"] = settings.stopBits;
}

/**
 * @brief 从 HTTP 请求参数解析 UART 设置
 *
 * 校验范围：baudRate >= 300，dataBits 5-8，parity 单字符，stopBits 1 或 2。
 * 同时验证参数组合是否可被 mapUartConfig 接受。
 *
 * @param settings 输出：解析后的 UART 参数
 * @param error   输出：错误描述
 * @return true 解析成功
 */
bool parseUartSettingsFromRequest(UartSettings &settings, String &error) {
  if (!server.hasArg("baudRate") || !server.hasArg("dataBits") ||
      !server.hasArg("parity") || !server.hasArg("stopBits")) {
    error = "Missing required UART parameters";
    return false;
  }

  const uint32_t baudRate = static_cast<uint32_t>(server.arg("baudRate").toInt());
  const uint8_t dataBits = static_cast<uint8_t>(server.arg("dataBits").toInt());
  const String parityArg = server.arg("parity");
  const uint8_t stopBits = static_cast<uint8_t>(server.arg("stopBits").toInt());

  if (baudRate < 300) {
    error = "Baud rate must be >= 300";
    return false;
  }

  if (parityArg.length() != 1) {
    error = "Parity must be one of N, E, or O";
    return false;
  }

  settings = {baudRate, dataBits, static_cast<char>(toupper(parityArg[0])), stopBits};
  uint32_t serialConfig = SERIAL_8N1;
  if (!uart_port::mapUartConfig(settings, serialConfig)) {
    error = "Unsupported UART framing combination";
    return false;
  }

  return true;
}

/**
 * @brief 从 HTTP 请求参数解析 Profile 槽位编号
 *
 * @param index 输出：槽位编号
 * @param error 输出：错误描述
 * @return true 解析成功
 */
bool parseProfileIndexArg(uint8_t &index, String &error) {
  if (!server.hasArg("index")) {
    error = "Missing profile index";
    return false;
  }

  const int parsed = server.arg("index").toInt();
  if (parsed < 0 || parsed >= kMaxWiFiProfiles) {
    error = "Profile index out of range";
    return false;
  }

  index = static_cast<uint8_t>(parsed);
  return true;
}

/** @brief GET / : 返回嵌入式配置网页 */
void handleConfigPage() {
  server.send(200, "text/html; charset=utf-8", FPSTR(kConfigPageHtml));
}

/** @brief GET /api/wifi : 返回 Wi-Fi 状态及 Profile 列表 */
void handleGetWiFiProfiles() {
  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/** @brief GET /api/uart : 返回当前 UART 参数 */
void handleGetUartSettings() {
  JsonDocument doc;
  addUartSettingsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief POST /api/uart : 应用新 UART 参数
 *
 * 解析请求参数 → 校验合法性 → 调用 applySettings → 返回新参数。
 */
void handleSetUartSettings() {
  UartSettings requested = uart_port::settings();
  String error;
  if (!parseUartSettingsFromRequest(requested, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!uart_port::applySettings(requested)) {
    sendJsonError(400, "Failed to apply UART settings");
    return;
  }

  JsonDocument doc;
  addUartSettingsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief POST /api/wifi/save : 保存凭据到指定槽位
 *
 * 若请求中 activate=1，同时切换激活槽位并触发 Wi-Fi 重配置。
 */
void handleSaveWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!server.hasArg("ssid")) {
    sendJsonError(400, "Missing Wi-Fi name");
    return;
  }

  const String ssid = server.arg("ssid");
  if (ssid.isEmpty()) {
    sendJsonError(400, "Wi-Fi name cannot be empty");
    return;
  }

  // 密码为空时默认兼容旧调用方保留旧密码；显式 keepPassword=0 时才清空。
  String password = server.arg("password");
  const bool keepExistingPassword = !server.hasArg("keepPassword") || server.arg("keepPassword") == "1";
  if (password.isEmpty() && keepExistingPassword && wifi_profiles::profileInUse(index)) {
    password = wifi_profiles::profile(index).password;
  }

  if (!wifi_profiles::save(index, ssid, password)) {
    sendJsonError(500, "Failed to save Wi-Fi profile");
    return;
  }

  if (server.arg("activate") == "1") {
    wifi_profiles::setActiveIndex(static_cast<int8_t>(index));
    wifi_manager::requestReconfigure();  // 标记，等待主 loop 执行
  }

  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief POST /api/wifi/activate : 激活指定槽位 Profile
 *
 * 激活后立即触发 Wi-Fi 重配置（标记方式，非立即执行）。
 */
void handleActivateWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!wifi_profiles::profileInUse(index) || !wifi_profiles::setActiveIndex(static_cast<int8_t>(index))) {
    sendJsonError(400, "Selected profile does not exist");
    return;
  }

  wifi_manager::requestReconfigure();
  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief POST /api/wifi/delete : 删除指定槽位 Profile
 *
 * 若删除的是当前激活槽位，触发 Wi-Fi 重配置（切换到 AP fallback）。
 */
void handleDeleteWiFiProfile() {
  uint8_t index = 0;
  String error;
  if (!parseProfileIndexArg(index, error)) {
    sendJsonError(400, error);
    return;
  }

  if (!wifi_profiles::profileInUse(index)) {
    sendJsonError(400, "Selected profile does not exist");
    return;
  }

  const bool deletedActiveProfile = wifi_profiles::activeIndex() == static_cast<int8_t>(index);
  wifi_profiles::remove(index);
  if (deletedActiveProfile) {
    wifi_manager::requestReconfigure();
  }

  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

/** @brief GET /api/wifi/scan : 返回当前扫描状态和最近结果 */
void handleGetWiFiScan() {
  JsonDocument doc;
  addWiFiScanResultsJson(doc);
  sendJsonDocument(200, doc);
}

/**
 * @brief POST /api/wifi/scan : 触发异步 Wi-Fi 扫描并返回当前状态
 *
 * 立即返回（scanning=true 表示后台扫描进行中），
 * 前端通过 GET /api/wifi/scan 轮询直到 scanning=false 再读取结果。
 */
void handleScanWiFi() {
  wifi_manager::scanNearby();
  JsonDocument doc;
  addWiFiScanResultsJson(doc);
  sendJsonDocument(200, doc);
}

/** @brief 处理所有未匹配路由：返回 404 JSON 错误 */
void handleNotFound() {
  sendJsonError(404, "Not found");
}

}  // namespace

void begin() {
  // 注册所有路由
  server.on("/", HTTP_GET, handleConfigPage);
  server.on("/api/wifi", HTTP_GET, handleGetWiFiProfiles);
  server.on("/api/wifi/scan", HTTP_GET, handleGetWiFiScan);
  server.on("/api/wifi/scan", HTTP_POST, handleScanWiFi);
  server.on("/api/wifi/save", HTTP_POST, handleSaveWiFiProfile);
  server.on("/api/wifi/activate", HTTP_POST, handleActivateWiFiProfile);
  server.on("/api/wifi/delete", HTTP_POST, handleDeleteWiFiProfile);
  server.on("/api/uart", HTTP_GET, handleGetUartSettings);
  server.on("/api/uart", HTTP_POST, handleSetUartSettings);
  server.onNotFound(handleNotFound);

  server.begin();
  debugPrintf("HTTP config page listening on port %u\n", kHttpPort);
}

void handleClient() {
  server.handleClient();
}

}
}
