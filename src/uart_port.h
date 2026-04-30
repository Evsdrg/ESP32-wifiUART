#pragma once

#include "models.h"

namespace wifi_uart {
namespace uart_port {

HardwareSerial &serial();
const UartSettings &settings();
bool mapUartConfig(const UartSettings &settings, uint32_t &serialConfig);
bool applySettings(const UartSettings &settings);

}
}
