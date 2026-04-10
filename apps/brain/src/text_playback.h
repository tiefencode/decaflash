#pragma once

#include <stdint.h>

namespace decaflash::brain::text_playback {

enum class Owner : uint8_t {
  Manual = 0,
  Ai = 1,
};

bool isActive();
bool isAiOwnedActive();
bool start(const char* text, uint32_t delayMs = 0, Owner owner = Owner::Manual);
void stop(bool announce = true);
bool stopAiOwned(bool announce = false);
void printHelp();
void serviceSerialInput();
bool serviceMatrix(uint32_t now);

}  // namespace decaflash::brain::text_playback
