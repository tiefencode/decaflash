#pragma once

#include "decaflash_types.h"

namespace decaflash::node {

static constexpr DefaultPreset kPrograms[] = {
  { "Beat Drive", EffectType::BeatPulse, 255, 1, 0, 1, 0, 0, 80 },
  { "Heavy Half", EffectType::BeatPulse, 255, 1, 1, 1, 0, 0, 150 },
  { "Double Tap 3Hz", EffectType::BarBurst, 255, 1, 1, 2, 333, 0, 90 },
  { "Quad Skip", EffectType::BarBurst, 255, 2, 1, 4, 260, -20, 70 },
  { "Riser 5x", EffectType::BarBurst, 255, 1, 1, 5, 360, -45, 60 },
};

static constexpr size_t kProgramCount = sizeof(kPrograms) / sizeof(kPrograms[0]);

}  // namespace decaflash::node
