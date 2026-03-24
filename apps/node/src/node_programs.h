#pragma once

#include "scene_programs.h"

namespace decaflash::node {

static constexpr size_t kSceneCount = decaflash::scenes::kSceneCount;

inline const char* sceneName(size_t sceneIndex) {
  return decaflash::scenes::sceneName(sceneIndex);
}

inline FlashCommand flashSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  return decaflash::scenes::flashSceneCommandFor(effect, sceneIndex);
}

inline uint32_t flashVariationEpochFor(const FlashCommand& command, uint32_t currentBar) {
  return decaflash::scenes::flashVariationEpochFor(command, currentBar);
}

inline FlashRenderCommand flashRenderCommandFor(const FlashCommand& command, uint32_t currentBar) {
  return decaflash::scenes::flashRenderCommandFor(command, currentBar);
}

inline const RgbCommand& rgbSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  return decaflash::scenes::rgbSceneCommandFor(effect, sceneIndex);
}

}  // namespace decaflash::node
