#pragma once

#include <stdint.h>

namespace decaflash::brain::text_playback {

bool isActive();
void printHelp();
void serviceSerialInput();
bool serviceMatrix(uint32_t now);

}  // namespace decaflash::brain::text_playback
