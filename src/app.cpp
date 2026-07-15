/**
 * @file   app.cpp
 * @brief  应用层初始化与主事件循环
 *
 * 编排所有子模块的启动顺序，并在 loop() 中顺序执行各模块的
 * 事件处理（Wi-Fi 状态维护、HTTP 服务、TCP-UART 桥接、状态灯刷新）。
 */

#include "app.h"

#include "app_config.h"
#include "debug_log.h"
#include "http_server.h"
#include "rfc2217_bridge.h"
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

/**
 * @brief 采集当前系统状态并刷新状态灯
 *
 * 快照包含：Wi-Fi 连接状态、AP 状态、TCP 客户端连接状态、
 * 扫描状态以及最近活动时间戳。
 */
void showStatusLed() {
  StatusLedSnapshot snapshot;
  snapshot.wifiConnected = wifi_manager::wifiConnected();
  snapshot.accessPointActive = wifi_manager::accessPointActive();
  snapshot.tcpClientConnected = bridge::isTcpClientConnected() || rfc2217_bridge::isClientConnected();
  snapshot.wifiScanInProgress = wifi_manager::scanInProgress();
  snapshot.lastActivityAtMs = bridge::lastActivityMs();
  updateStatusLed(snapshot);
}

}  // namespace

void setup() {
  // 仅在调试日志开启时初始化调试串口
  if constexpr (kEnableDebugLogs) {
    Serial.begin(kDebugBaudRate);
  }

  // 初始化 NVS Wi-Fi 凭据存储；失败时重启
  if (!wifi_profiles::begin()) {
    debugPrintln("Failed to initialize Preferences. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  // 初始化 TCP↔UART 环形缓冲区；分配失败时重启
  if (!bridge::beginBuffers()) {
    debugPrintln("Failed to allocate bridge buffers. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  if (!rfc2217_bridge::beginBuffers()) {
    debugPrintln("Failed to allocate RFC2217 buffers. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  // 初始化 UART1 硬件串口；失败时重启
  if (!uart_port::applySettings(uart_port::settings())) {
    debugPrintln("Failed to initialize UART1. Restarting in 2 seconds.");
    delay(2000);
    ESP.restart();
  }

  initStatusLed();
  showStatusLed();

  // 关闭 Wi-Fi 睡眠以降低延迟；可选配置发射功率
  WiFi.setSleep(false);
#if CONFIGURE_WIFI_TX_POWER
  WiFi.setTxPower(kWifiTxPower);
#endif

  // 启动 Wi-Fi 管理器（内部启动 TCP Server）和 HTTP 服务器
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
  // Wi-Fi STA 断线检测与重连
  wifi_manager::handleStationMode();

  // 异步 Wi-Fi 扫描完成检测
  wifi_manager::pollScan();

  // HTTP 请求处理（配置网页 + JSON API）
  http_server::handleClient();

  // 应用待处理的 Wi-Fi 重配置（Profile 切换后触发）
  wifi_manager::applyPendingReconfigureIfNeeded();

  HardwareSerial &uartPort = uart_port::serial();

  // 无活动会话时先丢弃旧 UART 输入，避免新客户端收到连接前残留数据。
  if (!bridge::isTcpClientConnected() && !rfc2217_bridge::isClientConnected()) {
    bridge::pullUartIntoBuffer(uartPort);
  }

  // RFC2217 与原始 TCP 桥接互斥使用同一个 UART
  rfc2217_bridge::acceptClientIfNeeded(uartPort);
  if (rfc2217_bridge::isClientConnected()) {
    rfc2217_bridge::handleClient(uartPort);
  } else {
    // TCP Server 接受新客户端
    bridge::acceptClientIfNeeded();

    // TCP → UART 数据通路
    bridge::pullTcpIntoBuffer();
    bridge::flushTcpBufferToUart(uartPort);

    // UART → TCP 数据通路
    bridge::pullUartIntoBuffer(uartPort);
    bridge::flushUartBufferToTcp();
  }

  // 定期打印桥接统计信息
  bridge::logStatsIfNeeded(
      wifi_manager::wifiConnected(),
      wifi_manager::accessPointActive(),
      AP_SSID);

  // 刷新状态灯
  showStatusLed();

  // ~1ms 循环周期，yield 给 Wi-Fi 栈处理 ISR 事件
  delay(1);
}

}
}
