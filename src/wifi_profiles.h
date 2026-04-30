/**
 * @file   wifi_profiles.h
 * @brief  Wi-Fi 连接凭据持久化管理
 *
 * 最多管理 kMaxWiFiProfiles 个凭据槽位，数据存储在 ESP32 NVS 分区。
 * 首次启动时如无有效凭据且定义了 WIFI_SSID，则自动种子写入槽位 0。
 */
#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace wifi_profiles {

/**
 * @brief 初始化 NVS 并加载所有凭据
 * @return true 初始化成功
 */
bool begin();

/** @brief 返回当前已存储的凭据数量 */
int count();

/**
 * @brief 获取当前激活的 Wi-Fi Profile
 * @return 指针（无激活 profile 时返回 nullptr）
 */
const WiFiProfile *active();

/** @brief 当前激活的槽位编号（-1 表示无激活） */
int8_t activeIndex();

/**
 * @brief 切换激活的凭据槽位
 * @param index 目标槽位（-1 表示清除激活状态）
 * @return true 切换成功
 */
bool setActiveIndex(int8_t index);

/**
 * @brief 获取指定槽位的凭据副本
 * @param index 槽位编号
 */
const WiFiProfile &profile(uint8_t index);

/** @brief 指定槽位是否包含有效凭据 */
bool profileInUse(uint8_t index);

/**
 * @brief 保存凭据到指定槽位（若密码为空则保留该槽位已有密码）
 * @param index     槽位编号
 * @param ssid      网络名称
 * @param password  密码（可为空字符串）
 * @return true 保存成功
 */
bool save(uint8_t index, const String &ssid, const String &password);

/** @brief 删除指定槽位的凭据（同时清除激活状态） */
void remove(uint8_t index);

}
}