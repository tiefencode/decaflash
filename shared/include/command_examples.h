#pragma once

#include "decaflash_types.h"

namespace decaflash::examples {

static constexpr NodeCommand kFlashCommands[] = {
  {
    "Beat Drive",
    EffectType::BeatPulse,
    255,
    1,
    0,
    1,
    0,
    0,
    65,
  },
  {
    "Heavy Half",
    EffectType::BeatPulse,
    255,
    1,
    1,
    1,
    0,
    0,
    120,
  },
  {
    "Double Tap 3Hz",
    EffectType::BarBurst,
    255,
    1,
    1,
    2,
    360,
    0,
    70,
  },
  {
    "Quad Skip",
    EffectType::BarBurst,
    255,
    2,
    1,
    4,
    290,
    -15,
    55,
  },
  {
    "Riser 5x",
    EffectType::BarBurst,
    255,
    1,
    1,
    5,
    390,
    -35,
    45,
  },
};

static constexpr size_t kFlashCommandCount =
  sizeof(kFlashCommands) / sizeof(kFlashCommands[0]);

static constexpr NodeCommand kFlashQuadSkip = kFlashCommands[3];

static constexpr NodeCommand kRgbCommands[] = {
  {
    "Neon Pulse",
    EffectType::BeatPulse,
    224,
    1,
    0,
    1,
    0,
    0,
    85,
  },
  {
    "Rose Downbeat",
    EffectType::BeatPulse,
    255,
    1,
    1,
    1,
    0,
    0,
    150,
  },
  {
    "Cyan Double",
    EffectType::BarBurst,
    232,
    1,
    1,
    2,
    220,
    0,
    80,
  },
  {
    "Chase Quad",
    EffectType::BarBurst,
    240,
    2,
    1,
    4,
    190,
    -10,
    70,
  },
  {
    "Sunrise 5x",
    EffectType::BarBurst,
    255,
    1,
    1,
    5,
    240,
    -20,
    55,
  },
};

static constexpr size_t kRgbCommandCount =
  sizeof(kRgbCommands) / sizeof(kRgbCommands[0]);

}  // namespace decaflash::examples
