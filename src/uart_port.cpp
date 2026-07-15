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

#include <cinttypes>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <soc/soc_caps.h>

namespace wifi_uart {
namespace uart_port {

namespace {

/** @brief UART1 HardwareSerial 实例（使用 ESP32 第二路 UART） */
HardwareSerial uartPort(1);

/** @brief 当前生效的 UART 参数（内存级副本，不依赖 UART 驱动） */
UartSettings currentSettings = {kUartBaudRate, 8, 'N', 1};
bool uartRunningFlag = false;

bool startUart(const UartSettings &settings, uint32_t serialConfig) {
  uartRunningFlag = false;
  if (uartPort.setRxBufferSize(kUartDriverRxBufferSize) != kUartDriverRxBufferSize) {
    debugPrintln("Failed to configure UART1 RX buffer size");
    return false;
  }
  if (uartPort.setTxBufferSize(kUartDriverTxBufferSize) != kUartDriverTxBufferSize) {
    debugPrintln("Failed to configure UART1 TX buffer size");
    return false;
  }

  uartPort.begin(
      settings.baudRate,
      serialConfig,
      UART1_RX_PIN,
      UART1_TX_PIN,
      false,
      20000UL,
      static_cast<uint8_t>(UART_FIFO_THRESHOLD));
  if (!uartPort || uartPort.baudRate() == 0) {
    debugPrintln("Failed to start UART1 driver");
    uartPort.end();
    return false;
  }
  if (!uartPort.setRxFIFOFull(UART_FIFO_THRESHOLD)) {
    debugPrintln("Failed to configure UART1 RX FIFO threshold");
    uartPort.end();
    return false;
  }

  uartPort.setTimeout(0);
  uartRunningFlag = true;
  return true;
}

uint32_t txDrainTimeoutMs() {
  const uint32_t baudRate = max(currentSettings.baudRate, 300UL);
  const uint64_t queuedBits =
      static_cast<uint64_t>(kUartDriverTxBufferSize + SOC_UART_FIFO_LEN) * 12ULL;
  const uint64_t calculatedMs = (queuedBits * 1000ULL + baudRate - 1) / baudRate + 100ULL;
  return static_cast<uint32_t>(
      calculatedMs > kUartTxDrainTimeoutMs ? calculatedMs : kUartTxDrainTimeoutMs);
}

}  // namespace

HardwareSerial &serial() {
  return uartPort;
}

const UartSettings &settings() {
  return currentSettings;
}

bool isRunning() {
  return uartRunningFlag;
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
 * 变更前有界等待旧 UART TX 队列排空；超时保留旧配置。
 * 新配置启动失败时尽量恢复先前配置。
 *
 * @param settings 新的 UART 参数
 * @return true 应用成功
 */
bool waitForTxDrain(uint32_t maxWaitMs) {
  if (!uartRunningFlag) {
    return false;
  }
  uint32_t timeoutMs = txDrainTimeoutMs();
  if (maxWaitMs > 0 && timeoutMs > maxWaitMs) {
    timeoutMs = maxWaitMs;
  }
  const esp_err_t waitResult = uart_wait_tx_done(
      UART_NUM_1,
      pdMS_TO_TICKS(timeoutMs));
  if (waitResult != ESP_OK) {
    debugPrintf("UART1 TX did not drain: %s\n", esp_err_to_name(waitResult));
    return false;
  }
  return true;
}

bool applySettings(
    const UartSettings &settings,
    BeforeRestartCallback beforeRestart,
    uint32_t maxDrainWaitMs) {
  if (settings.baudRate < 300 || settings.baudRate > SOC_UART_BITRATE_MAX) {
    return false;
  }

  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(settings, serialConfig)) {
    return false;
  }

  const bool wasRunning = uartRunningFlag;
  const UartSettings previousSettings = currentSettings;
  uint32_t previousSerialConfig = SERIAL_8N1;
  if (!mapUartConfig(previousSettings, previousSerialConfig)) {
    return false;
  }

  if (wasRunning) {
    if (!waitForTxDrain(maxDrainWaitMs)) {
      return false;
    }
    if (beforeRestart != nullptr) {
      if (!beforeRestart(uartPort)) {
        return false;
      }
    }
    uartPort.end();
    uartRunningFlag = false;
  }

  if (!startUart(settings, serialConfig)) {
    uartPort.end();
    uartRunningFlag = false;
    if (wasRunning && !startUart(previousSettings, previousSerialConfig)) {
      debugPrintln("Failed to restore previous UART1 configuration");
    }
    return false;
  }

  currentSettings = settings;

  debugPrintf(
      "UART1 reconfigured. Baud=%" PRIu32 ", Data=%u, Parity=%c, Stop=%u\n",
      currentSettings.baudRate,
      currentSettings.dataBits,
      currentSettings.parity,
      currentSettings.stopBits);
  return true;
}

bool discardOutput(BeforeRestartCallback beforeRestart) {
  if (!uartRunningFlag) {
    return false;
  }

  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(currentSettings, serialConfig)) {
    return false;
  }
  if (beforeRestart != nullptr) {
    if (!beforeRestart(uartPort)) {
      return false;
    }
  }
  uartPort.end();
  uartRunningFlag = false;
  return startUart(currentSettings, serialConfig);
}

}
}
