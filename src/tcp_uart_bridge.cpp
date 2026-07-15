/**
 * @file   tcp_uart_bridge.cpp
 * @brief  TCP ↔ UART 双向桥接核心实现
 *
 * 维护一个 TCP Server（接受单一客户端连接），通过两个 ByteRingBuffer
 * 实现双向数据转发：
 * - tcpToUartBuffer：TCP → UART（pullTcpIntoBuffer + flushTcpBufferToUart）
 * - uartToTcpBuffer：UART → TCP（pullUartIntoBuffer + flushUartBufferToTcp）
 *
 * 所有操作均为非阻塞，适配单线程事件循环。
 * 缓冲区支持 PSRAM（由 USE_PSRAM_BRIDGE_BUFFERS 控制）。
 */

#include "tcp_uart_bridge.h"
#include "app_config.h"
#include "byte_ring_buffer.h"
#include "debug_log.h"

#include <WiFi.h>
#include <cerrno>
#include <sys/socket.h>

namespace wifi_uart {
namespace bridge {

namespace {

/** @brief TCP Server 实例，监听 TCP_BRIDGE_PORT */
WiFiServer tcpServer(TCP_BRIDGE_PORT);

/** @brief 当前活跃的 TCP 客户端连接 */
WiFiClient tcpClient;

/** @brief TCP Server 是否已启动 */
bool tcpServerStarted = false;

/**
 * @brief 静态缓冲区（用于 USE_PSRAM_BRIDGE_BUFFERS=0 场景）
 *
 * 使用静态全局数组避免堆碎片；大小由 kPendingTcpToUartBytes / kPendingUartToTcpBytes 决定。
 */
#if !USE_PSRAM_BRIDGE_BUFFERS
uint8_t tcpToUartStorage[kPendingTcpToUartBytes] = {};
uint8_t uartToTcpStorage[kPendingUartToTcpBytes] = {};
#endif

/** @brief TCP→UART 方向缓冲区 */
ByteRingBuffer tcpToUartBuffer;

/** @brief UART→TCP 方向缓冲区 */
ByteRingBuffer uartToTcpBuffer;

/** @brief 最近一次通信活动的时间戳（供状态灯使用） */
uint32_t lastActivityAtMs = 0;

/** @brief 最近一次 UART 反压警告的时间（防重复日志） */
uint32_t lastUartBackpressureLogAtMs = 0;

/** @brief 最近一次统计日志的时间 */
uint32_t lastStatsLogAtMs = 0;

/** @brief TCP 客户端是否处于连接状态（会话级别标志） */
bool tcpClientActive = false;

/** @brief 累计 UART→TCP 字节数 */
uint64_t uartToTcpBytes = 0;

/** @brief 累计 TCP→UART 字节数 */
uint64_t tcpToUartBytes = 0;

/** @brief TCP 客户端连接总次数 */
uint32_t tcpClientConnectCount = 0;

/** @brief TCP 客户端主动断开总次数 */
uint32_t tcpClientDisconnectCount = 0;

/** @brief Wi-Fi 重连次数 */
uint32_t wifiReconnectCountVal = 0;

/** @brief UART RX FIFO 满导致丢帧次数 */
uint32_t uartBackpressureEvents = 0;

/** @brief UART→TCP 缓冲区溢出丢帧次数 */
uint32_t uartToTcpOverflowEvents = 0;

/** @brief TCP→UART 缓冲区溢出丢帧次数 */
uint32_t tcpToUartOverflowEvents = 0;

/** @brief TCP 写入未完成（缓冲区满导致截断）次数 */
uint32_t tcpPartialWriteEvents = 0;

/** @brief 单次 loop 允许提交给 TCP socket 的最大字节数 */
constexpr size_t kTcpSendBudgetPerLoop = kIoChunkSize * 4;

int sendNonBlocking(WiFiClient &client, const uint8_t *data, size_t size) {
  const int socketFd = client.fd();
  if (socketFd < 0) {
    errno = EBADF;
    return -1;
  }

  return send(socketFd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);
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

}  // namespace

bool beginBuffers() {
#if USE_PSRAM_BRIDGE_BUFFERS
  // PSRAM 模式：从 PSRAM 分配大缓冲区（12KB/20KB）
  const bool tcpReady = tcpToUartBuffer.begin(kPendingTcpToUartBytes);
  const bool uartReady = uartToTcpBuffer.begin(kPendingUartToTcpBytes);
#else
  // 普通模式：使用静态全局数组（零堆分配）
  const bool tcpReady = tcpToUartBuffer.begin(tcpToUartStorage, sizeof(tcpToUartStorage));
  const bool uartReady = uartToTcpBuffer.begin(uartToTcpStorage, sizeof(uartToTcpStorage));
#endif

  if (!tcpReady || !uartReady) {
    // 任一缓冲区失败时释放已分配资源
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
    return false;  // 防止重复启动
  }

  tcpServer.begin();
  tcpServer.setNoDelay(true);  // 禁用 Nagle 算法，降低 TCP 延迟
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

/**
 * @brief 主动断开 TCP 客户端并更新统计
 *
 * @param reason 断开原因描述（记录到日志）
 */
void disconnectTcpClient(const char *reason) {
  const bool hadSession = tcpClientActive;
  clearSessionBuffers();
  tcpClient.stop();              // 关闭 socket 并释放资源
  tcpClientActive = false;
  if (hadSession) {
    ++tcpClientDisconnectCount;
  }
  debugPrintf("TCP client disconnected: %s\n", reason);
}

/**
 * @brief 按需接受 TCP 客户端连接
 *
 * 逻辑：
 * 1. 若已有活跃连接：检测是否被对端关闭，必要时断开
 * 2. 若无活跃连接：尝试 accept 新连接
 *
 * 每次 loop 最多接受一个客户端，保证单一连接。
 */
void acceptClientIfNeeded(HardwareSerial &uartPort, bool allowNewClient) {
  if (!tcpServerStarted) {
    return;
  }

  // 检测现有连接是否被对端关闭
  if (tcpClientActive && !tcpClient.connected()) {
    disconnectTcpClient("peer closed");
  }

  // 已有活跃连接则不再接受新的
  if (tcpClientActive && tcpClient.connected()) {
    return;
  }

  WiFiClient newClient = tcpServer.accept();
  if (!newClient) {
    return;  // 尚无待处理的连接
  }
  if (!allowNewClient) {
    newClient.stop();
    return;
  }

  drainUartInput(uartPort);
  clearSessionBuffers();
  newClient.setNoDelay(true);  // 立即发送，无延迟
  tcpClient = newClient;
  tcpClientActive = true;
  ++tcpClientConnectCount;
  debugPrintf("TCP client connected: %s\n", tcpClient.remoteIP().toString().c_str());
}

bool isTcpClientConnected() {
  return tcpClientActive && tcpClient.connected();
}

/**
 * @brief 将 TCP 接收到的数据推入 tcp→uart 缓冲区
 *
 * 以 kIoChunkSize（256B）为单位分块读取，循环直到：
 * - TCP socket 无更多数据
 * - 缓冲区已满（此时增加 tcpToUartOverflowEvents）
 */
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

  // 缓冲区满但 socket 仍有数据，说明对端发送速度快于 UART 消费速度
  if (tcpClient.available() > 0 && tcpToUartBuffer.freeSpace() == 0) {
    ++tcpToUartOverflowEvents;
  }
}

/**
 * @brief 将 tcp→uart 缓冲区的内容 flush 到 UART 硬件
 *
 * 通过 availableForWrite() 获知 UART TX FIFO 剩余空间，
 * 以 kIoChunkSize 为单位分批写出，直到缓冲区空或 UART FIFO 满。
 */
void flushTcpBufferToUart(HardwareSerial &uartPort) {
  if (tcpToUartBuffer.size() == 0) {
    return;
  }

  const int uartWriteSpace = uartPort.availableForWrite();
  if (uartWriteSpace <= 0) {
    return;  // UART TX FIFO 满，稍后重试
  }

  uint8_t buffer[kIoChunkSize];
  while (tcpToUartBuffer.size() > 0) {
    const int availableSpace = uartPort.availableForWrite();
    if (availableSpace <= 0) {
      break;
    }

    // peek 读取数据（不删除），写出后用 discard 删除
    const size_t chunkSize = min(
        tcpToUartBuffer.size(),
        min(sizeof(buffer), static_cast<size_t>(availableSpace)));
    tcpToUartBuffer.peek(buffer, chunkSize);
    const size_t written = uartPort.write(buffer, chunkSize);
    if (written == 0) {
      break;  // UART TX FIFO 满，停止等待
    }

    tcpToUartBuffer.discard(written);
    tcpToUartBytes += written;
    markActivity();
  }
}

/**
 * @brief 从 UART 硬件读取数据推入 uart→tcp 缓冲区
 *
 * 通过 uartPort.available() 检测 RX FIFO 是否有数据，
 * 以 kIoChunkSize 为单位分块读取并推入缓冲区。
 *
 * 缓冲区满时记录反压事件（并限速日志输出）。
 */
void pullUartIntoBuffer(HardwareSerial &uartPort) {
  if (!isTcpClientConnected()) {
    drainUartInput(uartPort);
    return;
  }

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
    markActivity();
  }

  // 缓冲区满但 UART RX 仍有过量数据（对端发送过快）
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

/**
 * @brief 将 uart→tcp 缓冲区的内容 flush 到已连接的 TCP 客户端
 *
 * 直接对底层 socket 使用非阻塞 send，并限制每次 loop 的发送预算。
 * EAGAIN/EWOULDBLOCK 时保留缓冲区数据等待下轮，其他错误断开会话。
 */
void flushUartBufferToTcp() {
  if (!isTcpClientConnected() || uartToTcpBuffer.size() == 0) {
    return;
  }

  uint8_t buffer[kIoChunkSize];
  size_t sendBudget = kTcpSendBudgetPerLoop;
  while (isTcpClientConnected() && uartToTcpBuffer.size() > 0 && sendBudget > 0) {
    if (!tcpClient.connected()) {
      disconnectTcpClient("peer reset before write");
      break;
    }

    const size_t chunkSize = min(uartToTcpBuffer.size(), min(sizeof(buffer), sendBudget));
    uartToTcpBuffer.peek(buffer, chunkSize);
    const int written = sendNonBlocking(tcpClient, buffer, chunkSize);
    if (written < 0) {
      const int socketError = errno;
      if (socketError == EAGAIN || socketError == EWOULDBLOCK || socketError == EINTR) {
        break;
      }

      debugPrintf("TCP socket send failed: errno=%d\n", socketError);
      disconnectTcpClient("socket send failed");
      break;
    }

    if (written == 0) {
      disconnectTcpClient("socket send returned zero");
      break;
    }

    const size_t sentSize = static_cast<size_t>(written);
    if (sentSize < chunkSize) {
      ++tcpPartialWriteEvents;  // 记录部分写入
    }

    uartToTcpBuffer.discard(sentSize);
    uartToTcpBytes += sentSize;
    sendBudget -= sentSize;
    markActivity();
  }
}

/**
 * @brief 周期性打印桥接运行统计
 *
 * 包含：连接状态、IP、累计字节数、当前缓冲待处理量、
 * 连接/断开计数、各种异常事件计数。
 */
void logStatsIfNeeded(bool wifiConnected, bool accessPointActive, const char *apSsid) {
  if (millis() - lastStatsLogAtMs < kStatsLogIntervalMs) {
    return;
  }

  lastStatsLogAtMs = millis();
  const String ip = accessPointActive ? WiFi.softAPIP().toString()
                                        : (wifiConnected ? WiFi.localIP().toString() : String("-"));
  const String ssid = accessPointActive ? String(apSsid)
                                         : (wifiConnected ? WiFi.SSID() : String("-"));
  debugPrintf(
      "Stats: wifi=%s ap=%s ssid=%s ip=%s tcp=%s uart->tcp=%" PRIu64 " tcp->uart=%" PRIu64 " "
      "pending(u2t/t2u)=%" PRIu32 "/%" PRIu32 " tcp_conn=%" PRIu32 " tcp_disc=%" PRIu32 " "
      "wifi_reconn=%" PRIu32 " uart_backpressure=%" PRIu32 " uart_overflow=%" PRIu32 " "
      "tcp_overflow=%" PRIu32 " tcp_partial=%" PRIu32 "\n",
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
