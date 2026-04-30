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

void makeWiFiProfileKeys(uint8_t index, char *ssidKey, char *passwordKey) {
  snprintf(ssidKey, 4, "s%02u", index);
  snprintf(passwordKey, 4, "p%02u", index);
}

int countStored() {
  int cnt = 0;
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    if (profiles[i].inUse) {
      ++cnt;
    }
  }
  return cnt;
}

void persistActiveIndex() {
  preferencesStore.putUChar("aidx", activeIndex_ >= 0 ? static_cast<uint8_t>(activeIndex_) : 255);
}

void loadProfiles() {
  for (uint8_t i = 0; i < kMaxWiFiProfiles; ++i) {
    char ssidKey[4] = {0};
    char passwordKey[4] = {0};
    makeWiFiProfileKeys(i, ssidKey, passwordKey);
    const String ssid = preferencesStore.isKey(ssidKey) ? preferencesStore.getString(ssidKey, String()) : String();
    const String password = preferencesStore.isKey(passwordKey) ? preferencesStore.getString(passwordKey, String()) : String();
    profiles[i] = {!ssid.isEmpty(), ssid, password};
  }

  const uint8_t savedIndex = preferencesStore.getUChar("aidx", 255);
  if (savedIndex < kMaxWiFiProfiles && profiles[savedIndex].inUse) {
    activeIndex_ = static_cast<int8_t>(savedIndex);
  } else {
    activeIndex_ = -1;
  }
}

void seedDefaultProfileIfNeeded() {
  if (countStored() > 0 || std::strlen(WIFI_SSID) == 0) {
    return;
  }

  char ssidKey[4] = {0};
  char passwordKey[4] = {0};
  makeWiFiProfileKeys(0, ssidKey, passwordKey);
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

  preferencesStore.putString(passwordKey, password);
  profiles[index] = {true, ssid, password};
  return true;
}

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
