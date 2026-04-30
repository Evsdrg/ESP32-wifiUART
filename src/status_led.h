#pragma once

#include "models.h"

namespace wifi_uart {

void initStatusLed();
void updateStatusLed(const StatusLedSnapshot &snapshot);

}