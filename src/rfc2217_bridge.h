#pragma once

#include <Arduino.h>

namespace wifi_uart {
namespace rfc2217_bridge {

/** @brief Initialize RFC2217 session buffers and optional control GPIOs. */
bool beginBuffers();

/** @brief Start the RFC2217 TCP server when the network is ready. */
bool startServer();

/** @brief Stop the RFC2217 TCP server and disconnect any active client. */
void stopServer();

/** @brief Disconnect the active RFC2217 client, if any. */
void disconnectClient(const char *reason);

/** @brief Accept one pending RFC2217 client when no RFC2217 session is active. */
void acceptClientIfNeeded(HardwareSerial &uartPort, bool allowNewClient = true);

/** @brief Service RFC2217 control and data paths for the active client. */
void handleClient(HardwareSerial &uartPort);

/** @brief Return whether an RFC2217 client is currently connected. */
bool isClientConnected();

}
}
