#pragma once

#include "decaflash_types.h"

namespace decaflash::scenes {

struct SceneDefinition {
  const char* name;
  FlashCommand flashPulse;
  FlashCommand flashAccent;
  RgbCommand wash;
  RgbCommand pulse;
  RgbCommand accent;
  RgbCommand flicker;
};

namespace detail {

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

inline void copyCommandName(char* destination, const char* source) {
  size_t index = 0;
  while (source != nullptr && source[index] != '\0' && index + 1U < kCommandNameLength) {
    destination[index] = source[index];
    ++index;
  }

  while (index < kCommandNameLength) {
    destination[index++] = '\0';
  }
}

inline FlashCommand flashCommand(
  const char* name,
  FlashPattern pattern,
  FlashLength length,
  uint8_t everyBars,
  uint8_t beat
) {
  FlashCommand command = {};
  copyCommandName(command.name, name);
  command.pattern = pattern;
  command.length = length;
  command.triggerEveryBars = everyBars;
  command.triggerBeat = beat;
  return command;
}

inline RgbCommand rgbCommand(
  const char* name,
  RgbPattern pattern,
  RgbColor primary,
  RgbColor secondary,
  uint8_t floorLevel,
  uint8_t baseLevel,
  uint8_t peakLevel,
  uint8_t everyBars,
  uint8_t beat,
  uint16_t cycleMs,
  uint16_t accentDurationMs
) {
  RgbCommand command = {};
  copyCommandName(command.name, name);
  command.pattern = pattern;
  command.primaryR = primary.r;
  command.primaryG = primary.g;
  command.primaryB = primary.b;
  command.secondaryR = secondary.r;
  command.secondaryG = secondary.g;
  command.secondaryB = secondary.b;
  command.floorLevel = floorLevel;
  command.baseLevel = baseLevel;
  command.peakLevel = peakLevel;
  command.triggerEveryBars = everyBars;
  command.triggerBeat = beat;
  command.cycleMs = cycleMs;
  command.accentDurationMs = accentDurationMs;
  return command;
}

inline SceneDefinition makeScene1() {
  constexpr RgbColor kDeepBlue = {0, 24, 110};
  constexpr RgbColor kIceBlue = {92, 214, 255};
  constexpr RgbColor kPulseBlue = {0, 78, 255};
  constexpr RgbColor kHeartDarkRed = {84, 0, 6};
  constexpr RgbColor kHeartRed = {255, 18, 0};
  constexpr uint8_t kWashFloor = 0;
  constexpr uint8_t kWashBase = 18;
  constexpr uint8_t kWashPeak = 152;
  constexpr uint16_t kWashTravelMs = 380;
  constexpr uint16_t kWashWhiteHoldMs = 70;

  // Szene 1 startet mit einer langsamen 4-Takte-Welle:
  // dunkel -> blau -> hellblau -> kurzes weiss -> wieder aus.
  return {
    "szene 1",

    flashCommand("Scene 1 Pulse", FlashPattern::PerBeat, FlashLength::Long, 4, 1),
    flashCommand("Scene 1 Accent", FlashPattern::Off, FlashLength::Long, 1, 1),

    rgbCommand(
      "Scene 1 Wash",
      RgbPattern::BarWave,
      kDeepBlue,
      kIceBlue,
      kWashFloor,
      kWashBase,
      kWashPeak,
      4,
      1,
      kWashTravelMs,
      kWashWhiteHoldMs
    ),

    rgbCommand(
      "Scene 1 Pulse",
      RgbPattern::BeatPulse,
      kPulseBlue,
      kPulseBlue,
      0,
      86,
      116,
      1,
      1,
      380,
      100
    ),

    rgbCommand(
      "Scene 1 Accent",
      RgbPattern::Accent,
      kHeartDarkRed,
      kHeartRed,
      0,
      14,
      176,
      1,
      1,
      0,
      0
    ),

    rgbCommand(
      "Scene 1 Flicker",
      RgbPattern::RunnerFlicker,
      kDeepBlue,
      kPulseBlue,
      0,
      92,
      176,
      1,
      1,
      0,
      140
    ),
  };
}

}  // namespace detail

static const SceneDefinition kScenes[] = {
  detail::makeScene1(),
};

static constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

inline const SceneDefinition& sceneDefinitionFor(size_t sceneIndex) {
  return kScenes[sceneIndex % kSceneCount];
}

inline const char* sceneName(size_t sceneIndex) {
  return sceneDefinitionFor(sceneIndex).name;
}

inline const FlashCommand& flashSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  const auto& scene = sceneDefinitionFor(sceneIndex);

  switch (effect) {
    case NodeEffect::Accent:
      return scene.flashAccent;

    case NodeEffect::Pulse:
    default:
      return scene.flashPulse;
  }
}

inline const RgbCommand& rgbSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  const auto& scene = sceneDefinitionFor(sceneIndex);

  switch (effect) {
    case NodeEffect::Wash:
      return scene.wash;

    case NodeEffect::Accent:
      return scene.accent;

    case NodeEffect::Flicker:
      return scene.flicker;

    case NodeEffect::Pulse:
    default:
      return scene.pulse;
  }
}

static const FlashCommand& kFlashReference = kScenes[0].flashPulse;
static const RgbCommand& kPulseReference = kScenes[0].pulse;

}  // namespace decaflash::scenes
