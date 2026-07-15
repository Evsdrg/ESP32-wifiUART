/**
 * @file   wifi_manager.h
 * @brief  Wi-Fi 连接状态机与 AP/STA 模式管理
 *
 * 负责：首次启动连接判定、STA 断线重连、AP  fallback、Wi-Fi 扫描。
 * 内部维护 pendingReconfigure 标志以在主循环中安全地切换网络配置。
 */
#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace wifi_manager {

/**
 * @brief 初始化 Wi-Fi 管理器
 * @param statusUpdateCallback 状态变化时调用的回调（可为 nullptr）
 *
 * 若存在激活的 Wi-Fi Profile 则启动 STA 模式并等待连接；
 * 否则直接启动 AP 模式。无论哪种模式，最终都会启动 TCP Server。
 */
void begin(void (*statusUpdateCallback)());

/**
 * @brief STA 模式主循环处理（断线检测、重连触发）
 *
 * 必须在主循环中定期调用。检测到 STA 断线时停止 TCP Server，
 * 达到重连间隔后自动尝试恢复连接。
 */
void handleStationMode();

/** @brief 请求在下一次主循环中重新应用网络配置（Profile 切换后触发） */
void requestReconfigure();

/** @brief 执行待处理的重新配置（仅在 pendingReconfigure==true 时有效） */
void applyPendingReconfigureIfNeeded();

/** @brief 任意网络模式（STA 或 AP）是否就绪 */
bool isNetworkReady();

/** @brief AP 模式是否激活 */
bool accessPointActive();

/** @brief Wi-Fi 扫描是否正在进行 */
bool scanInProgress();
bool scanFailed();

/** @brief 当前是否工作在 STA 模式 */
bool useStationMode();

/** @brief STA 模式是否已连接 */
bool wifiConnected();

/** @brief 当前连接的 SSID（STA 未连接时返回空字符串） */
String connectedSsid();

/**
 * @brief 当前 IP 地址
 *
 * AP 模式返回软 AP IP；STA 模式返回本地 IP；均未就绪时返回空字符串。
 */
String ipAddress();

/**
 * @brief 触发一次异步 Wi-Fi 扫描（后台运行）
 *
 * 扫描期间设备仍可正常通信。扫描完成后结果可通过 scanResult() 获取。
 */
void scanNearby();

/** @brief 轮询异步 Wi-Fi 扫描状态并在完成时缓存结果 */
void pollScan();

/** @brief 返回最近一次扫描的结果数量 */
size_t scanResultCount();

/**
 * @brief 获取指定索引的扫描结果
 * @param index 结果索引（需 < scanResultCount()）
 */
const WiFiScanResult &scanResult(size_t index);

/**
 * @brief 将 esp32 加密类型转换为可读字符串
 * @param encryptionType WiFiAuthMode_t 值
 */
String securityLabel(uint8_t encryptionType);

}
}
