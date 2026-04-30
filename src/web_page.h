/**
 * @file   web_page.h
 * @brief  配置网页资源声明
 */
#pragma once

#include <Arduino.h>

namespace wifi_uart {

/**
 * @brief 嵌入式配置网页全文（PROGMEM Flash 存储）
 *
 * 网页包含：UART 参数配置、Wi-Fi Profile 管理、附近热点扫描。
 */
extern const char kConfigPageHtml[] PROGMEM;

}