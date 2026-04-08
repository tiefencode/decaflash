#include "mulaw.h"

namespace decaflash::brain::mulaw {

namespace {

static constexpr int16_t kBias = 0x84;
static constexpr int16_t kClip = 32635;

uint8_t exponentForMagnitude(uint16_t magnitude) {
  uint8_t exponent = 7;
  for (uint16_t mask = 0x4000; (magnitude & mask) == 0 && exponent > 0; mask >>= 1) {
    exponent--;
  }
  return exponent;
}

}  // namespace

uint8_t encodeSample(int16_t sample) {
  uint8_t sign = 0;
  int16_t magnitude = sample;

  if (magnitude < 0) {
    sign = 0x80;
    magnitude = static_cast<int16_t>(-magnitude);
    if (magnitude < 0) {
      magnitude = kClip;
    }
  }

  if (magnitude > kClip) {
    magnitude = kClip;
  }

  magnitude = static_cast<int16_t>(magnitude + kBias);
  const uint8_t exponent = exponentForMagnitude(static_cast<uint16_t>(magnitude));
  const uint8_t mantissa = static_cast<uint8_t>((magnitude >> (exponent + 3U)) & 0x0F);
  return static_cast<uint8_t>(~(sign | (exponent << 4U) | mantissa));
}

int16_t decodeByte(uint8_t value) {
  value = static_cast<uint8_t>(~value);

  const int16_t sign = static_cast<int16_t>(value & 0x80U);
  const int16_t exponent = static_cast<int16_t>((value >> 4U) & 0x07U);
  const int16_t mantissa = static_cast<int16_t>(value & 0x0FU);

  int16_t magnitude = static_cast<int16_t>(((mantissa << 3U) + kBias) << exponent);
  magnitude = static_cast<int16_t>(magnitude - kBias);

  return (sign != 0) ? static_cast<int16_t>(-magnitude) : magnitude;
}

}  // namespace decaflash::brain::mulaw
