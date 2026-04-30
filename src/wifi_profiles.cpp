/**
 * @file   wifi_profiles.cpp
 * @brief  Wi-Fi 凭据 NVS 持久化管理
 *
 * 使用 Preferences（基于 NVS分区）存储最多 kMaxWiFiProfiles 个凭据槽位，
 * 每个槽位以 "sXX"/"pXX" 键名存储 SSID/密码，以 "aidx" 存储激活槽位编号。
 *
 * 首次启动时（如 build flags 中定义了 WIFI_SSID），自动将默认凭据种子写入槽位 0，
 * 方便开箱即用而无需通过网页配置。
 */

#include "wifi_profiles.h"
#include "app_config.h"
#include "debug_log.h"
#include <Preferences.h>
#include <cstring>

namespace wifi_uart {
namespace wifi_profiles {

namespace {

Preferences preferencesStore;

/** @brief 内存中缓存的所有凭据（启动时从 NVS 加载） */
WiFiProfile profiles[kMaxWiFiProfiles] = {};

/** @brief 当前激活的槽位编号；-1 表示无激活 */
int8_t activeIndex_ = -1;

/**
 * @brief 生成指定槽位的 NVS 键名
 *
 * SSID 键格式：sXX（如 s00, s01）
 * 密码键格式：pXX（如 p00, p01）
 */
void makeWiFiProfileKeys(uint8_t index, char *ssidKey, char *passwordKey) {
  snprintf(ssidKey, 4, "s%02u", index);
  snprintf(passwordKey, 4, "p%02u", index);
}

/** @brief 返回已存储（inUse=true）的凭据数量 */
int countStored() {
  int cnt = 0;
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (profiles[i].inUse) {
      ++cnt;
    }
  }
  return cnt;
}

/** @brief 将 activeIndex_ 写入 NVS（255 表示无激活） */
void persistActiveIndex() {
  preferencesStore.putUChar("aidx", activeIndex_ >= 0 ? static_cast<uint8_t>(activeIndex_) : 255);
}

/**
 * @brief 从 NVS 加载所有凭据到内存
 *
 * 遍历所有槽位读取 "sXX"/"pXX"；读取 "aidx" 作为激活槽位。
 * 仅当目标槽位已有 SSID 时才标记 inUse=true。
 */
void loadProfiles() {
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    char ssidKey[4] = {0};
    char passwordKey[4] = {0};
    makeWiFiProfileKeys(i, ssidKey, passwordKey);
    const String ssid =
        preferencesStore.isKey(ssidKey) ? preferencesStore.getString(ssidKey, String()) : String();
    const String password =
        preferencesStore.isKey(passwordKey) ? preferencesStore.getString(passwordKey, String()) : String();
    profiles[i] = {!ssid.isEmpty(), ssid, password};
  }

  const uint8_t savedIndex = preferencesStore.getUChar("aidx", 255);
  if (savedIndex < kMaxWiFiProfiles && profiles[savedIndex].inUse) {
    activeIndex_ = static_cast<int8_t>(savedIndex);
  } else {
    activeIndex_ = -1;
  }
}

/**
 * @brief 首次启动时从 build flags 种子默认凭据
 *
 * 仅在 NVS 中无任何凭据（countStored()==0）且定义了 WIFI_SSID 时执行。
 * 将 WIFI_SSID/WIFI_PASSWORD 写入槽位 0 并设为激活状态。
 */
void seedDefaultProfileIfNeeded() {
  if (countStored() > 0 || std::strlen(WIFI_SSID) == 0) {
    return;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(0, ssidKey, passwordKey);

  // putString 返回写入字节数；0 表示失败（通常是 NVS 空间不足）
  if (preferencesStore.putString(ssidKey, WIFI_SSID) == 0) {
    debugPrintln("Failed to seed default Wi-Fi profile SSID");
    return;
  }

  preferencesStore.putString(passwordKey, WIFI_PASSWORD);
  profiles[0] = {true, String(WIFI_SSID), String(WIFI_PASSWORD)};
  activeIndex_ = 0;
  persistActiveIndex();
  debugPrintf("Seeded default Wi-Fi profile from build flags: %s\n", WIFI_SSID);
}

}  // namespace

bool begin() {
  // 以读写模式打开 NVS namespace "bridgecfg"（第二个参数 false = 读写）
  if (!preferencesStore.begin("bridgecfg", false)) {
    return false;
  }

  loadProfiles();
  seedDefaultProfileIfNeeded();

  debugPrintf(
      "Loaded Wi-Fi profiles: count=%d active=%d\n",
      countStored(),
      activeIndex_);
  if (activeIndex_ >= 0) {
    debugPrintf(
        "Active Wi-Fi profile SSID: %s\n",
        profiles[activeIndex_].ssid.c_str());
  }

  return true;
}

int count() {
  return countStored();
}

const WiFiProfile *active() {
  if (activeIndex_ < 0 || activeIndex_ >= kMaxWiFiProfiles) {
    return nullptr;
  }
  return profiles[activeIndex_].inUse ? &profiles[activeIndex_] : nullptr;
}

int8_t activeIndex() {
  return activeIndex_;
}

bool setActiveIndex(int8_t index) {
  if (index < 0) {
    // 清除激活状态
    activeIndex_ = -1;
    persistActiveIndex();
    return true;
  }

  if (index >= kMaxWiFiProfiles || !profiles[index].inUse) {
    return false;
  }

  activeIndex_ = index;
  persistActiveIndex();
  return true;
}

const WiFiProfile &profile(uint8_t index) {
  return profiles[index];
}

bool profileInUse(uint8_t index) {
  return index < kMaxWiFiProfiles && profiles[index].inUse;
}

/**
 * @brief 保存凭据到指定槽位
 *
 * 若 password 为空且该槽位已有凭据，则保留原密码（实现"仅修改 SSID"场景）。
 *
 * @param index    目标槽位
 * @param ssid     网络名称（不可为空）
 * @param password 密码（可为空：开放网络或保留原密码）
 * @return true 保存成功
 */
bool save(uint8_t index, const String &ssid, const String &password) {
  if (index >= kMaxWiFiProfiles || ssid.isEmpty()) {
    return false;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(index, ssidKey, passwordKey);

  if (preferencesStore.putString(ssidKey, ssid) == 0) {
    return false;
  }

  // 密码为空时保留原密码，实现渐进式修改
  String effectivePassword = password;
  if (password.isEmpty() && profiles[index].inUse) {
    effectivePassword = profiles[index].password;
  }
  preferencesStore.putString(passwordKey, effectivePassword);
  profiles[index] = {true, ssid, effectivePassword};
  return true;
}

/**
 * @brief 删除指定槽位的凭据
 *
 * 从 NVS 中移除键值，并重置内存中对应槽位为未使用状态。
 * 若删除的是当前激活槽位，则同时清除激活状态（切换到 AP fallback）。
 */
void remove(uint8_t index) {
  if (index >= kMaxWiFiProfiles) {
    return;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(index, ssidKey, passwordKey);
  preferencesStore.remove(ssidKey);
  preferencesStore.remove(passwordKey);
  profiles[index] = {false, String(), String()};

  if (activeIndex_ == static_cast<int8_t>(index)) {
    setActiveIndex(-1);
  }
}

}
}
