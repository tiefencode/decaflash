#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::matrix {

void clearAllPixels();
void clearMatrix();
void drawSolidColor(uint32_t colorValue);
void clearStatusPixel();
void clearBeatDotPixel();
void drawSceneNumber(size_t sceneIndex);
void drawStatusPixelOverlay(uint32_t colorValue);
void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride);
size_t measureTextColumns(const uint8_t* text, size_t length);
void drawTextCharacter(uint8_t character, uint32_t color = 0xFFFFFF);
void drawTextWindow(const uint8_t* text,
                    size_t length,
                    int16_t startColumn,
                    uint32_t color = 0xFFFFFF);
void drawSpeakerIcon(uint32_t color = 0xFFFFFF);
void drawSpeakerMutedIcon(uint32_t color = 0xFFFFFF);
void drawAiWaveAnimation(uint32_t elapsedMs,
                         uint32_t durationMs,
                         bool bottomToTop,
                         uint32_t color);

}  // namespace decaflash::brain::matrix
