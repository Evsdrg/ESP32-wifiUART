/**
 * @file   main.cpp
 * @brief  Arduino 框架入口
 *
 * 极简入口文件，仅转发 setup()/loop() 到 wifi_uart::app 命名空间，
 * 所有业务逻辑由 app.cpp 中的模块负责初始化和调度。
 */

#include "app.h"

void setup() {
  wifi_uart::app::setup();
}

void loop() {
  wifi_uart::app::loop();
}
