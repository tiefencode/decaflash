#pragma once

#include <stdint.h>

namespace decaflash::brain::matrix {

enum class MeterTheme : uint8_t {
  Default = 0,
  AiActive = 1,
};

void drawMicrophoneMeter(uint8_t filledPixels, MeterTheme theme = MeterTheme::Default);

}  // namespace decaflash::brain::matrix
