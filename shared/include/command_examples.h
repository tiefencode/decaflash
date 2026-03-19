#pragma once

#include "decaflash_types.h"

namespace decaflash::examples {

static constexpr NodeCommand kFlashQuadSkip = {
  "Quad Skip",
  EffectType::BarBurst,
  255,
  2,
  1,
  4,
  260,
  -20,
  70,
};

}  // namespace decaflash::examples
