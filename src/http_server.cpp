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

void sendJsonDocument(int statusCode, JsonDocument &doc) {
  String response;
  serializeJson(doc, response);
  server.send(statusCode, "application/json", response);
}

void sendJsonError(int statusCode, const String &message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJsonDocument(statusCode, doc);
}

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

void addWiFiScanResultsJson(JsonDocument &doc) {
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

void addUartSettingsJson(JsonDocument &doc) {
  const UartSettings &settings = uart_port::settings();
  doc["baudRate"] = settings.baudRate;
  doc["dataBits"] = settings.dataBits;
  char parity[2] = {settings.parity, '\0'};
  doc["parity"] = parity;
  doc["stopBits"] = settings.stopBits;
}

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

void handleConfigPage() {
  server.send(200, "text/html; charset=utf-8", FPSTR(kConfigPageHtml));
}

void handleGetWiFiProfiles() {
  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

void handleGetUartSettings() {
  JsonDocument doc;
  addUartSettingsJson(doc);
  sendJsonDocument(200, doc);
}

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

  String password = server.arg("password");
  if (password.isEmpty() && wifi_profiles::profileInUse(index)) {
    password = wifi_profiles::profile(index).password;
  }

  if (!wifi_profiles::save(index, ssid, password)) {
    sendJsonError(500, "Failed to save Wi-Fi profile");
    return;
  }

  if (server.arg("activate") == "1") {
    wifi_profiles::setActiveIndex(static_cast<int8_t>(index));
    wifi_manager::requestReconfigure();
  }

  JsonDocument doc;
  addWiFiProfilesJson(doc);
  sendJsonDocument(200, doc);
}

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

void handleScanWiFi() {
  wifi_manager::scanNearby();
  JsonDocument doc;
  addWiFiScanResultsJson(doc);
  sendJsonDocument(200, doc);
}

void handleNotFound() {
  sendJsonError(404, "Not found");
}

}  // namespace

void begin() {
  server.on("/", HTTP_GET, handleConfigPage);
  server.on("/api/wifi", HTTP_GET, handleGetWiFiProfiles);
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
