/**
 * @file   tcp_uart_bridge.h
 * @brief  TCP ↔ UART 双向桥接核心逻辑
 *
 * 在 TCP Server 模式下接受一个客户端连接，然后将两端数据相互转发：
 * - TCP → UART：pullTcpIntoBuffer() + flushTcpBufferToUart()
 * - UART → TCP：pullUartIntoBuffer() + flushUartBufferToTcp()
 *
 * 内部维护两个 ByteRingBuffer 作为收发缓冲，支持 PSRAM。
 */
#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace bridge {

/**
 * @brief 初始化 TCP↔UART 环形缓冲区
 * @return true 分配成功
 *
 * 根据 USE_PSRAM_BRIDGE_BUFFERS 决定使用 PSRAM 或普通堆内存。
 */
bool beginBuffers();

/** @brief 清空本次 session 的收发缓冲区内容 */
void clearSessionBuffers();

/** @brief 记录一次 Wi-Fi 重连事件（供统计使用） */
void markWifiReconnect();

/** @brief 返回最近一次通信活动的时间戳（millis） */
uint32_t lastActivityMs();

/** @brief 记录一次桥接活动（供状态灯和统计使用） */
void markActivity();

/** @brief 启动 TCP Server（端口由 TCP_BRIDGE_PORT 指定） */
bool startTcpServer();

/** @brief 停止 TCP Server */
void stopTcpServer();

/**
 * @brief 检查是否有待接受的客户端，必要时接受
 *
 * 在 TCP Server 未启动时直接返回；若当前无客户端则尝试接受新连接。
 */
void acceptClientIfNeeded();

/**
 * @brief 主动断开当前 TCP 客户端连接
 * @param reason 断开原因描述（用于日志）
 */
void disconnectTcpClient(const char *reason);

/** @brief 是否有 TCP 客户端处于连接状态 */
bool isTcpClientConnected();

/** @brief 从 TCP Socket 读取数据写入 tcp→uart 缓冲区 */
void pullTcpIntoBuffer();

/**
 * @brief 将 tcp→uart 缓冲区内容刷新到 UART 硬件
 * @param uartPort 目标串口实例
 */
void flushTcpBufferToUart(HardwareSerial &uartPort);

/**
 * @brief 从 UART 硬件读取数据写入 uart→tcp 缓冲区
 * @param uartPort 源串口实例
 */
void pullUartIntoBuffer(HardwareSerial &uartPort);

/** @brief 将 uart→tcp 缓冲区内容发送到已连接的 TCP 客户端 */
void flushUartBufferToTcp();

/**
 * @brief 周期性打印桥接统计信息
 * @param wifiConnected      Wi-Fi STA 连接状态
 * @param accessPointActive  AP 模式状态
 * @param apSsid            AP SSID（AP 模式未激活时忽略）
 */
void logStatsIfNeeded(bool wifiConnected, bool accessPointActive, const char *apSsid);

}
}
