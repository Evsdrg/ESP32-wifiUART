#pragma once

#include <Arduino.h>
#include "models.h"

namespace wifi_uart {
namespace bridge {

bool beginBuffers();
void clearSessionBuffers();
void markWifiReconnect();
uint32_t lastActivityMs();
bool startTcpServer();
void stopTcpServer();
void acceptClientIfNeeded();
void disconnectTcpClient(const char *reason);
bool isTcpClientConnected();
void pullTcpIntoBuffer();
void flushTcpBufferToUart(HardwareSerial &uartPort);
void pullUartIntoBuffer(HardwareSerial &uartPort);
void flushUartBufferToTcp();
void logStatsIfNeeded(bool wifiConnected, bool accessPointActive, const char *apSsid);

}
}
