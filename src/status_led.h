/**
 * @file   status_led.h
 * @brief  板载状态灯驱动
 *
 * 支持普通 GPIO 模式（高低电平驱动）和 RGB WS2812 模式。
 * 状态灯通过 StatusLedSnapshot 快照反映设备当前运行状态。
 */
#pragma once

#include "models.h"

namespace wifi_uart {

/**
 * @brief 初始化状态灯硬件
 */
void initStatusLed();

/**
 * @brief 根据设备状态快照更新状态灯
 * @param snapshot 当前设备状态
 *
 * 状态灯含义（RGB 模式）：
 * - 绿色：Wi-Fi STA 已连接
 * - 粉色（红+蓝）：TCP 客户端已连接
 * - 橙色（红+淡绿）：AP 模式激活
 * - 蓝灯闪烁：Wi-Fi 扫描进行中
 * - 蓝灯脉冲：串口有数据活动
 */
void updateStatusLed(const StatusLedSnapshot &snapshot);

}