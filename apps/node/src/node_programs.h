#pragma once

#include "scene_programs.h"

namespace decaflash::node {

static constexpr size_t kSceneCount = decaflash::scenes::kSceneCount;

inline const char* sceneName(size_t sceneIndex) {
  return decaflash::scenes::sceneName(sceneIndex);
}

inline const FlashCommand& flashSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  return decaflash::scenes::flashSceneCommandFor(effect, sceneIndex);
}

inline const RgbCommand& rgbSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  return decaflash::scenes::rgbSceneCommandFor(effect, sceneIndex);
}

}  // namespace decaflash::node
