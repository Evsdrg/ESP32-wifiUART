/**
 * @file   app.h
 * @brief  应用层入口，桥接 Arduino framework 与业务模块
 */
#pragma once

namespace wifi_uart {
namespace app {

/**
 * @brief 设备初始化入口
 *
 * 依次完成：Wi-Fi 配置存储、TCP-UART 桥接缓冲区、UART 端口、状态灯、
 * Wi-Fi 管理器、HTTP 服务器的初始化。任意环节失败将重启设备。
 */
void setup();

/**
 * @brief 主循环处理
 *
 * 执行：Wi-Fi 状态维护、HTTP 请求处理、桥接数据转发（TCP→UART、UART→TCP）、
 * 状态灯更新。循环以 ~1ms 间隔运行。
 */
void loop();

}
}
