#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::matrix {

void clearAllPixels();
void clearMatrix();
void clearBeatDotPixel();
void drawSceneNumber(size_t sceneIndex);
void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride);
void drawTextCharacter(uint8_t character, uint32_t color = 0xFFFFFF);

}  // namespace decaflash::brain::matrix
