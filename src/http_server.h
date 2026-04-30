/**
 * @file   http_server.h
 * @brief  HTTP 配置服务器（Web 页面 + JSON API）
 */
#pragma once

namespace wifi_uart {
namespace http_server {

/**
 * @brief 启动 HTTP 服务器（端口 80）
 *
 * 注册路由：
 * - GET  /             → 配置网页
 * - GET  /api/wifi     → Wi-Fi 状态及 Profile 列表
 * - POST /api/wifi/save     → 保存凭据到指定槽位
 * - POST /api/wifi/activate → 激活指定槽位 Profile
 * - POST /api/wifi/delete   → 删除指定槽位 Profile
 * - POST /api/wifi/scan     → 触发 Wi-Fi 扫描
 * - GET  /api/uart     → 当前 UART 参数
 * - POST /api/uart     → 应用新 UART 参数
 */
void begin();

/** @brief 处理一个 HTTP 请求（需在主循环中频繁调用） */
void handleClient();

}
}
