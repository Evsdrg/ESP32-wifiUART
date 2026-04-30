#include "uart_port.h"
#include "app_config.h"
#include "debug_log.h"
#include "tcp_uart_bridge.h"

#include <cinttypes>

namespace wifi_uart {
namespace uart_port {

namespace {

HardwareSerial uartPort(1);
UartSettings currentSettings = {kUartBaudRate, 8, 'N', 1};

}  // namespace

HardwareSerial &serial() {
  return uartPort;
}

const UartSettings &settings() {
  return currentSettings;
}

bool mapUartConfig(const UartSettings &settings, uint32_t &serialConfig) {
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

bool applySettings(const UartSettings &settings) {
  uint32_t serialConfig = SERIAL_8N1;
  if (!mapUartConfig(settings, serialConfig)) {
    return false;
  }

  bridge::clearSessionBuffers();
  uartPort.flush();
  uartPort.end();
  uartPort.setRxBufferSize(kUartDriverRxBufferSize);
  uartPort.setTxBufferSize(kUartDriverTxBufferSize);
  uartPort.begin(
      settings.baudRate,
      serialConfig,
      UART1_RX_PIN,
      UART1_TX_PIN,
      false,
      20000UL,
      kUartRxFifoFullThreshold);
  uartPort.setTimeout(0);
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
