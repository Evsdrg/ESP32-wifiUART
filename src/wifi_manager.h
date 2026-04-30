#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace wifi_manager {

void begin(void (*statusUpdateCallback)());
void handleStationMode();
void requestReconfigure();
void applyPendingReconfigureIfNeeded();

bool isNetworkReady();
bool accessPointActive();
bool scanInProgress();
bool useStationMode();
bool wifiConnected();

String connectedSsid();
String ipAddress();

void scanNearby();
size_t scanResultCount();
const WiFiScanResult &scanResult(size_t index);
String securityLabel(uint8_t encryptionType);

}
}
