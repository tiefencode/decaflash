#include "ima_adpcm.h"

#include <cstring>

namespace decaflash::brain::ima_adpcm {

namespace {

static constexpr int16_t kStepTable[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21,
  23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
  73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
  230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
  724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
  2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
  7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
  22385, 24623, 27086, 29794, 32767
};

static constexpr int8_t kIndexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8,
  -1, -1, -1, -1, 2, 4, 6, 8
};

int16_t clampPredictor(int32_t value) {
  if (value > INT16_MAX) {
    return INT16_MAX;
  }
  if (value < INT16_MIN) {
    return INT16_MIN;
  }
  return static_cast<int16_t>(value);
}

uint8_t clampIndex(int32_t value) {
  if (value < 0) {
    return 0;
  }
  if (value > 88) {
    return 88;
  }
  return static_cast<uint8_t>(value);
}

}  // namespace

size_t requiredBytesForSamples(size_t sampleCount) {
  if (sampleCount == 0) {
    return 0;
  }

  size_t total = 0;
  size_t remaining = sampleCount;
  while (remaining > 0) {
    const size_t blockSamples = (remaining < kBlockSampleCount) ? remaining : kBlockSampleCount;
    total += requiredBytesForBlock(blockSamples);
    remaining -= blockSamples;
  }
  return total;
}

size_t requiredBytesForBlock(size_t sampleCount) {
  if (sampleCount == 0) {
    return 0;
  }

  const size_t encodedSampleCount = sampleCount - 1U;
  return kHeaderSize + ((encodedSampleCount + 1U) / 2U);
}

void writeHeader(uint8_t* destination, const State& state) {
  if (destination == nullptr) {
    return;
  }

  destination[0] = static_cast<uint8_t>(state.predictor & 0xFF);
  destination[1] = static_cast<uint8_t>((state.predictor >> 8) & 0xFF);
  destination[2] = state.index;
  destination[3] = 0;
}

bool readHeader(const uint8_t* source, size_t length, State& state) {
  if (source == nullptr || length < kHeaderSize) {
    return false;
  }

  state.predictor = static_cast<int16_t>(
    static_cast<uint16_t>(source[0]) |
    (static_cast<uint16_t>(source[1]) << 8)
  );
  state.index = clampIndex(source[2]);
  return true;
}

uint8_t encodeSample(int16_t sample, State& state) {
  const int32_t step = kStepTable[state.index];
  int32_t delta = static_cast<int32_t>(sample) - state.predictor;
  uint8_t code = 0;

  if (delta < 0) {
    code = 8;
    delta = -delta;
  }

  int32_t difference = step >> 3;
  if (delta >= step) {
    code |= 4;
    delta -= step;
    difference += step;
  }
  if (delta >= (step >> 1)) {
    code |= 2;
    delta -= (step >> 1);
    difference += (step >> 1);
  }
  if (delta >= (step >> 2)) {
    code |= 1;
    difference += (step >> 2);
  }

  int32_t predictor = state.predictor;
  if ((code & 8U) != 0U) {
    predictor -= difference;
  } else {
    predictor += difference;
  }

  state.predictor = clampPredictor(predictor);
  state.index = clampIndex(static_cast<int32_t>(state.index) + kIndexTable[code & 0x0F]);
  return static_cast<uint8_t>(code & 0x0F);
}

int16_t decodeNibble(uint8_t nibble, State& state) {
  nibble &= 0x0F;

  const int32_t step = kStepTable[state.index];
  int32_t difference = step >> 3;
  if ((nibble & 4U) != 0U) {
    difference += step;
  }
  if ((nibble & 2U) != 0U) {
    difference += (step >> 1);
  }
  if ((nibble & 1U) != 0U) {
    difference += (step >> 2);
  }

  int32_t predictor = state.predictor;
  if ((nibble & 8U) != 0U) {
    predictor -= difference;
  } else {
    predictor += difference;
  }

  state.predictor = clampPredictor(predictor);
  state.index = clampIndex(static_cast<int32_t>(state.index) + kIndexTable[nibble]);
  return state.predictor;
}

}  // namespace decaflash::brain::ima_adpcm
