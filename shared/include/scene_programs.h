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

static constexpr SceneDefinition kScenes[] = {
  {
    "ruhig",
    {"Quiet Pulse", FlashPattern::PerBeat, FlashLength::Long, 16, 1, 1, FlashCadence::Hz2, 0},
    {"Quiet Accent", FlashPattern::Off, FlashLength::Long, 8, 4, 0, FlashCadence::Hz2, 0},
    {"Quiet Wash", RgbPattern::Breathe, 0, 12, 64, 48, 0, 88, 5, 12, 30, 1, 0, 8800, 480},
    {"Quiet Pulse", RgbPattern::BeatPulse, 0, 20, 96, 84, 0, 130, 6, 24, 72, 4, 1, 5400, 420},
    {"Quiet Accent", RgbPattern::Accent, 10, 0, 0, 150, 0, 0, 0, 10, 96, 8, 4, 4200, 240},
    {"Quiet Flicker", RgbPattern::RunnerFlicker, 0, 20, 80, 110, 0, 120, 4, 20, 68, 8, 4, 2800, 180},
  },
  {
    "dynamisch",
    {"Drive Pulse", FlashPattern::Burst, FlashLength::Short, 1, 1, 2, FlashCadence::Hz3, 0},
    {"Drive Accent", FlashPattern::Burst, FlashLength::Short, 2, 3, 3, FlashCadence::Hz3, 0},
    {"Drive Wash", RgbPattern::Breathe, 0, 40, 120, 72, 0, 120, 14, 34, 88, 1, 0, 2600, 240},
    {"Drive Pulse", RgbPattern::BeatPulse, 0, 70, 180, 180, 0, 190, 16, 88, 220, 1, 0, 1100, 210},
    {"Drive Accent", RgbPattern::Accent, 14, 0, 0, 255, 12, 0, 0, 22, 255, 2, 2, 1000, 170},
    {"Drive Flicker", RgbPattern::RunnerFlicker, 0, 96, 210, 255, 0, 70, 12, 92, 230, 1, 1, 520, 110},
  },
  {
    "balanciert",
    {"Balanced Pulse", FlashPattern::PerBeat, FlashLength::Short, 2, 1, 1, FlashCadence::Hz3, 0},
    {"Balanced Accent", FlashPattern::PerBeat, FlashLength::Long, 4, 3, 1, FlashCadence::Hz3, 0},
    {"Balanced Wash", RgbPattern::Breathe, 0, 22, 92, 36, 0, 90, 8, 20, 44, 1, 0, 5200, 300},
    {"Balanced Pulse", RgbPattern::BeatPulse, 0, 50, 150, 120, 0, 160, 10, 54, 160, 2, 1, 1900, 220},
    {"Balanced Accent", RgbPattern::Accent, 12, 0, 0, 220, 0, 0, 0, 14, 180, 4, 3, 1700, 170},
    {"Balanced Flicker", RgbPattern::RunnerFlicker, 0, 56, 150, 180, 0, 120, 8, 60, 180, 2, 2, 980, 140},
  },
  {
    "melancholisch kalt",
    {"Cold Pulse", FlashPattern::PerBeat, FlashLength::Long, 8, 1, 1, FlashCadence::Hz2, 0},
    {"Cold Accent", FlashPattern::PerBeat, FlashLength::Short, 8, 4, 1, FlashCadence::Hz2, 0},
    {"Cold Wash", RgbPattern::Breathe, 0, 12, 54, 0, 40, 92, 2, 8, 20, 1, 0, 9800, 520},
    {"Cold Pulse", RgbPattern::BeatPulse, 0, 20, 88, 0, 58, 120, 4, 18, 58, 4, 3, 3400, 260},
    {"Cold Accent", RgbPattern::Accent, 6, 0, 0, 120, 0, 0, 0, 8, 84, 8, 4, 2400, 190},
    {"Cold Flicker", RgbPattern::RunnerFlicker, 0, 28, 96, 120, 0, 18, 2, 26, 92, 4, 4, 1650, 125},
  },
  {
    "eskalation",
    {"Rise Pulse", FlashPattern::Burst, FlashLength::Short, 1, 1, 5, FlashCadence::TightenFast, 0},
    {"Rise Accent", FlashPattern::Burst, FlashLength::Short, 1, 3, 4, FlashCadence::TightenSoft, 0},
    {"Rise Wash", RgbPattern::RunnerFlicker, 0, 80, 190, 140, 0, 170, 20, 72, 170, 1, 1, 720, 120},
    {"Rise Pulse", RgbPattern::BeatPulse, 0, 110, 255, 255, 0, 220, 22, 120, 255, 1, 0, 680, 150},
    {"Rise Accent", RgbPattern::Accent, 20, 0, 0, 255, 18, 0, 0, 28, 255, 1, 2, 920, 130},
    {"Rise Flicker", RgbPattern::RunnerFlicker, 0, 120, 255, 255, 0, 0, 14, 120, 255, 1, 1, 300, 80},
  },
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

static constexpr FlashCommand kFlashReference = kScenes[0].flashPulse;
static constexpr RgbCommand kPulseReference = kScenes[0].pulse;

}  // namespace decaflash::scenes
