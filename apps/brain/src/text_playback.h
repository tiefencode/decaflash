#pragma once

#include <stdint.h>

namespace decaflash::brain::text_playback {

bool isActive();
bool start(const char* text);
void stop(bool announce = true);
void printHelp();
void serviceSerialInput();
bool serviceMatrix(uint32_t now);

}  // namespace decaflash::brain::text_playback
