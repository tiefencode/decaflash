#pragma once

#include <stdint.h>

namespace decaflash::brain::wifi_manager {

bool credentialsAvailable();
bool connect();
void disconnect();
bool isConnected();
uint8_t currentChannel();
void scan();
void printStatus();

}  // namespace decaflash::brain::wifi_manager
