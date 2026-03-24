#pragma once

#include "decaflash_types.h"

namespace decaflash::scenes {

struct SceneDefinition {
  const char* name;
  FlashCommand flash;
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

inline FlashCommand flashProfileCommand(
  const char* name,
  uint8_t variationWindowBars,
  uint16_t profileSeed,
  uint8_t driveWeight,
  uint8_t heavyWeight,
  uint8_t doubleWeight,
  uint8_t quadWeight,
  uint8_t riserWeight
) {
  FlashCommand command = {};
  copyCommandName(command.name, name);
  command.mode = FlashCommandMode::VariationProfile;
  command.variationWindowBars = variationWindowBars;
  command.profileSeed = profileSeed;
  command.driveWeight = driveWeight;
  command.heavyWeight = heavyWeight;
  command.doubleWeight = doubleWeight;
  command.quadWeight = quadWeight;
  command.riserWeight = riserWeight;
  return command;
}

inline FlashRenderCommand flashRenderCommand(
  const char* name,
  FlashPattern pattern,
  uint8_t everyBars,
  uint8_t beat,
  uint8_t burstCount,
  uint16_t burstIntervalMs,
  int16_t burstIntervalStepMs,
  uint16_t flashDurationMs
) {
  FlashRenderCommand command = {};
  copyCommandName(command.name, name);
  command.pattern = pattern;
  command.triggerEveryBars = everyBars;
  command.triggerBeat = beat;
  command.burstCount = burstCount;
  command.burstIntervalMs = burstIntervalMs;
  command.burstIntervalStepMs = burstIntervalStepMs;
  command.flashDurationMs = flashDurationMs;
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

inline uint32_t mixSeed(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dUL;
  value ^= value >> 15;
  value *= 0x846ca68bUL;
  value ^= value >> 16;
  return value;
}

inline uint32_t flashSeed(const FlashCommand& command, uint32_t variationEpoch, uint32_t salt) {
  return mixSeed(
    static_cast<uint32_t>(command.profileSeed) ^
    static_cast<uint32_t>((variationEpoch + 1U) * 0x9E3779B9UL) ^
    salt
  );
}

inline uint32_t rangeUint32(uint32_t seed, uint32_t minimum, uint32_t maximum) {
  if (minimum >= maximum) {
    return minimum;
  }

  return minimum + (seed % (maximum - minimum + 1U));
}

inline int32_t rangeInt32(uint32_t seed, int32_t minimum, int32_t maximum) {
  if (minimum >= maximum) {
    return minimum;
  }

  const uint32_t span = static_cast<uint32_t>(maximum - minimum + 1);
  return minimum + static_cast<int32_t>(seed % span);
}

inline uint32_t variationEpochFor(const FlashCommand& command, uint32_t currentBar) {
  const uint32_t safeWindowBars =
    (command.variationWindowBars == 0U) ? 1U : static_cast<uint32_t>(command.variationWindowBars);
  const uint32_t zeroBasedBar = (currentBar == 0U) ? 0U : (currentBar - 1U);
  return zeroBasedBar / safeWindowBars;
}

enum class FlashMotif : uint8_t {
  Drive = 0,
  Heavy = 1,
  Double = 2,
  Quad = 3,
  Riser = 4,
};

inline FlashMotif pickFlashMotif(const FlashCommand& command, uint32_t variationEpoch) {
  const uint16_t totalWeight =
    static_cast<uint16_t>(command.driveWeight) +
    static_cast<uint16_t>(command.heavyWeight) +
    static_cast<uint16_t>(command.doubleWeight) +
    static_cast<uint16_t>(command.quadWeight) +
    static_cast<uint16_t>(command.riserWeight);

  if (totalWeight == 0U) {
    return FlashMotif::Drive;
  }

  const uint32_t roll = flashSeed(command, variationEpoch, 0xA341316CUL) % totalWeight;
  uint16_t cursor = command.driveWeight;
  if (roll < cursor) {
    return FlashMotif::Drive;
  }

  cursor = static_cast<uint16_t>(cursor + command.heavyWeight);
  if (roll < cursor) {
    return FlashMotif::Heavy;
  }

  cursor = static_cast<uint16_t>(cursor + command.doubleWeight);
  if (roll < cursor) {
    return FlashMotif::Double;
  }

  cursor = static_cast<uint16_t>(cursor + command.quadWeight);
  if (roll < cursor) {
    return FlashMotif::Quad;
  }

  return FlashMotif::Riser;
}

inline FlashRenderCommand flashRenderCommandForMotif(
  FlashMotif motif,
  const FlashCommand& command,
  uint32_t variationEpoch
) {
  const uint32_t seed0 = flashSeed(command, variationEpoch, 0x1234567UL);
  const uint32_t seed1 = flashSeed(command, variationEpoch, 0x2345678UL);
  const uint32_t seed2 = flashSeed(command, variationEpoch, 0x3456789UL);
  const uint32_t seed3 = flashSeed(command, variationEpoch, 0x456789AUL);

  switch (motif) {
    case FlashMotif::Heavy:
      return flashRenderCommand(
        "Heavy Half",
        FlashPattern::BeatPulse,
        (rangeUint32(seed1, 0, 1) == 0U) ? 4U : 8U,
        1,
        1,
        0,
        0,
        static_cast<uint16_t>(rangeUint32(seed0, 105, 135))
      );

    case FlashMotif::Double:
      return flashRenderCommand(
        "Double Tap 3Hz",
        FlashPattern::BarBurst,
        (rangeUint32(seed2, 0, 1) == 0U) ? 4U : 8U,
        1,
        2,
        static_cast<uint16_t>(rangeUint32(seed0, 330, 380)),
        0,
        static_cast<uint16_t>(rangeUint32(seed1, 60, 85))
      );

    case FlashMotif::Quad:
      return flashRenderCommand(
        "Quad Skip",
        FlashPattern::BarBurst,
        (rangeUint32(seed3, 0, 1) == 0U) ? 8U : 16U,
        1,
        4,
        static_cast<uint16_t>(rangeUint32(seed0, 260, 310)),
        static_cast<int16_t>(rangeInt32(seed1, -20, -10)),
        static_cast<uint16_t>(rangeUint32(seed2, 45, 60))
      );

    case FlashMotif::Riser:
      return flashRenderCommand(
        "Riser 5x",
        FlashPattern::BarBurst,
        (rangeUint32(seed0, 0, 2) == 0U) ? 16U : 32U,
        1,
        5,
        static_cast<uint16_t>(rangeUint32(seed1, 340, 410)),
        static_cast<int16_t>(rangeInt32(seed2, -40, -25)),
        static_cast<uint16_t>(rangeUint32(seed3, 40, 55))
      );

    case FlashMotif::Drive:
    default:
      return flashRenderCommand(
        "Beat Drive",
        FlashPattern::BeatPulse,
        (rangeUint32(seed1, 0, 2) == 0U) ? 4U : 2U,
        (rangeUint32(seed2, 0, 4) == 0U) ? 3U : 1U,
        1,
        0,
        0,
        static_cast<uint16_t>(rangeUint32(seed0, 60, 85))
      );
  }
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

  // Eine gemeinsame Szene fuer die ganze Node-Welt.
  // Flash lebt innerhalb dieser Szene ueber lokale Motif-Variationen.
  return {
    "szene 1",

    flashProfileCommand("Scene 1 Flash", 32, 17, 44, 32, 14, 7, 3),

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

inline uint32_t flashVariationEpochFor(const FlashCommand& command, uint32_t currentBar) {
  return detail::variationEpochFor(command, currentBar);
}

inline FlashCommand flashSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  (void)effect;
  return sceneDefinitionFor(sceneIndex).flash;
}

inline FlashRenderCommand flashRenderCommandFor(const FlashCommand& command, uint32_t currentBar) {
  if (command.mode == FlashCommandMode::Off) {
    return detail::flashRenderCommand("Off", FlashPattern::Off, 1, 1, 0, 0, 0, 0);
  }

  const uint32_t variationEpoch = detail::variationEpochFor(command, currentBar);
  const detail::FlashMotif motif = detail::pickFlashMotif(command, variationEpoch);
  return detail::flashRenderCommandForMotif(motif, command, variationEpoch);
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

static const FlashCommand kFlashReference = kScenes[0].flash;
static const RgbCommand& kPulseReference = kScenes[0].pulse;

}  // namespace decaflash::scenes
