/**
 * @file   status_led.cpp
 * @brief  状态灯驱动实现
 *
 * 支持两种硬件模式：
 * - RGB WS2812 模式（USE_RGB_STATUS_LED=1）：全彩颜色表达
 * - 普通 GPIO 模式（USE_RGB_STATUS_LED=0）：单色闪烁表达
 *
 * 状态灯通过颜色/闪烁模式反映：Wi-Fi 连接、TCP 客户端、AP 模式、
 * Wi-Fi 扫描、数据活动。
 */

#include "status_led.h"
#include "app_config.h"

namespace wifi_uart {

namespace {

/**
 * @brief 设置 LED 为亮/灭（GPIO 模式）
 * @param on true 亮，false 灭
 */
void writeStatusLed(bool on) {
#if USE_RGB_STATUS_LED
  // RGB 模式下关闭 = 全黑
  const uint8_t brightness = on ? STATUS_RGB_BRIGHTNESS : 0;
  rgbLedWrite(STATUS_LED_PIN, 0, brightness, 0);
#else
  const int activeLevel = STATUS_LED_ACTIVE_LEVEL ? HIGH : LOW;
  const int inactiveLevel = STATUS_LED_ACTIVE_LEVEL ? LOW : HIGH;
  digitalWrite(STATUS_LED_PIN, on ? activeLevel : inactiveLevel);
#endif
}

/**
 * @brief 设置 LED 为指定 RGB 颜色（RGB 模式）
 * @param color 目标颜色
 */
void writeStatusLedColor(const RgbColor &color) {
#if USE_RGB_STATUS_LED
  rgbLedWrite(STATUS_LED_PIN, color.red, color.green, color.blue);
#else
  // GPIO 模式：将任意非零分量视为"亮"
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

  // 根据网络状态确定基础颜色
  RgbColor baseColor = {brightness, 0, 0};  // 默认红色：未连接
  if (snapshot.tcpClientConnected) {
    // 粉色：TCP 已连接（红+蓝）
    baseColor = {brightness, 0, brightness};
  } else if (snapshot.wifiConnected) {
    // 绿色：Wi-Fi STA 已连接
    baseColor = {0, brightness, 0};
  } else if (snapshot.accessPointActive) {
    // 橙色：AP 模式激活（红+淡绿）
    baseColor = {brightness, static_cast<uint8_t>(brightness / 3), 0};
  }

  // 计算距最近活动的时间，用于活动脉冲效果
  const uint32_t activityAgeMs = millis() - snapshot.lastActivityAtMs;
  const bool inActivityWindow = activityAgeMs <= kActivityFlashWindowMs;
  const bool firstPulse = activityAgeMs < kActivityPulseMs;
  // 双脉冲：活动时产生两次短促蓝灯闪烁
  const bool secondPulse =
      activityAgeMs >= (kActivityPulseMs + kActivityPulseGapMs) &&
      activityAgeMs < (2 * kActivityPulseMs + kActivityPulseGapMs);
  // 扫描指示灯：每 180ms 切换一次
  const bool scanBlinkOn = (millis() / 180) % 2 == 0;

  if (snapshot.wifiScanInProgress) {
    // 扫描中：绿色灯每 180ms 闪烁
    writeStatusLedColor(scanBlinkOn ? RgbColor{0, brightness, 0} : RgbColor{0, 0, 0});
  } else if (inActivityWindow) {
    // 有数据活动：双脉冲蓝色闪烁
    writeStatusLedColor((firstPulse || secondPulse) ? RgbColor{0, 0, brightness} : RgbColor{0, 0, 0});
  } else {
    // 常态：显示基础颜色
    writeStatusLedColor(baseColor);
  }
#else
  // GPIO 模式：使用闪烁频率和常亮/常灭表达状态
  const uint32_t activityAgeMs = millis() - snapshot.lastActivityAtMs;
  const bool inDataFlashWindow = activityAgeMs <= kDataFlashWindowMs;

  bool ledOn = false;
  if (inDataFlashWindow) {
    // 数据活动窗口内：40ms 切换一次（快速闪烁）
    ledOn = (millis() / 40) % 2 == 0;
  } else if (snapshot.tcpClientConnected) {
    ledOn = true;  // 常亮：TCP 已连接
  } else if (snapshot.wifiConnected) {
    // 以 kWifiBlinkPeriodMs 为周期切换（慢速闪烁）
    ledOn = (millis() / kWifiBlinkPeriodMs) % 2 == 0;
  } else {
    // 未连接：更快频率闪烁（kDisconnectedBlinkPeriodMs）
    ledOn = (millis() / kDisconnectedBlinkPeriodMs) % 2 == 0;
  }

  writeStatusLed(ledOn);
#endif
}

}