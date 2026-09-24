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
  Pulse = 1,
  PulseRow = 2,
};

enum class FlashCommandMode : uint8_t {
  Off = 0,
  VariationProfile = 1,
};

enum class RgbPattern : uint8_t {
  Off = 0,
  Wave = 1,
  Pulse = 2,
  PulseRow = 3,
  Runner = 4,
  Heartbeat = 5,
  RiserPulse = 6,
};

enum class RunnerMotion : uint8_t {
  Bounce = 0,
  Loop = 1,
};

enum class RunnerPresentation : uint8_t {
  Parallel = 0,
  Sequence = 1,
};

static constexpr uint8_t kMaxRunnerBands = 4;

// A runner is made from a few independently positioned colour bands. Width,
// position and edge are relative to the whole strip, so scene code does not
// need to know how many LEDs a particular node has.
struct RgbRunnerBand {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t widthPercent;
  uint8_t phasePercent;
  uint8_t edgePercent;
};

struct FlashCommand {
  char name[kCommandNameLength];
  FlashCommandMode mode;
  uint8_t variationWindowBars;
  uint16_t profileSeed;
  uint8_t pulseWeight;
  uint8_t slowPulseWeight;
  uint8_t doublePulseWeight;
  uint8_t quadPulseWeight;
  uint8_t riserWeight;
};

struct FlashRenderCommand {
  char name[kCommandNameLength];
  FlashPattern pattern;
  uint8_t triggerEveryBars;
  uint8_t triggerBeat;
  uint8_t burstCount;
  uint16_t burstIntervalMs;
  int16_t burstIntervalStepMs;
  uint16_t flashDurationMs;
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
  uint16_t startOffsetMs;
  uint16_t durationMs;
  uint16_t peakHoldMs;
  uint16_t fadeOutMs;
  uint8_t waveCycleBeats;
  uint8_t pulseCount;
  uint16_t pulseGapMs;
  uint8_t subsequentPulseLevel;
  RunnerMotion runnerMotion;
  RunnerPresentation runnerPresentation;
  uint8_t runnerBandCount;
  RgbRunnerBand runnerBands[kMaxRunnerBands];
};

struct NodeIdentity {
  DeviceType deviceType;
  NodeKind nodeKind;
  NodeEffect nodeEffect;
};

}  // namespace decaflash
