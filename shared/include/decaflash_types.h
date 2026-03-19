#pragma once

#include <Arduino.h>

namespace decaflash {

enum class DeviceType : uint8_t {
  Brain = 0,
  Node = 1,
};

enum class NodeKind : uint8_t {
  Flashlight = 0,
  RgbStrip = 1,
};

enum class EffectType : uint8_t {
  Off = 0,
  On = 1,
  PulseSlow = 2,
  Strobe = 3,
};

struct DefaultPreset {
  const char* name;
  EffectType effect;
  uint8_t intensity;
  uint16_t intervalMs;
};

}  // namespace decaflash
