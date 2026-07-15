/**
 * @file   uart_port.h
 * @brief  UART1 硬件串口封装
 */
#pragma once

#include "models.h"

namespace wifi_uart {
namespace uart_port {

/** @brief 获取 HardwareSerial 实例（UART1） */
HardwareSerial &serial();

/** @brief 获取当前生效的 UART 参数 */
const UartSettings &settings();

/** @brief UART1 驱动当前是否已成功启动 */
bool isRunning();

/**
 * @brief 将 UartSettings 映射为 ESP32 serial_config 位域值
 * @param settings     UART 参数
 * @param serialConfig 输出：映射后的 serial_config 值
 * @return true 映射成功
 */
bool mapUartConfig(const UartSettings &settings, uint32_t &serialConfig);

/**
 * @brief 应用新的 UART 参数到硬件
 * @param settings 新的 UART 参数
 * @return true 应用成功
 *
 * 会清空桥接 session 缓冲区并重启 UART1 驱动。
 */
bool applySettings(const UartSettings &settings);

}
}
