#pragma once

#include <Arduino.h>
#include "app_config.h"

namespace wifi_uart {

template <typename... Args>
void debugPrintf(const char *format, Args... args) {
  if constexpr (kEnableDebugLogs) {
    log_i(format, args...);
  }
}

inline void debugPrint(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

inline void debugPrintln(const char *message) {
  if constexpr (kEnableDebugLogs) {
    log_i("%s", message);
  }
}

}