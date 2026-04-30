/**
 * @file   models.h
 * @brief  跨模块共享数据结构定义
 */
#pragma once

#include <Arduino.h>
#include <cstdint>

namespace wifi_uart {

/**
 * @brief UART 串口参数
 */
struct UartSettings {
  uint32_t baudRate;  ///< 波特率
  uint8_t dataBits;   ///< 数据位（5-8）
  char parity;        ///< 校验位：'N','E','O'
  uint8_t stopBits;  ///< 停止位（1 或 2）
};

/**
 * @brief Wi-Fi 连接凭据
 */
struct WiFiProfile {
  bool inUse;         ///< 该槽位是否已存储有效凭据
  String ssid;        ///< 网络名称
  String password;    ///< 密码（可为空白字符串表示开放网络）
};

/**
 * @brief Wi-Fi 扫描结果条目
 */
struct WiFiScanResult {
  String ssid;         ///< 网络名称
  int32_t rssi;       ///< 信号强度（dBm）
  uint8_t channel;    ///< 所在信道
  uint8_t encryption; ///< 加密类型（esp32 WiFiAuthMode_t）
};

/**
 * @brief RGB 颜色分量
 */
struct RgbColor {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

/**
 * @brief 状态灯显示状态快照
 */
struct StatusLedSnapshot {
  bool wifiConnected;        ///< Wi-Fi STA 是否已连接
  bool accessPointActive;    ///< AP 模式是否激活
  bool tcpClientConnected;   ///< 是否有 TCP 客户端已连接
  bool wifiScanInProgress;   ///< Wi-Fi 扫描是否进行中
  uint32_t lastActivityAtMs; ///< 最近一次通信活动的时间戳（millis）
};

}