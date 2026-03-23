#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::matrix {

void clearMatrix();
void clearBeatDotPixel();
void drawSceneNumber(size_t sceneIndex);
void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride);

}  // namespace decaflash::brain::matrix
