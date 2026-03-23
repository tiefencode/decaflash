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
  UvLed = 2,
};

enum class NodeEffect : uint8_t {
  None = 0,

  Wash = 1,
  Pulse = 2,
  Accent = 3,
  Flicker = 4,
};

enum class FlashPattern : uint8_t {
  Off = 0,
  PerBeat = 1,
  Burst = 2,
};

enum class FlashLength : uint8_t {
  Short = 0,
  Long = 1,
};

enum class FlashCadence : uint8_t {
  Hz2 = 0,
  Hz3 = 1,
  TightenSoft = 2,
  TightenFast = 3,
};

enum class RgbPattern : uint8_t {
  Off = 0,
  Breathe = 1,
  BeatPulse = 2,
  Accent = 3,
  RunnerFlicker = 4,
};

struct FlashCommand {
  char name[kCommandNameLength];
  FlashPattern pattern;
  FlashLength length;
  uint8_t triggerEveryBars;
  uint8_t triggerBeat;
  uint8_t burstCount;
  FlashCadence cadence;
  uint8_t reserved0;
};

struct RgbCommand {
  char name[kCommandNameLength];
  RgbPattern pattern;
  uint8_t primaryR;
  uint8_t primaryG;
  uint8_t primaryB;
  uint8_t secondaryR;
  uint8_t secondaryG;
  uint8_t secondaryB;
  uint8_t floorLevel;
  uint8_t baseLevel;
  uint8_t peakLevel;
  uint8_t triggerEveryBars;
  uint8_t triggerBeat;
  uint16_t cycleMs;
  uint16_t accentDurationMs;
};

struct NodeIdentity {
  DeviceType deviceType;
  NodeKind nodeKind;
  NodeEffect nodeEffect;
  uint8_t reserved0;
};

}  // namespace decaflash
