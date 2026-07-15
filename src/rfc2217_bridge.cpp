/**
 * @file   rfc2217_bridge.cpp
 * @brief  RFC2217 remote serial bridge with DTR/RTS control support
 *
 * This module keeps the raw TCP bridge on TCP_BRIDGE_PORT unchanged and exposes
 * a separate RFC2217 endpoint for tools such as esptool.py that need modem
 * control lines. It implements the pyserial/esptool compatible subset: Telnet
 * option negotiation, UART parameter acknowledgements, COM-PORT-OPTION control
 * commands, purge commands, and IAC escaping for binary serial data.
 */

#include "rfc2217_bridge.h"

#include "app_config.h"
#include "byte_ring_buffer.h"
#include "debug_log.h"
#include "tcp_uart_bridge.h"
#include "uart_port.h"

#include <WiFi.h>
#include <cinttypes>

namespace wifi_uart {
namespace rfc2217_bridge {

namespace {

constexpr uint8_t kTelnetSe = 0xF0;
constexpr uint8_t kTelnetSb = 0xFA;
constexpr uint8_t kTelnetWill = 0xFB;
constexpr uint8_t kTelnetWont = 0xFC;
constexpr uint8_t kTelnetDo = 0xFD;
constexpr uint8_t kTelnetDont = 0xFE;
constexpr uint8_t kTelnetIac = 0xFF;

constexpr uint8_t kTelnetOptionBinary = 0x00;
constexpr uint8_t kTelnetOptionEcho = 0x01;
constexpr uint8_t kTelnetOptionSuppressGoAhead = 0x03;
constexpr uint8_t kTelnetOptionComPort = 0x2C;

constexpr uint8_t kSetBaudRate = 0x01;
constexpr uint8_t kSetDataSize = 0x02;
constexpr uint8_t kSetParity = 0x03;
constexpr uint8_t kSetStopSize = 0x04;
constexpr uint8_t kSetControl = 0x05;
constexpr uint8_t kNotifyLineState = 0x06;
constexpr uint8_t kNotifyModemState = 0x07;
constexpr uint8_t kSetLineStateMask = 0x0A;
constexpr uint8_t kSetModemStateMask = 0x0B;
constexpr uint8_t kPurgeData = 0x0C;

constexpr uint8_t kServerSetBaudRate = 0x65;
constexpr uint8_t kServerSetDataSize = 0x66;
constexpr uint8_t kServerSetParity = 0x67;
constexpr uint8_t kServerSetStopSize = 0x68;
constexpr uint8_t kServerSetControl = 0x69;
constexpr uint8_t kServerNotifyLineState = 0x6A;
constexpr uint8_t kServerNotifyModemState = 0x6B;
constexpr uint8_t kServerSetLineStateMask = 0x6E;
constexpr uint8_t kServerSetModemStateMask = 0x6F;
constexpr uint8_t kServerPurgeData = 0x70;

constexpr uint8_t kControlRequestFlowSetting = 0x00;
constexpr uint8_t kControlUseNoFlow = 0x01;
constexpr uint8_t kControlUseSoftwareFlow = 0x02;
constexpr uint8_t kControlUseHardwareFlow = 0x03;
constexpr uint8_t kControlRequestBreak = 0x04;
constexpr uint8_t kControlBreakOn = 0x05;
constexpr uint8_t kControlBreakOff = 0x06;
constexpr uint8_t kControlRequestDtr = 0x07;
constexpr uint8_t kControlDtrOn = 0x08;
constexpr uint8_t kControlDtrOff = 0x09;
constexpr uint8_t kControlRequestRts = 0x0A;
constexpr uint8_t kControlRtsOn = 0x0B;
constexpr uint8_t kControlRtsOff = 0x0C;

constexpr uint8_t kPurgeReceiveBuffer = 0x01;
constexpr uint8_t kPurgeTransmitBuffer = 0x02;
constexpr uint8_t kPurgeBothBuffers = 0x03;

enum class TelnetState : uint8_t {
  Normal,
  IacSeen,
  Negotiate,
  Subnegotiation,
  SubnegotiationIacSeen,
};

#if ENABLE_RFC2217_BRIDGE
WiFiServer rfcServer(RFC2217_BRIDGE_PORT);
WiFiClient rfcClient;

uint8_t tcpToUartStorage[kRfc2217PendingTcpToUartBytes] = {};
uint8_t uartToTcpStorage[kRfc2217PendingUartToTcpBytes] = {};
ByteRingBuffer tcpToUartBuffer;
ByteRingBuffer uartToTcpBuffer;
#endif

bool rfcServerStarted = false;
bool rfcClientActive = false;
bool pendingEscapedIacByte = false;
bool dtrActive = false;
bool rtsActive = false;
bool breakActive = false;
uint8_t lineStateMask = 0;
uint8_t modemStateMask = 0xFF;
TelnetState telnetState = TelnetState::Normal;
uint8_t negotiationCommand = 0;
uint8_t suboption[16] = {};
size_t suboptionLength = 0;

bool isSupportedLocalOption(uint8_t option) {
  return option == kTelnetOptionEcho || option == kTelnetOptionSuppressGoAhead ||
         option == kTelnetOptionBinary || option == kTelnetOptionComPort;
}

bool isSupportedRemoteOption(uint8_t option) {
  return option == kTelnetOptionSuppressGoAhead || option == kTelnetOptionBinary ||
         option == kTelnetOptionComPort;
}

void resetProtocolState() {
  pendingEscapedIacByte = false;
  telnetState = TelnetState::Normal;
  negotiationCommand = 0;
  suboptionLength = 0;
}

void clearTcpToUartBuffer() {
#if ENABLE_RFC2217_BRIDGE
  tcpToUartBuffer.clear();
#endif
}

void clearUartToTcpBuffer() {
#if ENABLE_RFC2217_BRIDGE
  uartToTcpBuffer.clear();
#endif
  pendingEscapedIacByte = false;
}

void clearSessionBuffers() {
  clearTcpToUartBuffer();
  clearUartToTcpBuffer();
}

void sendTelnetOption(uint8_t action, uint8_t option) {
#if ENABLE_RFC2217_BRIDGE
  const uint8_t command[] = {kTelnetIac, action, option};
  rfcClient.write(command, sizeof(command));
#else
  (void)action;
  (void)option;
#endif
}

void sendSuboption(uint8_t serverOption, const uint8_t *value, size_t valueLength) {
#if ENABLE_RFC2217_BRIDGE
  const uint8_t prefix[] = {kTelnetIac, kTelnetSb, kTelnetOptionComPort, serverOption};
  const uint8_t suffix[] = {kTelnetIac, kTelnetSe};
  rfcClient.write(prefix, sizeof(prefix));
  for (size_t i = 0; i < valueLength; ++i) {
    rfcClient.write(value[i]);
    if (value[i] == kTelnetIac) {
      rfcClient.write(value[i]);
    }
  }
  rfcClient.write(suffix, sizeof(suffix));
#else
  (void)serverOption;
  (void)value;
  (void)valueLength;
#endif
}

void sendSuboptionByte(uint8_t serverOption, uint8_t value) {
  sendSuboption(serverOption, &value, 1);
}

void sendSuboptionU32(uint8_t serverOption, uint32_t value) {
  const uint8_t bytes[] = {
      static_cast<uint8_t>((value >> 24) & 0xFF),
      static_cast<uint8_t>((value >> 16) & 0xFF),
      static_cast<uint8_t>((value >> 8) & 0xFF),
      static_cast<uint8_t>(value & 0xFF),
  };
  sendSuboption(serverOption, bytes, sizeof(bytes));
}

void sendInitialNegotiation() {
  sendTelnetOption(kTelnetWill, kTelnetOptionEcho);
  sendTelnetOption(kTelnetWill, kTelnetOptionSuppressGoAhead);
  sendTelnetOption(kTelnetDo, kTelnetOptionSuppressGoAhead);
  sendTelnetOption(kTelnetWill, kTelnetOptionBinary);
  sendTelnetOption(kTelnetDo, kTelnetOptionBinary);
  sendTelnetOption(kTelnetWill, kTelnetOptionComPort);
  sendTelnetOption(kTelnetDo, kTelnetOptionComPort);
  sendSuboptionByte(kServerNotifyLineState, 0);
  sendSuboptionByte(kServerNotifyModemState, 0);
}

int controlActiveLevel() {
  return UART_BRIDGE_CONTROL_ACTIVE_LOW ? LOW : HIGH;
}

int controlInactiveLevel() {
  return UART_BRIDGE_CONTROL_ACTIVE_LOW ? HIGH : LOW;
}

void writeControlPin(int pin, bool active) {
  if (pin < 0) {
    return;
  }
  digitalWrite(pin, active ? controlActiveLevel() : controlInactiveLevel());
}

void initializeControlPin(int pin) {
  if (pin < 0) {
    return;
  }
  digitalWrite(pin, controlInactiveLevel());
  pinMode(pin, OUTPUT);
}

void setDtr(bool active) {
  dtrActive = active;
  writeControlPin(UART_BRIDGE_DTR_PIN, active);
}

void setRts(bool active) {
  rtsActive = active;
  writeControlPin(UART_BRIDGE_RTS_PIN, active);
}

uint8_t currentParityCode() {
  switch (uart_port::settings().parity) {
    case 'O':
      return 2;
    case 'E':
      return 3;
    case 'N':
    default:
      return 1;
  }
}

uint8_t currentStopSizeCode() {
  return uart_port::settings().stopBits == 2 ? 2 : 1;
}

void applyUartSettings(const UartSettings &settings) {
  if (uart_port::applySettings(settings)) {
    clearSessionBuffers();
  }
}

void drainUartInput(HardwareSerial &uartPort) {
  uint8_t buffer[kIoChunkSize];
  size_t bytesToDiscard = static_cast<size_t>(uartPort.available());
  while (bytesToDiscard > 0) {
    const size_t requestSize = min(bytesToDiscard, sizeof(buffer));
    const size_t readSize = uartPort.read(buffer, requestSize);
    if (readSize == 0) {
      break;
    }
    bytesToDiscard -= readSize;
  }
}

void handleTelnetNegotiation(uint8_t command, uint8_t option) {
  switch (command) {
    case kTelnetDo:
      sendTelnetOption(isSupportedLocalOption(option) ? kTelnetWill : kTelnetWont, option);
      break;
    case kTelnetDont:
      sendTelnetOption(kTelnetWont, option);
      break;
    case kTelnetWill:
      sendTelnetOption(isSupportedRemoteOption(option) ? kTelnetDo : kTelnetDont, option);
      break;
    case kTelnetWont:
      sendTelnetOption(kTelnetDont, option);
      break;
    default:
      break;
  }
}

void handleSetControl(uint8_t value) {
  switch (value) {
    case kControlRequestFlowSetting:
    case kControlUseNoFlow:
    case kControlUseSoftwareFlow:
    case kControlUseHardwareFlow:
      sendSuboptionByte(kServerSetControl, kControlUseNoFlow);
      break;
    case kControlRequestBreak:
      sendSuboptionByte(kServerSetControl, breakActive ? kControlBreakOn : kControlBreakOff);
      break;
    case kControlBreakOn:
      breakActive = true;
      sendSuboptionByte(kServerSetControl, kControlBreakOn);
      break;
    case kControlBreakOff:
      breakActive = false;
      sendSuboptionByte(kServerSetControl, kControlBreakOff);
      break;
    case kControlRequestDtr:
      sendSuboptionByte(kServerSetControl, dtrActive ? kControlDtrOn : kControlDtrOff);
      break;
    case kControlDtrOn:
      setDtr(true);
      sendSuboptionByte(kServerSetControl, kControlDtrOn);
      break;
    case kControlDtrOff:
      setDtr(false);
      sendSuboptionByte(kServerSetControl, kControlDtrOff);
      break;
    case kControlRequestRts:
      sendSuboptionByte(kServerSetControl, rtsActive ? kControlRtsOn : kControlRtsOff);
      break;
    case kControlRtsOn:
      setRts(true);
      sendSuboptionByte(kServerSetControl, kControlRtsOn);
      break;
    case kControlRtsOff:
      setRts(false);
      sendSuboptionByte(kServerSetControl, kControlRtsOff);
      break;
    default:
      break;
  }
}

void handleSuboption(HardwareSerial &uartPort) {
  if (suboptionLength < 2 || suboption[0] != kTelnetOptionComPort) {
    return;
  }

  const uint8_t option = suboption[1];
  switch (option) {
    case kSetBaudRate:
      if (suboptionLength >= 6) {
        const uint32_t baudRate = (static_cast<uint32_t>(suboption[2]) << 24) |
                                  (static_cast<uint32_t>(suboption[3]) << 16) |
                                  (static_cast<uint32_t>(suboption[4]) << 8) |
                                  static_cast<uint32_t>(suboption[5]);
        if (baudRate > 0) {
          UartSettings next = uart_port::settings();
          next.baudRate = baudRate;
          applyUartSettings(next);
        }
      }
      sendSuboptionU32(kServerSetBaudRate, uart_port::settings().baudRate);
      break;
    case kSetDataSize:
      if (suboptionLength >= 3 && suboption[2] >= 5 && suboption[2] <= 8) {
        UartSettings next = uart_port::settings();
        next.dataBits = suboption[2];
        applyUartSettings(next);
      }
      sendSuboptionByte(kServerSetDataSize, uart_port::settings().dataBits);
      break;
    case kSetParity:
      if (suboptionLength >= 3 && suboption[2] != 0) {
        UartSettings next = uart_port::settings();
        if (suboption[2] == 1) {
          next.parity = 'N';
          applyUartSettings(next);
        } else if (suboption[2] == 2) {
          next.parity = 'O';
          applyUartSettings(next);
        } else if (suboption[2] == 3) {
          next.parity = 'E';
          applyUartSettings(next);
        }
      }
      sendSuboptionByte(kServerSetParity, currentParityCode());
      break;
    case kSetStopSize:
      if (suboptionLength >= 3 && (suboption[2] == 1 || suboption[2] == 2)) {
        UartSettings next = uart_port::settings();
        next.stopBits = suboption[2] == 2 ? 2 : 1;
        applyUartSettings(next);
      }
      sendSuboptionByte(kServerSetStopSize, currentStopSizeCode());
      break;
    case kSetControl:
      if (suboptionLength >= 3) {
        handleSetControl(suboption[2]);
      }
      break;
    case kNotifyLineState:
      sendSuboptionByte(kServerNotifyLineState, 0);
      break;
    case kNotifyModemState:
      sendSuboptionByte(kServerNotifyModemState, 0);
      break;
    case kSetLineStateMask:
      if (suboptionLength >= 3) {
        lineStateMask = suboption[2];
      }
      sendSuboptionByte(kServerSetLineStateMask, lineStateMask);
      break;
    case kSetModemStateMask:
      if (suboptionLength >= 3) {
        modemStateMask = suboption[2];
      }
      sendSuboptionByte(kServerSetModemStateMask, modemStateMask);
      break;
    case kPurgeData:
      if (suboptionLength >= 3) {
#if ENABLE_RFC2217_BRIDGE
        if (suboption[2] == kPurgeReceiveBuffer || suboption[2] == kPurgeBothBuffers) {
          clearUartToTcpBuffer();
          drainUartInput(uartPort);
        }
        if (suboption[2] == kPurgeTransmitBuffer || suboption[2] == kPurgeBothBuffers) {
          clearTcpToUartBuffer();
        }
#endif
        sendSuboptionByte(kServerPurgeData, suboption[2]);
      }
      break;
    default:
      break;
  }
}

bool enqueueTcpDataByte(uint8_t value) {
#if ENABLE_RFC2217_BRIDGE
  return tcpToUartBuffer.push(&value, 1) == 1;
#else
  (void)value;
  return false;
#endif
}

bool processIncomingByte(uint8_t value, HardwareSerial &uartPort) {
  switch (telnetState) {
    case TelnetState::Normal:
      if (value == kTelnetIac) {
        telnetState = TelnetState::IacSeen;
      } else if (!enqueueTcpDataByte(value)) {
        return false;
      }
      break;
    case TelnetState::IacSeen:
      if (value == kTelnetIac) {
        if (!enqueueTcpDataByte(kTelnetIac)) {
          return false;
        }
        telnetState = TelnetState::Normal;
      } else if (value == kTelnetDo || value == kTelnetDont || value == kTelnetWill || value == kTelnetWont) {
        negotiationCommand = value;
        telnetState = TelnetState::Negotiate;
      } else if (value == kTelnetSb) {
        suboptionLength = 0;
        telnetState = TelnetState::Subnegotiation;
      } else {
        telnetState = TelnetState::Normal;
      }
      break;
    case TelnetState::Negotiate:
      handleTelnetNegotiation(negotiationCommand, value);
      telnetState = TelnetState::Normal;
      break;
    case TelnetState::Subnegotiation:
      if (value == kTelnetIac) {
        telnetState = TelnetState::SubnegotiationIacSeen;
      } else if (suboptionLength < sizeof(suboption)) {
        suboption[suboptionLength++] = value;
      }
      break;
    case TelnetState::SubnegotiationIacSeen:
      if (value == kTelnetIac) {
        if (suboptionLength < sizeof(suboption)) {
          suboption[suboptionLength++] = value;
        }
        telnetState = TelnetState::Subnegotiation;
      } else if (value == kTelnetSe) {
        handleSuboption(uartPort);
        telnetState = TelnetState::Normal;
      } else {
        telnetState = TelnetState::Normal;
      }
      break;
  }
  return true;
}

void pullTcpIntoBuffer(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  while (rfcClient.available() > 0) {
    if ((telnetState == TelnetState::Normal || telnetState == TelnetState::IacSeen) &&
        tcpToUartBuffer.freeSpace() == 0) {
      break;
    }

    const int byteValue = rfcClient.read();
    if (byteValue < 0) {
      break;
    }
    if (!processIncomingByte(static_cast<uint8_t>(byteValue), uartPort)) {
      break;
    }
    bridge::markActivity();
  }
#else
  (void)uartPort;
#endif
}

void flushTcpBufferToUart(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  uint8_t buffer[kIoChunkSize];
  while (tcpToUartBuffer.size() > 0) {
    const int availableSpace = uartPort.availableForWrite();
    if (availableSpace <= 0) {
      break;
    }
    const size_t chunkSize = min(
        tcpToUartBuffer.size(),
        min(sizeof(buffer), static_cast<size_t>(availableSpace)));
    tcpToUartBuffer.peek(buffer, chunkSize);
    const size_t written = uartPort.write(buffer, chunkSize);
    if (written == 0) {
      break;
    }
    tcpToUartBuffer.discard(written);
    bridge::markActivity();
  }
#else
  (void)uartPort;
#endif
}

void pullUartIntoBuffer(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  uint8_t buffer[kIoChunkSize];
  while (uartPort.available() > 0 && uartToTcpBuffer.freeSpace() > 0) {
    const size_t requestSize = min(
        static_cast<size_t>(uartPort.available()),
        min(sizeof(buffer), uartToTcpBuffer.freeSpace()));
    const size_t readSize = uartPort.read(buffer, requestSize);
    if (readSize == 0) {
      break;
    }
    uartToTcpBuffer.push(buffer, readSize);
    bridge::markActivity();
  }
#else
  (void)uartPort;
#endif
}

bool writeClientByte(uint8_t value) {
#if ENABLE_RFC2217_BRIDGE
  return rfcClient.write(value) == 1;
#else
  (void)value;
  return false;
#endif
}

void flushUartBufferToTcp() {
#if ENABLE_RFC2217_BRIDGE
  while (rfcClient.connected()) {
    if (pendingEscapedIacByte) {
      if (!writeClientByte(kTelnetIac)) {
        return;
      }
      pendingEscapedIacByte = false;
      uartToTcpBuffer.discard(1);
      bridge::markActivity();
      continue;
    }

    if (uartToTcpBuffer.size() == 0) {
      return;
    }

    uint8_t value = 0;
    uartToTcpBuffer.peek(&value, 1);
    if (!writeClientByte(value)) {
      return;
    }

    if (value == kTelnetIac) {
      pendingEscapedIacByte = true;
      continue;
    }

    uartToTcpBuffer.discard(1);
    bridge::markActivity();
  }
#endif
}

}  // namespace

bool beginBuffers() {
#if ENABLE_RFC2217_BRIDGE
  const bool tcpReady = tcpToUartBuffer.begin(tcpToUartStorage, sizeof(tcpToUartStorage));
  const bool uartReady = uartToTcpBuffer.begin(uartToTcpStorage, sizeof(uartToTcpStorage));
  if (!tcpReady || !uartReady) {
    tcpToUartBuffer.release();
    uartToTcpBuffer.release();
    return false;
  }
  initializeControlPin(UART_BRIDGE_DTR_PIN);
  initializeControlPin(UART_BRIDGE_RTS_PIN);
#endif
  return true;
}

bool startServer() {
#if ENABLE_RFC2217_BRIDGE
  if (rfcServerStarted) {
    return false;
  }
  rfcServer.begin();
  rfcServer.setNoDelay(true);
  rfcServerStarted = true;
  return true;
#else
  return false;
#endif
}

void stopServer() {
  if (!rfcServerStarted) {
    return;
  }
  disconnectClient("server stopping");
#if ENABLE_RFC2217_BRIDGE
  rfcServer.end();
#endif
  rfcServerStarted = false;
}

void disconnectClient(const char *reason) {
#if ENABLE_RFC2217_BRIDGE
  const bool hadClient = rfcClientActive;
  rfcClient.stop();
  rfcClientActive = false;
  clearSessionBuffers();
  resetProtocolState();
  setDtr(false);
  setRts(false);
  if (hadClient) {
    debugPrintf("RFC2217 client disconnected: %s\n", reason);
  }
#else
  (void)reason;
#endif
}

void acceptClientIfNeeded(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  if (!rfcServerStarted) {
    return;
  }

  if (rfcClientActive && !rfcClient.connected()) {
    disconnectClient("peer closed");
  }

  if (rfcClientActive && rfcClient.connected()) {
    return;
  }

  WiFiClient newClient = rfcServer.accept();
  if (!newClient) {
    return;
  }

  if (bridge::isTcpClientConnected()) {
    bridge::disconnectTcpClient("rfc2217 session active");
  }

  drainUartInput(uartPort);
  clearSessionBuffers();
  resetProtocolState();
  newClient.setNoDelay(true);
  rfcClient = newClient;
  rfcClientActive = true;
  sendInitialNegotiation();
  debugPrintf("RFC2217 client connected: %s\n", rfcClient.remoteIP().toString().c_str());
#else
  (void)uartPort;
#endif
}

void handleClient(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  if (!isClientConnected()) {
    return;
  }

  if (!rfcClient.connected()) {
    disconnectClient("peer closed");
    return;
  }

  pullTcpIntoBuffer(uartPort);
  flushTcpBufferToUart(uartPort);
  pullUartIntoBuffer(uartPort);
  flushUartBufferToTcp();

  if (!rfcClient.connected()) {
    disconnectClient("peer closed");
  }
#else
  (void)uartPort;
#endif
}

bool isClientConnected() {
#if ENABLE_RFC2217_BRIDGE
  return rfcClientActive && rfcClient.connected();
#else
  return false;
#endif
}

}
}
