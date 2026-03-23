#include "matrix_ui.h"

#include <Arduino.h>
#include <M5Atom.h>

namespace decaflash::brain::matrix {

namespace {

static constexpr uint8_t kMatrixPixelCount = 25;
static constexpr uint8_t kBeatDotPixelIndex = 4;
static constexpr uint8_t kDigitMasks[][5] = {
  {0b00100, 0b01100, 0b00100, 0b00100, 0b01110},  // 1
  {0b01110, 0b00010, 0b01110, 0b01000, 0b01110},  // 2
  {0b01110, 0b00010, 0b00110, 0b00010, 0b01110},  // 3
  {0b01010, 0b01010, 0b01110, 0b00010, 0b00010},  // 4
  {0b01110, 0b01000, 0b01110, 0b00010, 0b01110},  // 5
};

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

size_t sceneSlotCount() {
  return sizeof(kDigitMasks) / sizeof(kDigitMasks[0]);
}

uint32_t scenePixelColor(size_t sceneIndex, uint8_t x, uint8_t y) {
  if (sceneIndex >= sceneSlotCount() || x > 4 || y > 4) {
    return 0x000000;
  }

  const uint8_t* rows = kDigitMasks[sceneIndex];
  const bool on = (rows[y] >> (4 - x)) & 0x01;
  return on ? color(255, 255, 255) : 0x000000;
}

uint32_t beatDotColor(uint8_t beatDotBeat, uint32_t beatDotColorOverride) {
  if (beatDotColorOverride != 0) {
    return beatDotColorOverride;
  }

  return (beatDotBeat == 1) ? color(255, 210, 0) : color(255, 255, 255);
}

}  // namespace

void clearMatrix() {
  for (uint8_t i = 0; i < kMatrixPixelCount; ++i) {
    if (i == kBeatDotPixelIndex) {
      continue;
    }

    M5.dis.drawpix(i, 0x000000);
  }
}

void clearBeatDotPixel() {
  M5.dis.drawpix(kBeatDotPixelIndex, 0x000000);
}

void drawSceneNumber(size_t sceneIndex) {
  clearMatrix();

  if (sceneIndex >= sceneSlotCount()) {
    return;
  }

  const uint8_t* rows = kDigitMasks[sceneIndex];
  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      if ((y * 5U + x) == kBeatDotPixelIndex) {
        continue;
      }

      M5.dis.drawpix(y * 5 + x, scenePixelColor(sceneIndex, x, y));
    }
  }
}

void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride) {
  M5.dis.drawpix(kBeatDotPixelIndex, beatDotColor(beatDotBeat, beatDotColorOverride));
}

}  // namespace decaflash::brain::matrix
