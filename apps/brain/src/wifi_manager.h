#pragma once

#include <stdint.h>

namespace decaflash::brain::wifi_manager {

bool credentialsAvailable();
bool connect(bool renderStatus = false);
void disconnect();
bool isConnected();
uint8_t currentChannel();
bool statusPixelColor(uint32_t now, uint32_t& colorValue);
void scan();
void printStatus();

}  // namespace decaflash::brain::wifi_manager
