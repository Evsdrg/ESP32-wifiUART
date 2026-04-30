#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace wifi_profiles {

bool begin();
int count();
const WiFiProfile *active();
int8_t activeIndex();
bool setActiveIndex(int8_t index);

const WiFiProfile &profile(uint8_t index);
bool profileInUse(uint8_t index);
bool save(uint8_t index, const String &ssid, const String &password);
void remove(uint8_t index);

}
}