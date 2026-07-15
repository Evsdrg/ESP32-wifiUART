/**
 * @file   wifi_profiles.cpp
 * @brief  Wi-Fi 凭据 NVS 持久化管理
 *
 * 所有 Profile 与 active index 存储在单个 "wcfg" blob 中，使保存、激活和删除
 * 都由一次 NVS commit 原子完成。旧版 "sXX"/"pXX"/"aidx" 键会在首次加载时迁移。
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
WiFiProfile profiles[kMaxWiFiProfiles] = {};
int8_t activeIndex_ = -1;
bool storageReady_ = false;

constexpr char kConfigKey[] = "wcfg";
constexpr char kMigrationKey[] = "wcfgok";
constexpr uint8_t kConfigMagic[] = {'W', 'F', 'C', '1'};
constexpr size_t kConfigHeaderSize = 8;
constexpr size_t kProfileSlotSize = 4 + 32 + 64;
constexpr size_t kConfigSize = kConfigHeaderSize + kMaxWiFiProfiles * kProfileSlotSize;

uint8_t encodedConfig[kConfigSize] = {};
uint8_t verifiedConfig[kConfigSize] = {};

int countStored() {
  int count = 0;
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (profiles[i].inUse) {
      ++count;
    }
  }
  return count;
}

void clearRuntimeConfig() {
  for (WiFiProfile &profile : profiles) {
    profile = {false, String(), String()};
  }
  activeIndex_ = -1;
}

String decodeString(const uint8_t *data, uint8_t length) {
  String value;
  value.reserve(length);
  for (uint8_t i = 0; i < length; ++i) {
    value += static_cast<char>(data[i]);
  }
  return value;
}

bool encodeConfig() {
  if (activeIndex_ >= 0 &&
      (activeIndex_ >= kMaxWiFiProfiles || !profiles[activeIndex_].inUse)) {
    return false;
  }

  memset(encodedConfig, 0, sizeof(encodedConfig));
  memcpy(encodedConfig, kConfigMagic, sizeof(kConfigMagic));
  encodedConfig[4] = activeIndex_ >= 0 ? static_cast<uint8_t>(activeIndex_) : 255;
  encodedConfig[5] = kMaxWiFiProfiles;

  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    const WiFiProfile &profile = profiles[i];
    if (profile.ssid.length() > 32 || profile.password.length() > 64 ||
        (profile.inUse && profile.ssid.isEmpty())) {
      return false;
    }

    uint8_t *slot = encodedConfig + kConfigHeaderSize + i * kProfileSlotSize;
    slot[0] = profile.inUse ? 1 : 0;
    slot[1] = static_cast<uint8_t>(profile.ssid.length());
    slot[2] = static_cast<uint8_t>(profile.password.length());
    if (profile.inUse) {
      memcpy(slot + 4, profile.ssid.c_str(), profile.ssid.length());
      memcpy(slot + 4 + 32, profile.password.c_str(), profile.password.length());
    }
  }
  return true;
}

bool persistConfig() {
  if (!encodeConfig()) {
    return false;
  }

  const size_t writeResult =
      preferencesStore.putBytes(kConfigKey, encodedConfig, sizeof(encodedConfig));
  memset(verifiedConfig, 0, sizeof(verifiedConfig));
  const bool exact = preferencesStore.isKey(kConfigKey) &&
      preferencesStore.getBytesLength(kConfigKey) == sizeof(verifiedConfig) &&
      preferencesStore.getBytes(kConfigKey, verifiedConfig, sizeof(verifiedConfig)) ==
          sizeof(verifiedConfig) &&
      memcmp(encodedConfig, verifiedConfig, sizeof(encodedConfig)) == 0;
  if (!exact) {
    debugPrintf("Failed to persist Wi-Fi config blob, writeResult=%u\n", writeResult);
  }
  return exact;
}

bool persistMigrationMarker() {
  if (preferencesStore.isKey(kMigrationKey) &&
      preferencesStore.getType(kMigrationKey) == PT_U8 &&
      preferencesStore.getUChar(kMigrationKey, 0) == 1) {
    return true;
  }
  preferencesStore.putUChar(kMigrationKey, 1);
  return preferencesStore.isKey(kMigrationKey) &&
      preferencesStore.getType(kMigrationKey) == PT_U8 &&
      preferencesStore.getUChar(kMigrationKey, 0) == 1;
}

void removeLegacyConfig() {
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    char ssidKey[4] = {};
    char passwordKey[4] = {};
    snprintf(ssidKey, sizeof(ssidKey), "s%02u", i);
    snprintf(passwordKey, sizeof(passwordKey), "p%02u", i);
    if (preferencesStore.isKey(ssidKey)) {
      preferencesStore.remove(ssidKey);
    }
    if (preferencesStore.isKey(passwordKey)) {
      preferencesStore.remove(passwordKey);
    }
  }
  if (preferencesStore.isKey("aidx")) {
    preferencesStore.remove("aidx");
  }
}

bool commitConfig() {
  if (!persistMigrationMarker() || !persistConfig()) {
    return false;
  }
  removeLegacyConfig();
  storageReady_ = true;
  return true;
}

bool loadConfigBlob() {
  if (preferencesStore.getBytesLength(kConfigKey) != sizeof(verifiedConfig) ||
      preferencesStore.getBytes(kConfigKey, verifiedConfig, sizeof(verifiedConfig)) !=
          sizeof(verifiedConfig) ||
      memcmp(verifiedConfig, kConfigMagic, sizeof(kConfigMagic)) != 0 ||
      verifiedConfig[5] != kMaxWiFiProfiles) {
    return false;
  }

  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    const uint8_t *slot = verifiedConfig + kConfigHeaderSize + i * kProfileSlotSize;
    if (slot[0] > 1 || slot[1] > 32 || slot[2] > 64 ||
        (slot[0] == 1 && slot[1] == 0)) {
      return false;
    }
  }

  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    const uint8_t *slot = verifiedConfig + kConfigHeaderSize + i * kProfileSlotSize;
    profiles[i] = slot[0] == 1
        ? WiFiProfile{
              true,
              decodeString(slot + 4, slot[1]),
              decodeString(slot + 4 + 32, slot[2])}
        : WiFiProfile{false, String(), String()};
  }

  const uint8_t savedIndex = verifiedConfig[4];
  activeIndex_ = savedIndex < kMaxWiFiProfiles && profiles[savedIndex].inUse
      ? static_cast<int8_t>(savedIndex)
      : -1;
  return savedIndex == 255 || activeIndex_ >= 0;
}

void makeLegacyKeys(uint8_t index, char *ssidKey, char *passwordKey) {
  snprintf(ssidKey, 4, "s%02u", index);
  snprintf(passwordKey, 4, "p%02u", index);
}

bool loadLegacyString(const char *key, size_t maxLength, String &value) {
  if (!preferencesStore.isKey(key)) {
    value = String();
    return true;
  }
  if (preferencesStore.getType(key) != PT_STR) {
    return false;
  }
  const size_t storedLength = preferencesStore.getStringLength(key);
  if (storedLength == 0 || storedLength - 1 > maxLength) {
    return false;
  }
  value = preferencesStore.getString(key, String());
  return value.length() + 1 == storedLength;
}

bool loadLegacyConfig() {
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    char ssidKey[4] = {};
    char passwordKey[4] = {};
    makeLegacyKeys(i, ssidKey, passwordKey);
    String ssid;
    String password;
    if (!loadLegacyString(ssidKey, 32, ssid) ||
        !loadLegacyString(passwordKey, 64, password)) {
      return false;
    }
    profiles[i] = {!ssid.isEmpty(), ssid, password};
  }

  uint8_t savedIndex = 255;
  if (preferencesStore.isKey("aidx")) {
    if (preferencesStore.getType("aidx") != PT_U8) {
      return false;
    }
    const uint8_t firstRead = preferencesStore.getUChar("aidx", 254);
    const uint8_t secondRead = preferencesStore.getUChar("aidx", 253);
    if (firstRead != secondRead) {
      return false;
    }
    savedIndex = firstRead;
  }
  activeIndex_ = savedIndex < kMaxWiFiProfiles && profiles[savedIndex].inUse
      ? static_cast<int8_t>(savedIndex)
      : -1;
  return savedIndex == 255 || activeIndex_ >= 0;
}

bool seedDefaultProfileIfNeeded(bool &seeded) {
  seeded = false;
  if (countStored() > 0 || std::strlen(WIFI_SSID) == 0) {
    return true;
  }
  if (std::strlen(WIFI_SSID) > 32 || std::strlen(WIFI_PASSWORD) > 64) {
    debugPrintln("Default Wi-Fi profile from build flags is too long");
    return false;
  }

  profiles[0] = {true, String(WIFI_SSID), String(WIFI_PASSWORD)};
  activeIndex_ = 0;
  seeded = true;
  return true;
}

}  // namespace

bool begin() {
  storageReady_ = false;
  if (!preferencesStore.begin("bridgecfg", false)) {
    return false;
  }

  bool needsPersist = false;
  if (preferencesStore.isKey(kConfigKey)) {
    if (!loadConfigBlob()) {
      debugPrintln("Invalid Wi-Fi config blob; refusing legacy fallback");
      clearRuntimeConfig();
      return false;
    }
  } else {
    if (preferencesStore.isKey(kMigrationKey)) {
      debugPrintln("Wi-Fi config blob is missing after migration; refusing legacy fallback");
      clearRuntimeConfig();
      return false;
    }
    if (!loadLegacyConfig()) {
      debugPrintln("Failed to read legacy Wi-Fi config; refusing migration");
      clearRuntimeConfig();
      return false;
    }
    needsPersist = true;
  }
  bool seeded = false;
  if (!seedDefaultProfileIfNeeded(seeded)) {
    return false;
  }
  needsPersist = needsPersist || seeded;
  if (!persistMigrationMarker() || (needsPersist && !persistConfig())) {
    return false;
  }
  removeLegacyConfig();
  storageReady_ = true;
  if (seeded) {
    debugPrintf("Seeded default Wi-Fi profile from build flags: %s\n", WIFI_SSID);
  }

  debugPrintf("Loaded Wi-Fi profiles: count=%d active=%d\n", countStored(), activeIndex_);
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

bool ready() {
  return storageReady_;
}

bool setActiveIndex(int8_t index) {
  if (index < -1 || index >= kMaxWiFiProfiles ||
      (index >= 0 && !profiles[index].inUse)) {
    return false;
  }
  const int8_t previousIndex = activeIndex_;
  activeIndex_ = index;
  if (!commitConfig()) {
    activeIndex_ = previousIndex;
    return false;
  }
  return true;
}

const WiFiProfile &profile(uint8_t index) {
  return profiles[index];
}

bool profileInUse(uint8_t index) {
  return index < kMaxWiFiProfiles && profiles[index].inUse;
}

bool save(
    uint8_t index,
    const String &ssid,
    const String &password,
    bool activate) {
  if (index >= kMaxWiFiProfiles || ssid.isEmpty() || ssid.length() > 32 ||
      password.length() > 64) {
    return false;
  }

  const WiFiProfile previousProfile = profiles[index];
  const int8_t previousIndex = activeIndex_;
  profiles[index] = {true, ssid, password};
  if (activate) {
    activeIndex_ = static_cast<int8_t>(index);
  }
  if (!commitConfig()) {
    profiles[index] = previousProfile;
    activeIndex_ = previousIndex;
    return false;
  }
  return true;
}

bool remove(uint8_t index) {
  if (index >= kMaxWiFiProfiles) {
    return false;
  }

  const WiFiProfile previousProfile = profiles[index];
  const int8_t previousIndex = activeIndex_;
  profiles[index] = {false, String(), String()};
  if (activeIndex_ == static_cast<int8_t>(index)) {
    activeIndex_ = -1;
  }
  if (!commitConfig()) {
    profiles[index] = previousProfile;
    activeIndex_ = previousIndex;
    return false;
  }
  return true;
}

}
}
