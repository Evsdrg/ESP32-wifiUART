#include "tcp_uart_bridge.h"
#include "app_config.h"
#include "byte_ring_buffer.h"
#include "debug_log.h"

#include <WiFi.h>

namespace wifi_uart {
namespace bridge {

namespace {

WiFiServer tcpServer(TCP_BRIDGE_PORT);
WiFiClient tcpClient;
bool tcpServerStarted = false;
#if !USE_PSRAM_BRIDGE_BUFFERS
uint8_t tcpToUartStorage[kPendingTcpToUartBytes] = {};
uint8_t uartToTcpStorage[kPendingUartToTcpBytes] = {};
#endif
ByteRingBuffer tcpToUartBuffer;
ByteRingBuffer uartToTcpBuffer;
uint32_t lastActivityAtMs = 0;
uint32_t lastUartBackpressureLogAtMs = 0;
uint32_t lastStatsLogAtMs = 0;
bool tcpClientActive = false;
uint64_t uartToTcpBytes = 0;
uint64_t tcpToUartBytes = 0;
uint32_t tcpClientConnectCount = 0;
uint32_t tcpClientDisconnectCount = 0;
uint32_t wifiReconnectCountVal = 0;
uint32_t uartBackpressureEvents = 0;
uint32_t uartToTcpOverflowEvents = 0;
uint32_t tcpToUartOverflowEvents = 0;
uint32_t tcpPartialWriteEvents = 0;

}  // namespace

bool beginBuffers() {
#if USE_PSRAM_BRIDGE_BUFFERS
  const bool tcpReady = tcpToUartBuffer.begin(kPendingTcpToUartBytes);
  const bool uartReady = uartToTcpBuffer.begin(kPendingUartToTcpBytes);
#else
  const bool tcpReady = tcpToUartBuffer.begin(tcpToUartStorage, sizeof(tcpToUartStorage));
  const bool uartReady = uartToTcpBuffer.begin(uartToTcpStorage, sizeof(uartToTcpStorage));
#endif

  if (!tcpReady || !uartReady) {
    tcpToUartBuffer.release();
    uartToTcpBuffer.release();
    return false;
  }

  debugPrintf(
      "Bridge buffers ready. TCP->UART=%" PRIu32 " (%s), UART->TCP=%" PRIu32 " (%s)\n",
      static_cast<uint32_t>(tcpToUartBuffer.capacity()),
      tcpToUartBuffer.usingPsram() ? "PSRAM" : "SRAM",
      static_cast<uint32_t>(uartToTcpBuffer.capacity()),
      uartToTcpBuffer.usingPsram() ? "PSRAM" : "SRAM");
  return true;
}

void markWifiReconnect() {
  ++wifiReconnectCountVal;
}

uint32_t lastActivityMs() {
  return lastActivityAtMs;
}

bool startTcpServer() {
  if (tcpServerStarted) {
    return false;
  }

  tcpServer.begin();
  tcpServer.setNoDelay(true);
  tcpServerStarted = true;
  return true;
}

void stopTcpServer() {
  if (!tcpServerStarted) {
    return;
  }

  tcpServer.end();
  tcpServerStarted = false;
}

void clearSessionBuffers() {
  tcpToUartBuffer.clear();
  uartToTcpBuffer.clear();
}

void markActivity() {
  lastActivityAtMs = millis();
}

void disconnectTcpClient(const char *reason) {
  const bool hadSession = tcpClientActive;
  tcpClient.stop();
  tcpClientActive = false;
  if (hadSession) {
    ++tcpClientDisconnectCount;
  }
  debugPrintf("TCP client disconnected: %s\n", reason);
}

void acceptClientIfNeeded() {
  if (!tcpServerStarted) {
    return;
  }

  if (tcpClientActive && !tcpClient.connected()) {
    disconnectTcpClient("peer closed");
  }

  if (tcpClientActive && tcpClient.connected()) {
    return;
  }

  WiFiClient newClient = tcpServer.accept();
  if (!newClient) {
    return;
  }

  newClient.setNoDelay(true);
  tcpClient = newClient;
  tcpClientActive = true;
  ++tcpClientConnectCount;
  debugPrintf("TCP client connected: %s\n", tcpClient.remoteIP().toString().c_str());
}

bool isTcpClientConnected() {
  return tcpClientActive && tcpClient.connected();
}

void pullTcpIntoBuffer() {
  if (!isTcpClientConnected()) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  while (tcpClient.available() > 0 && tcpToUartBuffer.freeSpace() > 0) {
    const size_t requestSize = min(
        static_cast<size_t>(tcpClient.available()),
        min(sizeof(buffer), tcpToUartBuffer.freeSpace()));
    const int readSize = tcpClient.read(buffer, requestSize);
    if (readSize <= 0) {
      break;
    }

    tcpToUartBuffer.push(buffer, static_cast<size_t>(readSize));
    markActivity();
  }

  if (tcpClient.available() > 0 && tcpToUartBuffer.freeSpace() == 0) {
    ++tcpToUartOverflowEvents;
  }
}

void flushTcpBufferToUart(HardwareSerial &uartPort) {
  if (tcpToUartBuffer.size() == 0) {
    return;
  }

  const int uartWriteSpace = uartPort.availableForWrite();
  if (uartWriteSpace <= 0) {
    return;
  }

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
    tcpToUartBytes += written;
    markActivity();
  }
}

void pullUartIntoBuffer(HardwareSerial &uartPort) {
  uint8_t buffer[kIoChunkSize];
  while (uartPort.available() > 0 && uartToTcpBuffer.freeSpace() > 0) {
    const size_t requestSize = min(
        static_cast<size_t>(uartPort.available()),
        min(sizeof(buffer), uartToTcpBuffer.freeSpace()));
    const size_t readSize = uartPort.readBytes(buffer, requestSize);
    if (readSize == 0) {
      break;
    }

    uartToTcpBuffer.push(buffer, readSize);
    markActivity();
  }

  if (uartPort.available() > 0 && uartToTcpBuffer.freeSpace() == 0 &&
      millis() - lastUartBackpressureLogAtMs >= kUartBackpressureLogIntervalMs) {
    ++uartBackpressureEvents;
    ++uartToTcpOverflowEvents;
    lastUartBackpressureLogAtMs = millis();
    debugPrintf(
        "Warning: UART backlog full (%" PRIu32 " bytes pending). Data loss is possible if the sender keeps streaming.\n",
        static_cast<uint32_t>(kPendingUartToTcpBytes));
  }
}

void flushUartBufferToTcp() {
  if (!isTcpClientConnected() || uartToTcpBuffer.size() == 0) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  while (isTcpClientConnected() && uartToTcpBuffer.size() > 0) {
    if (!tcpClient.connected()) {
      disconnectTcpClient("peer reset before write");
      break;
    }

    const size_t chunkSize = min(uartToTcpBuffer.size(), sizeof(buffer));
    uartToTcpBuffer.peek(buffer, chunkSize);
    const size_t written = tcpClient.write(buffer, chunkSize);
    if (written == 0) {
      if (!tcpClient.connected()) {
        disconnectTcpClient("peer reset during write");
      }
      break;
    }

    if (written < chunkSize) {
      ++tcpPartialWriteEvents;
    }

    uartToTcpBuffer.discard(written);
    uartToTcpBytes += written;
    markActivity();
  }
}

void logStatsIfNeeded(bool wifiConnected, bool accessPointActive, const char *apSsid) {
  if (millis() - lastStatsLogAtMs < kStatsLogIntervalMs) {
    return;
  }

  lastStatsLogAtMs = millis();
  const String ip = accessPointActive ? WiFi.softAPIP().toString() : (wifiConnected ? WiFi.localIP().toString() : String("-"));
  const String ssid = accessPointActive ? String(apSsid) : (wifiConnected ? WiFi.SSID() : String("-"));
  debugPrintf(
      "Stats: wifi=%s ap=%s ssid=%s ip=%s tcp=%s uart->tcp=%" PRIu64 " tcp->uart=%" PRIu64 " pending(u2t/t2u)=%" PRIu32 "/%" PRIu32 " tcp_conn=%" PRIu32 " tcp_disc=%" PRIu32 " wifi_reconn=%" PRIu32 " uart_backpressure=%" PRIu32 " uart_overflow=%" PRIu32 " tcp_overflow=%" PRIu32 " tcp_partial=%" PRIu32 "\n",
      wifiConnected ? "up" : "down",
      accessPointActive ? "on" : "off",
      ssid.c_str(),
      ip.c_str(),
      isTcpClientConnected() ? "up" : "down",
      static_cast<uint64_t>(uartToTcpBytes),
      static_cast<uint64_t>(tcpToUartBytes),
      static_cast<uint32_t>(uartToTcpBuffer.size()),
      static_cast<uint32_t>(tcpToUartBuffer.size()),
      tcpClientConnectCount,
      tcpClientDisconnectCount,
      wifiReconnectCountVal,
      uartBackpressureEvents,
      uartToTcpOverflowEvents,
      tcpToUartOverflowEvents,
      tcpPartialWriteEvents);
}

}
}
