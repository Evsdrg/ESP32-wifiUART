#include "status_led.h"
#include "app_config.h"

namespace wifi_uart {

namespace {

void writeStatusLed(bool on) {
#if USE_RGB_STATUS_LED
  const uint8_t brightness = on ? STATUS_RGB_BRIGHTNESS : 0;
  rgbLedWrite(STATUS_LED_PIN, 0, brightness, 0);
#else
  const int activeLevel = STATUS_LED_ACTIVE_LEVEL ? HIGH : LOW;
  const int inactiveLevel = STATUS_LED_ACTIVE_LEVEL ? LOW : HIGH;
  digitalWrite(STATUS_LED_PIN, on ? activeLevel : inactiveLevel);
#endif
}

void writeStatusLedColor(const RgbColor &color) {
#if USE_RGB_STATUS_LED
  rgbLedWrite(STATUS_LED_PIN, color.red, color.green, color.blue);
#else
  writeStatusLed(color.red != 0 || color.green != 0 || color.blue != 0);
#endif
}

}  // namespace

void initStatusLed() {
#if USE_RGB_STATUS_LED
  writeStatusLed(false);
#else
  pinMode(STATUS_LED_PIN, OUTPUT);
  writeStatusLed(false);
#endif
}

void updateStatusLed(const StatusLedSnapshot &snapshot) {
#if USE_RGB_STATUS_LED
  constexpr uint8_t brightness = STATUS_RGB_BRIGHTNESS;

  RgbColor baseColor = {brightness, 0, 0};
  if (snapshot.tcpClientConnected) {
    baseColor = {brightness, 0, brightness};
  } else if (snapshot.wifiConnected) {
    baseColor = {0, brightness, 0};
  } else if (snapshot.accessPointActive) {
    baseColor = {brightness, static_cast<uint8_t>(brightness / 3), 0};
  }

  const uint32_t activityAgeMs = millis() - snapshot.lastActivityAtMs;
  const bool inActivityWindow = activityAgeMs <= kActivityFlashWindowMs;
  const bool firstPulse = activityAgeMs < kActivityPulseMs;
  const bool secondPulse =
      activityAgeMs >= (kActivityPulseMs + kActivityPulseGapMs) &&
      activityAgeMs < (2 * kActivityPulseMs + kActivityPulseGapMs);
  const bool scanBlinkOn = (millis() / 180) % 2 == 0;

  if (snapshot.wifiScanInProgress) {
    writeStatusLedColor(scanBlinkOn ? RgbColor{0, brightness, 0} : RgbColor{0, 0, 0});
  } else if (inActivityWindow) {
    writeStatusLedColor((firstPulse || secondPulse) ? RgbColor{0, 0, brightness} : RgbColor{0, 0, 0});
  } else {
    writeStatusLedColor(baseColor);
  }
#else
  const uint32_t activityAgeMs = millis() - snapshot.lastActivityAtMs;
  const bool inDataFlashWindow = activityAgeMs <= kDataFlashWindowMs;

  bool ledOn = false;
  if (inDataFlashWindow) {
    ledOn = (millis() / 40) % 2 == 0;
  } else if (snapshot.tcpClientConnected) {
    ledOn = true;
  } else if (snapshot.wifiConnected) {
    ledOn = (millis() / kWifiBlinkPeriodMs) % 2 == 0;
  } else {
    ledOn = (millis() / kDisconnectedBlinkPeriodMs) % 2 == 0;
  }

  writeStatusLed(ledOn);
#endif
}

}