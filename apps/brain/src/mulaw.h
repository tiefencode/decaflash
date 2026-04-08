#pragma once

#include <stdint.h>

namespace decaflash::brain::mulaw {

uint8_t encodeSample(int16_t sample);
int16_t decodeByte(uint8_t value);

}  // namespace decaflash::brain::mulaw
