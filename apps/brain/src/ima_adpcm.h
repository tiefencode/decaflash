#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::ima_adpcm {

struct State {
  int16_t predictor = 0;
  uint8_t index = 0;
};

static constexpr size_t kHeaderSize = 4;

size_t requiredBytesForSamples(size_t sampleCount);
void writeHeader(uint8_t* destination, const State& state);
bool readHeader(const uint8_t* source, size_t length, State& state);
uint8_t encodeSample(int16_t sample, State& state);
int16_t decodeNibble(uint8_t nibble, State& state);

}  // namespace decaflash::brain::ima_adpcm
