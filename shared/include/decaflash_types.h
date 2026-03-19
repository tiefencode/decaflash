#pragma once

#include <Arduino.h>

namespace decaflash {

static constexpr size_t kCommandNameLength = 24;

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
  BeatPulse = 1,
  BarBurst = 2,
};

struct NodeCommand {
  char name[kCommandNameLength];
  EffectType effect;
  uint8_t intensity;
  uint8_t triggerEveryBars;
  uint8_t triggerBeat;
  uint8_t burstCount;
  uint16_t burstIntervalMs;
  int16_t burstIntervalStepMs;
  uint16_t flashDurationMs;
};

using DefaultPreset = NodeCommand;

struct NodeIdentity {
  DeviceType deviceType;
  NodeKind nodeKind;
};

}  // namespace decaflash
