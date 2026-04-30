/**
 * @file   uart_port.cpp
 * @brief  UART1 硬件串口封装
 *
 * 提供 UartSettings ↔ ESP32 serial_config 位域的映射，
 * 以及 HardwareSerial 实例的初始化与参数变更。
 */

#include "uart_port.h"
#include "app_config.h"
#include "debug_log.h"
#include "tcp_uart_bridge.h"

#include <cinttypes>

namespace wifi_uart {
namespace uart_port {

namespace {

/** @brief UART1 HardwareSerial 实例（使用 ESP32 第二路 UART） */
HardwareSerial uartPort(1);

/** @brief 当前生效的 UART 参数（内存级副本，不依赖 UART 驱动） */
UartSettings currentSettings = {kUartBaudRate, 8, 'N', 1};

}  // namespace

HardwareSerial &serial() {
  return uartPort;
}

const UartSettings &settings() {
  return currentSettings;
}

/**
 * @brief 将 UartSettings 结构转换为 ESP32 serial_config 位域
 *
 * ESP32 的 serial_config 使用形如 SERIAL_8N1 的宏编码所有帧参数。
 * 本函数穷举 dataBits×parity×stopBits 的所有合法组合。
 *
 * @param settings     输入 UART 参数
 * @param serialConfig 输出 serial_config 位域
 * @return true 映射成功；false 不支持的参数组合
 */
bool mapUartConfig(const UartSettings &settings, uint32_t &serialConfig) {
  // ESP32 仅支持 1 或 2 停止位
  if (settings.stopBits != 1 && settings.stopBits != 2) {
    return false;
  }

  switch (settings.dataBits) {
    case 5:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5N1 : SERIAL_5N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5E1 : SERIAL_5E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_5O1 : SERIAL_5O2;
        return true;
      }
      return false;
    case 6:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6N1 : SERIAL_6N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6E1 : SERIAL_6E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_6O1 : SERIAL_6O2;
        return true;
      }
      return false;
    case 7:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7N1 : SERIAL_7N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7E1 : SERIAL_7E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_7O1 : SERIAL_7O2;
        return true;
      }
      return false;
    case 8:
      if (settings.parity == 'N') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8N1 : SERIAL_8N2;
        return true;
      }
      if (settings.parity == 'E') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8E1 : SERIAL_8E2;
        return true;
      }
      if (settings.parity == 'O') {
        serialConfig = settings.stopBits == 1 ? SERIAL_8O1 : SERIAL_8O2;
        return true;
      }
      return false;
    default:
      return false;
  }
}

/**
 * @brief 将新 UART 参数应用到硬件
 *
 * 变更前清空桥接缓冲区并 flush UART 发送队列；
 * 变更后等待 UART 硬件 FIFO 排空（内部有 20ms 等待）。
 *
 * @param settings 新的 UART 参数
 * @return true 应用成功
 */
bool applySettings(const UartSettings &settings) {
  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(settings, serialConfig)) {
    return false;
  }

  // 参数变更前先清空 session 缓冲（避免新旧参数混用导致乱码）
  bridge::clearSessionBuffers();
  uartPort.flush();
  uartPort.end();

  // 配置驱动内部缓冲区大小（8192/4092 是平衡内存与吞吐的常用值）
  uartPort.setRxBufferSize(kUartDriverRxBufferSize);
  uartPort.setTxBufferSize(kUartDriverTxBufferSize);

  // 重新打开 UART1：最后两个参数分别是超时不等待和 FIFO 满阈值
  uartPort.begin(
      settings.baudRate,
      serialConfig,
      UART1_RX_PIN,
      UART1_TX_PIN,
      false,       // invert（不翻转信号极性）
      20000UL,     // timeout（等待 FIFO 排空前最多等 20ms）
      kUartRxFifoFullThreshold);  // RX FIFO 满阈值（调控流控）

  uartPort.setTimeout(0);  // readBytes() 立即返回，不阻塞等待
  currentSettings = settings;

  debugPrintf(
      "UART1 reconfigured. Baud=%" PRIu32 ", Data=%u, Parity=%c, Stop=%u\n",
      currentSettings.baudRate,
      currentSettings.dataBits,
      currentSettings.parity,
      currentSettings.stopBits);
  return true;
}

}
}
