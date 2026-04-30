/**
 * @file   debug_log.h
 * @brief  调试日志宏封装
 *
 * 通过 kEnableDebugLogs 编译开关控制，发布时禁用可减少二进制体积。
 */
#pragma once

#include <Arduino.h>
#include "app_config.h"

namespace wifi_uart {

/**
 * @brief 格式化调试打印，等价于 log_i
 * @param format  printf 格式串
 * @param args    可变参数
 */
template <typename... Args>
void debugPrintf(const char *format, Args... args) {
  if constexpr (kEnableDebugLogs) {
    log_i(format, args...);
  }
}

/**
 * @brief 调试字符串打印（无换行）
 * @param message 待打印字符串
 */
inline void debugPrint(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

/**
 * @brief 调试字符串打印（带回车）
 * @param message 待打印字符串
 */
inline void debugPrintln(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

}