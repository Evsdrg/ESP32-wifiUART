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
#include <cerrno>
#include <cinttypes>
#include <sys/socket.h>

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
constexpr uint8_t kResumeAfterFlush = 0x63;

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
WiFiServer flushControlServer(RFC2217_FLUSH_CONTROL_PORT);
WiFiClient rfcClient;
WiFiClient flushControlClient;

uint8_t tcpToUartStorage[kRfc2217PendingTcpToUartBytes] = {};
uint8_t uartToTcpStorage[kRfc2217PendingUartToTcpBytes] = {};
uint8_t controlToTcpStorage[kRfc2217ControlToTcpBytes] = {};
ByteRingBuffer tcpToUartBuffer;
ByteRingBuffer uartToTcpBuffer;
ByteRingBuffer controlToTcpBuffer;
#endif

bool rfcServerStarted = false;
bool rfcClientActive = false;
bool flushControlClientActive = false;
bool dropTcpDataUntilResume = false;
uint32_t flushToken = 0;
uint8_t flushRequest[9] = {};
size_t flushRequestLength = 0;
uint8_t flushAck[10] = {};
size_t flushAckLength = 0;
size_t flushAckOffset = 0;
bool dtrActive = false;
bool rtsActive = false;
bool breakActive = false;
bool suboptionReady = false;
bool protocolTxOverflow = false;
uint8_t lineStateMask = 0;
uint8_t modemStateMask = 0xFF;
TelnetState telnetState = TelnetState::Normal;
uint8_t negotiationCommand = 0;
uint8_t suboption[16] = {};
size_t suboptionLength = 0;
uint8_t localOptionEnabled = 0;
uint8_t localOptionRequested = 0;
uint8_t remoteOptionEnabled = 0;
uint8_t remoteOptionRequested = 0;
uint8_t encodedUartChunk[kIoChunkSize * 2] = {};
size_t encodedUartLength = 0;
size_t encodedUartOffset = 0;
size_t encodedUartRawBytes = 0;

uint8_t telnetOptionBit(uint8_t option) {
  switch (option) {
    case kTelnetOptionBinary:
      return 1U << 0;
    case kTelnetOptionEcho:
      return 1U << 1;
    case kTelnetOptionSuppressGoAhead:
      return 1U << 2;
    case kTelnetOptionComPort:
      return 1U << 3;
    default:
      return 0;
  }
}

bool isSupportedLocalOption(uint8_t option) {
  return option == kTelnetOptionEcho || option == kTelnetOptionSuppressGoAhead ||
         option == kTelnetOptionBinary || option == kTelnetOptionComPort;
}

bool isSupportedRemoteOption(uint8_t option) {
  return option == kTelnetOptionSuppressGoAhead || option == kTelnetOptionBinary ||
         option == kTelnetOptionComPort;
}

void resetProtocolState() {
  telnetState = TelnetState::Normal;
  negotiationCommand = 0;
  suboptionLength = 0;
  suboptionReady = false;
  protocolTxOverflow = false;
  localOptionEnabled = 0;
  localOptionRequested = 0;
  remoteOptionEnabled = 0;
  remoteOptionRequested = 0;
  lineStateMask = 0;
  modemStateMask = 0xFF;
  breakActive = false;
  dropTcpDataUntilResume = false;
  flushToken = 0;
  flushRequestLength = 0;
  flushAckLength = 0;
  flushAckOffset = 0;
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
  encodedUartLength = 0;
  encodedUartOffset = 0;
  encodedUartRawBytes = 0;
}

void clearSessionBuffers() {
  clearTcpToUartBuffer();
  clearUartToTcpBuffer();
#if ENABLE_RFC2217_BRIDGE
  controlToTcpBuffer.clear();
#endif
}

bool enqueueControlBytes(const uint8_t *data, size_t size) {
#if ENABLE_RFC2217_BRIDGE
  if (controlToTcpBuffer.freeSpace() < size ||
      controlToTcpBuffer.push(data, size) != size) {
    protocolTxOverflow = true;
    return false;
  }
  return true;
#else
  (void)data;
  (void)size;
  return false;
#endif
}

void sendTelnetOption(uint8_t action, uint8_t option) {
  const uint8_t command[] = {kTelnetIac, action, option};
  enqueueControlBytes(command, sizeof(command));
}

void sendSuboption(uint8_t serverOption, const uint8_t *value, size_t valueLength) {
  uint8_t frame[48] = {};
  size_t frameLength = 0;
  frame[frameLength++] = kTelnetIac;
  frame[frameLength++] = kTelnetSb;
  frame[frameLength++] = kTelnetOptionComPort;
  frame[frameLength++] = serverOption;
  for (size_t i = 0; i < valueLength; ++i) {
    if (frameLength + 2 > sizeof(frame)) {
      protocolTxOverflow = true;
      return;
    }
    frame[frameLength++] = value[i];
    if (value[i] == kTelnetIac) {
      frame[frameLength++] = value[i];
    }
  }
  frame[frameLength++] = kTelnetIac;
  frame[frameLength++] = kTelnetSe;
  enqueueControlBytes(frame, frameLength);
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

void requestLocalOption(uint8_t option) {
  const uint8_t bit = telnetOptionBit(option);
  if (bit == 0 || (localOptionEnabled & bit) != 0 ||
      (localOptionRequested & bit) != 0) {
    return;
  }
  localOptionRequested |= bit;
  sendTelnetOption(kTelnetWill, option);
}

void requestRemoteOption(uint8_t option) {
  const uint8_t bit = telnetOptionBit(option);
  if (bit == 0 || (remoteOptionEnabled & bit) != 0 ||
      (remoteOptionRequested & bit) != 0) {
    return;
  }
  remoteOptionRequested |= bit;
  sendTelnetOption(kTelnetDo, option);
}

void sendInitialNegotiation() {
  requestLocalOption(kTelnetOptionEcho);
  requestLocalOption(kTelnetOptionSuppressGoAhead);
  requestRemoteOption(kTelnetOptionSuppressGoAhead);
  requestLocalOption(kTelnetOptionBinary);
  requestRemoteOption(kTelnetOptionBinary);
  requestLocalOption(kTelnetOptionComPort);
  requestRemoteOption(kTelnetOptionComPort);
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

bool captureUartInput(HardwareSerial &uartPort) {
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
  }
#else
  (void)uartPort;
#endif
  return uartPort.available() == 0;
}

void queueFlushAck(uint8_t phase, uint8_t status, uint32_t token) {
#if ENABLE_RFC2217_BRIDGE
  const uint8_t ack[] = {
      'W', 'U', 'A', '1',
      phase,
      status,
      static_cast<uint8_t>(token >> 24),
      static_cast<uint8_t>(token >> 16),
      static_cast<uint8_t>(token >> 8),
      static_cast<uint8_t>(token),
  };
  memcpy(flushAck, ack, sizeof(ack));
  flushAckLength = sizeof(ack);
  flushAckOffset = 0;
#else
  (void)phase;
  (void)status;
  (void)token;
#endif
}

void flushControlAckIfNeeded() {
#if ENABLE_RFC2217_BRIDGE
  if (!flushControlClientActive || flushAckOffset >= flushAckLength) {
    return;
  }
  const int written = ::send(
      flushControlClient.fd(),
      flushAck + flushAckOffset,
      flushAckLength - flushAckOffset,
      MSG_DONTWAIT);
  if (written > 0) {
    flushAckOffset += static_cast<size_t>(written);
    if (flushAckOffset == flushAckLength) {
      flushAckLength = 0;
      flushAckOffset = 0;
    }
  } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    flushControlClient.stop();
    flushControlClientActive = false;
    if (dropTcpDataUntilResume) {
      rfcClient.stop();
    }
  }
#endif
}

void handleFlushControl(HardwareSerial &uartPort) {
#if ENABLE_RFC2217_BRIDGE
  if (flushControlClientActive && !flushControlClient.connected()) {
    flushControlClient.stop();
    flushControlClientActive = false;
    if (dropTcpDataUntilResume) {
      rfcClient.stop();
      return;
    }
  }
  if (!flushControlClientActive) {
    WiFiClient candidate = flushControlServer.accept();
    if (candidate) {
      if (!rfcClientActive || candidate.remoteIP() != rfcClient.remoteIP()) {
        candidate.stop();
      } else {
        candidate.setNoDelay(true);
        flushControlClient = candidate;
        flushControlClientActive = true;
        flushRequestLength = 0;
      }
    }
  }
  flushControlAckIfNeeded();
  if (!flushControlClientActive || flushAckLength != 0) {
    return;
  }
  while (flushControlClient.available() > 0 && flushRequestLength < sizeof(flushRequest)) {
    const int value = flushControlClient.read();
    if (value < 0) {
      break;
    }
    flushRequest[flushRequestLength++] = static_cast<uint8_t>(value);
  }
  if (flushRequestLength != sizeof(flushRequest)) {
    return;
  }
  const bool validMagic = flushRequest[0] == 'W' && flushRequest[1] == 'U' &&
      flushRequest[2] == 'F' && flushRequest[3] == '1';
  const uint8_t command = flushRequest[4];
  const uint32_t token = (static_cast<uint32_t>(flushRequest[5]) << 24) |
      (static_cast<uint32_t>(flushRequest[6]) << 16) |
      (static_cast<uint32_t>(flushRequest[7]) << 8) |
      static_cast<uint32_t>(flushRequest[8]);
  flushRequestLength = 0;
  if (!validMagic || (command != 1 && command != 2) || dropTcpDataUntilResume) {
    queueFlushAck(1, 1, token);
    return;
  }

  clearTcpToUartBuffer();
  const bool preserveInput = command == 1;
  if (!preserveInput) {
    clearUartToTcpBuffer();
    drainUartInput(uartPort);
  }
  if (!uart_port::discardOutput(preserveInput ? captureUartInput : nullptr)) {
    queueFlushAck(1, 1, token);
    return;
  }
  dropTcpDataUntilResume = true;
  flushToken = token;
  queueFlushAck(1, 0, token);
#else
  (void)uartPort;
#endif
}

bool applyUartSettings(const UartSettings &settings) {
  const UartSettings &current = uart_port::settings();
  if (settings.baudRate == current.baudRate &&
      settings.dataBits == current.dataBits &&
      settings.parity == current.parity &&
      settings.stopBits == current.stopBits) {
    if (!uart_port::waitForTxDrain() || !captureUartInput(uart_port::serial())) {
      return false;
    }
    return true;
  }
  return uart_port::applySettings(settings, captureUartInput);
}

void handleTelnetNegotiation(uint8_t command, uint8_t option) {
  const uint8_t bit = telnetOptionBit(option);
  switch (command) {
    case kTelnetDo: {
      if (!isSupportedLocalOption(option) || bit == 0) {
        sendTelnetOption(kTelnetWont, option);
        break;
      }
      const bool wasRequested = (localOptionRequested & bit) != 0;
      localOptionRequested &= static_cast<uint8_t>(~bit);
      if ((localOptionEnabled & bit) == 0) {
        localOptionEnabled |= bit;
        if (!wasRequested) {
          sendTelnetOption(kTelnetWill, option);
        }
      }
      break;
    }
    case kTelnetDont:
      if (bit != 0 && ((localOptionEnabled | localOptionRequested) & bit) != 0) {
        localOptionEnabled &= static_cast<uint8_t>(~bit);
        localOptionRequested &= static_cast<uint8_t>(~bit);
        sendTelnetOption(kTelnetWont, option);
      }
      break;
    case kTelnetWill: {
      if (!isSupportedRemoteOption(option) || bit == 0) {
        sendTelnetOption(kTelnetDont, option);
        break;
      }
      const bool wasRequested = (remoteOptionRequested & bit) != 0;
      remoteOptionRequested &= static_cast<uint8_t>(~bit);
      if ((remoteOptionEnabled & bit) == 0) {
        remoteOptionEnabled |= bit;
        if (!wasRequested) {
          sendTelnetOption(kTelnetDo, option);
        }
      }
      break;
    }
    case kTelnetWont:
      if (bit != 0 && ((remoteOptionEnabled | remoteOptionRequested) & bit) != 0) {
        remoteOptionEnabled &= static_cast<uint8_t>(~bit);
        remoteOptionRequested &= static_cast<uint8_t>(~bit);
        sendTelnetOption(kTelnetDont, option);
      }
      break;
    default:
      break;
  }
}

void handleSetControl(uint8_t value, HardwareSerial &uartPort) {
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
      breakActive = false;
      sendSuboptionByte(kServerSetControl, kControlBreakOff);
      break;
    case kControlBreakOff:
      breakActive = false;
      sendSuboptionByte(kServerSetControl, kControlBreakOff);
      break;
    case kControlRequestDtr:
      sendSuboptionByte(kServerSetControl, dtrActive ? kControlDtrOn : kControlDtrOff);
      break;
    case kControlDtrOn:
      if (uart_port::waitForTxDrain()) {
        if (captureUartInput(uartPort)) {
          setDtr(true);
        }
      }
      sendSuboptionByte(kServerSetControl, dtrActive ? kControlDtrOn : kControlDtrOff);
      break;
    case kControlDtrOff:
      if (uart_port::waitForTxDrain()) {
        if (captureUartInput(uartPort)) {
          setDtr(false);
        }
      }
      sendSuboptionByte(kServerSetControl, dtrActive ? kControlDtrOn : kControlDtrOff);
      break;
    case kControlRequestRts:
      sendSuboptionByte(kServerSetControl, rtsActive ? kControlRtsOn : kControlRtsOff);
      break;
    case kControlRtsOn:
      if (uart_port::waitForTxDrain()) {
        if (captureUartInput(uartPort)) {
          setRts(true);
        }
      }
      sendSuboptionByte(kServerSetControl, rtsActive ? kControlRtsOn : kControlRtsOff);
      break;
    case kControlRtsOff:
      if (uart_port::waitForTxDrain()) {
        if (captureUartInput(uartPort)) {
          setRts(false);
        }
      }
      sendSuboptionByte(kServerSetControl, rtsActive ? kControlRtsOn : kControlRtsOff);
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
          if (!applyUartSettings(next)) {
            sendSuboptionU32(kServerSetBaudRate, 0);
            break;
          }
        }
      }
      sendSuboptionU32(kServerSetBaudRate, uart_port::settings().baudRate);
      break;
    case kSetDataSize:
      if (suboptionLength >= 3 && suboption[2] >= 5 && suboption[2] <= 8) {
        UartSettings next = uart_port::settings();
        next.dataBits = suboption[2];
        if (!applyUartSettings(next)) {
          sendSuboptionByte(kServerSetDataSize, 0);
          break;
        }
      }
      sendSuboptionByte(kServerSetDataSize, uart_port::settings().dataBits);
      break;
    case kSetParity:
      if (suboptionLength >= 3 && suboption[2] != 0) {
        UartSettings next = uart_port::settings();
        if (suboption[2] == 1) {
          next.parity = 'N';
          if (!applyUartSettings(next)) {
            sendSuboptionByte(kServerSetParity, 0);
            break;
          }
        } else if (suboption[2] == 2) {
          next.parity = 'O';
          if (!applyUartSettings(next)) {
            sendSuboptionByte(kServerSetParity, 0);
            break;
          }
        } else if (suboption[2] == 3) {
          next.parity = 'E';
          if (!applyUartSettings(next)) {
            sendSuboptionByte(kServerSetParity, 0);
            break;
          }
        }
      }
      sendSuboptionByte(kServerSetParity, currentParityCode());
      break;
    case kSetStopSize:
      if (suboptionLength >= 3 && (suboption[2] == 1 || suboption[2] == 2)) {
        UartSettings next = uart_port::settings();
        next.stopBits = suboption[2] == 2 ? 2 : 1;
        if (!applyUartSettings(next)) {
          sendSuboptionByte(kServerSetStopSize, 0);
          break;
        }
      }
      sendSuboptionByte(kServerSetStopSize, currentStopSizeCode());
      break;
    case kSetControl:
      if (suboptionLength >= 3) {
        handleSetControl(suboption[2], uartPort);
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
          const bool preserveInput = suboption[2] == kPurgeTransmitBuffer;
          if (!uart_port::discardOutput(preserveInput ? captureUartInput : nullptr)) {
            sendSuboptionByte(kServerPurgeData, 0);
            break;
          }
        }
#endif
        sendSuboptionByte(kServerPurgeData, suboption[2]);
      }
      break;
    case kResumeAfterFlush:
      if (suboptionLength >= 6) {
        const uint32_t token = (static_cast<uint32_t>(suboption[2]) << 24) |
            (static_cast<uint32_t>(suboption[3]) << 16) |
            (static_cast<uint32_t>(suboption[4]) << 8) |
            static_cast<uint32_t>(suboption[5]);
        if (dropTcpDataUntilResume && token == flushToken) {
          dropTcpDataUntilResume = false;
          flushToken = 0;
          queueFlushAck(2, 0, token);
        }
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

bool processIncomingByte(uint8_t value) {
  switch (telnetState) {
    case TelnetState::Normal:
      if (value == kTelnetIac) {
        telnetState = TelnetState::IacSeen;
      } else if (!dropTcpDataUntilResume && !enqueueTcpDataByte(value)) {
        return false;
      }
      break;
    case TelnetState::IacSeen:
      if (value == kTelnetIac) {
        if (!dropTcpDataUntilResume && !enqueueTcpDataByte(kTelnetIac)) {
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
        telnetState = TelnetState::Normal;
        suboptionReady = true;
        return false;
      } else {
        telnetState = TelnetState::Normal;
      }
      break;
  }
  return true;
}

void pullTcpIntoBuffer() {
#if ENABLE_RFC2217_BRIDGE
  if (suboptionReady) {
    return;
  }
  while (rfcClient.available() > 0) {
    if ((telnetState == TelnetState::Normal || telnetState == TelnetState::IacSeen) &&
        tcpToUartBuffer.freeSpace() == 0) {
      break;
    }

    const int byteValue = rfcClient.read();
    if (byteValue < 0) {
      break;
    }
    if (!processIncomingByte(static_cast<uint8_t>(byteValue))) {
      break;
    }
    bridge::markActivity();
  }
#else
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
  const size_t previousSize = uartToTcpBuffer.size();
  captureUartInput(uartPort);
  if (uartToTcpBuffer.size() > previousSize) {
    bridge::markActivity();
  }
#else
  (void)uartPort;
#endif
}

ssize_t sendClientNonBlocking(const uint8_t *data, size_t size) {
#if ENABLE_RFC2217_BRIDGE
  const int socketFd = rfcClient.fd();
  if (socketFd < 0) {
    errno = EBADF;
    return -1;
  }
  return send(socketFd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
#else
  (void)data;
  (void)size;
  errno = ENOTCONN;
  return -1;
#endif
}

bool socketWriteWouldBlock() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

void prepareEncodedUartChunk() {
#if ENABLE_RFC2217_BRIDGE
  if (encodedUartLength != 0 || uartToTcpBuffer.size() == 0) {
    return;
  }

  uint8_t raw[kIoChunkSize];
  encodedUartRawBytes = min(uartToTcpBuffer.size(), sizeof(raw));
  uartToTcpBuffer.peek(raw, encodedUartRawBytes);
  encodedUartLength = 0;
  encodedUartOffset = 0;
  for (size_t i = 0; i < encodedUartRawBytes; ++i) {
    encodedUartChunk[encodedUartLength++] = raw[i];
    if (raw[i] == kTelnetIac) {
      encodedUartChunk[encodedUartLength++] = raw[i];
    }
  }
#endif
}

void flushOutboundToTcp() {
#if ENABLE_RFC2217_BRIDGE
  size_t sendBudget = kTcpSendBudgetPerLoop;
  uint8_t controlChunk[kIoChunkSize];

  while (isClientConnected() && sendBudget > 0) {
    // Never interleave a control frame into a partially sent escaped UART chunk.
    if (encodedUartOffset < encodedUartLength) {
      const size_t requestSize = min(
          encodedUartLength - encodedUartOffset,
          sendBudget);
      const ssize_t written = sendClientNonBlocking(
          encodedUartChunk + encodedUartOffset,
          requestSize);
      if (written < 0) {
        if (socketWriteWouldBlock()) {
          return;
        }
        disconnectClient("socket send failed");
        return;
      }
      if (written == 0) {
        disconnectClient("socket closed during write");
        return;
      }

      encodedUartOffset += static_cast<size_t>(written);
      sendBudget -= static_cast<size_t>(written);
      if (encodedUartOffset == encodedUartLength) {
        uartToTcpBuffer.discard(encodedUartRawBytes);
        encodedUartLength = 0;
        encodedUartOffset = 0;
        encodedUartRawBytes = 0;
        bridge::markActivity();
      }
      continue;
    }

    if (controlToTcpBuffer.size() > 0) {
      const size_t chunkSize = min(
          controlToTcpBuffer.size(),
          min(sizeof(controlChunk), sendBudget));
      controlToTcpBuffer.peek(controlChunk, chunkSize);
      const ssize_t written = sendClientNonBlocking(controlChunk, chunkSize);
      if (written < 0) {
        if (socketWriteWouldBlock()) {
          return;
        }
        disconnectClient("socket send failed");
        return;
      }
      if (written == 0) {
        disconnectClient("socket closed during write");
        return;
      }
      controlToTcpBuffer.discard(static_cast<size_t>(written));
      sendBudget -= static_cast<size_t>(written);
      continue;
    }

    if (uartToTcpBuffer.size() == 0) {
      return;
    }

    prepareEncodedUartChunk();
  }
#endif
}

}  // namespace

bool beginBuffers() {
#if ENABLE_RFC2217_BRIDGE
  const bool tcpReady = tcpToUartBuffer.begin(tcpToUartStorage, sizeof(tcpToUartStorage));
  const bool uartReady = uartToTcpBuffer.begin(uartToTcpStorage, sizeof(uartToTcpStorage));
  const bool controlReady = controlToTcpBuffer.begin(controlToTcpStorage, sizeof(controlToTcpStorage));
  if (!tcpReady || !uartReady || !controlReady) {
    tcpToUartBuffer.release();
    uartToTcpBuffer.release();
    controlToTcpBuffer.release();
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
  flushControlServer.begin();
  flushControlServer.setNoDelay(true);
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
  flushControlServer.end();
#endif
  rfcServerStarted = false;
}

void disconnectClient(const char *reason) {
#if ENABLE_RFC2217_BRIDGE
  const bool hadClient = rfcClientActive;
  rfcClient.stop();
  flushControlClient.stop();
  rfcClientActive = false;
  flushControlClientActive = false;
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

void acceptClientIfNeeded(HardwareSerial &uartPort, bool allowNewClient) {
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
  if (!allowNewClient) {
    newClient.stop();
    return;
  }

  if (bridge::isTcpClientConnected()) {
    newClient.stop();
    debugPrintln("RFC2217 client rejected while raw TCP session owns UART");
    return;
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

  handleFlushControl(uartPort);
  if (!rfcClient.connected()) {
    disconnectClient("flush control lost");
    return;
  }

  pullTcpIntoBuffer();
  flushTcpBufferToUart(uartPort);
  if (suboptionReady && tcpToUartBuffer.size() == 0) {
    handleSuboption(uartPort);
    suboptionReady = false;
    suboptionLength = 0;
  }
  pullUartIntoBuffer(uartPort);
  flushOutboundToTcp();

  if (protocolTxOverflow) {
    disconnectClient("protocol response queue overflow");
    return;
  }

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
