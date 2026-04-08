#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::matrix {

void clearAllPixels();
void clearMatrix();
void clearBeatDotPixel();
void drawSceneNumber(size_t sceneIndex);
void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride);
size_t measureTextColumns(const uint8_t* text, size_t length);
void drawTextCharacter(uint8_t character, uint32_t color = 0xFFFFFF);
void drawTextWindow(const uint8_t* text,
                    size_t length,
                    int16_t startColumn,
                    uint32_t color = 0xFFFFFF);
void drawWifiIcon(uint32_t color = 0xFFFFFF);
void drawWifiConnectedIcon(uint32_t color = 0x00FF00);
void drawWifiConnectingIcon(uint32_t now, uint32_t color = 0xFFD000);
void drawWifiFailedIcon(uint32_t color = 0xFF0000);
void drawSpeakerIcon(uint32_t color = 0xFFFFFF);
void drawSpeakerMutedIcon(uint32_t color = 0xFFFFFF);
void drawAiWaveAnimation(uint32_t elapsedMs,
                         uint32_t durationMs,
                         bool bottomToTop,
                         uint32_t color);

}  // namespace decaflash::brain::matrix
