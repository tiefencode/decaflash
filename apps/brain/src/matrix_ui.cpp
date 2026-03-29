#include "matrix_ui.h"

#include <Arduino.h>
#include <M5Atom.h>

#include <cctype>

namespace decaflash::brain::matrix {

namespace {

static constexpr uint8_t kMatrixPixelCount = 25;
static constexpr uint8_t kBeatDotPixelIndex = 4;
struct Glyph {
  char character;
  uint8_t rows[5];
};

static constexpr uint8_t kDigitMasks[][5] = {
  {0b00100, 0b01100, 0b00100, 0b00100, 0b01110},  // 1
  {0b01110, 0b00010, 0b01110, 0b01000, 0b01110},  // 2
  {0b01110, 0b00010, 0b00110, 0b00010, 0b01110},  // 3
  {0b01010, 0b01010, 0b01110, 0b00010, 0b00010},  // 4
  {0b01110, 0b01000, 0b01110, 0b00010, 0b01110},  // 5
};

static constexpr Glyph kTextGlyphs[] = {
  {' ', {0b00000, 0b00000, 0b00000, 0b00000, 0b00000}},
  {'!', {0b00100, 0b00100, 0b00100, 0b00000, 0b00100}},
  {'-', {0b00000, 0b00000, 0b11111, 0b00000, 0b00000}},
  {',', {0b00000, 0b00000, 0b00000, 0b00110, 0b00100}},
  {'.', {0b00000, 0b00000, 0b00000, 0b00110, 0b00110}},
  {'?', {0b01110, 0b00001, 0b00110, 0b00000, 0b00100}},
  {'0', {0b01110, 0b10011, 0b10101, 0b11001, 0b01110}},
  {'1', {0b00100, 0b01100, 0b00100, 0b00100, 0b01110}},
  {'2', {0b01110, 0b00001, 0b00110, 0b01000, 0b11111}},
  {'3', {0b11110, 0b00001, 0b00110, 0b00001, 0b11110}},
  {'4', {0b10010, 0b10010, 0b11111, 0b00010, 0b00010}},
  {'5', {0b11111, 0b10000, 0b11110, 0b00001, 0b11110}},
  {'6', {0b01110, 0b10000, 0b11110, 0b10001, 0b01110}},
  {'7', {0b11111, 0b00010, 0b00100, 0b01000, 0b01000}},
  {'8', {0b01110, 0b10001, 0b01110, 0b10001, 0b01110}},
  {'9', {0b01110, 0b10001, 0b01111, 0b00001, 0b01110}},
  {'A', {0b01110, 0b10001, 0b11111, 0b10001, 0b10001}},
  {'B', {0b11110, 0b10001, 0b11110, 0b10001, 0b11110}},
  {'C', {0b01111, 0b10000, 0b10000, 0b10000, 0b01111}},
  {'D', {0b11110, 0b10001, 0b10001, 0b10001, 0b11110}},
  {'E', {0b11111, 0b10000, 0b11110, 0b10000, 0b11111}},
  {'F', {0b11111, 0b10000, 0b11110, 0b10000, 0b10000}},
  {'G', {0b01111, 0b10000, 0b10111, 0b10001, 0b01110}},
  {'H', {0b10001, 0b10001, 0b11111, 0b10001, 0b10001}},
  {'I', {0b11111, 0b00100, 0b00100, 0b00100, 0b11111}},
  {'J', {0b00001, 0b00001, 0b00001, 0b10001, 0b01110}},
  {'K', {0b10001, 0b10010, 0b11100, 0b10010, 0b10001}},
  {'L', {0b10000, 0b10000, 0b10000, 0b10000, 0b11111}},
  {'M', {0b10001, 0b11011, 0b10101, 0b10001, 0b10001}},
  {'N', {0b10001, 0b11001, 0b10101, 0b10011, 0b10001}},
  {'O', {0b01110, 0b10001, 0b10001, 0b10001, 0b01110}},
  {'P', {0b11110, 0b10001, 0b11110, 0b10000, 0b10000}},
  {'Q', {0b01110, 0b10001, 0b10001, 0b10011, 0b01111}},
  {'R', {0b11110, 0b10001, 0b11110, 0b10010, 0b10001}},
  {'S', {0b01111, 0b10000, 0b01110, 0b00001, 0b11110}},
  {'T', {0b11111, 0b00100, 0b00100, 0b00100, 0b00100}},
  {'U', {0b10001, 0b10001, 0b10001, 0b10001, 0b01110}},
  {'V', {0b10001, 0b10001, 0b10001, 0b01010, 0b00100}},
  {'W', {0b10001, 0b10001, 0b10101, 0b11011, 0b10001}},
  {'X', {0b10001, 0b01010, 0b00100, 0b01010, 0b10001}},
  {'Y', {0b10001, 0b01010, 0b00100, 0b00100, 0b00100}},
  {'Z', {0b11111, 0b00010, 0b00100, 0b01000, 0b11111}},
};

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

void fillAllPixels(uint32_t pixelColor) {
  for (uint8_t i = 0; i < kMatrixPixelCount; ++i) {
    M5.dis.drawpix(i, pixelColor);
  }
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

const Glyph* glyphForCharacter(char character) {
  const char uppercaseCharacter = static_cast<char>(
    std::toupper(static_cast<unsigned char>(character))
  );

  for (const auto& glyph : kTextGlyphs) {
    if (glyph.character == uppercaseCharacter) {
      return &glyph;
    }
  }

  for (const auto& glyph : kTextGlyphs) {
    if (glyph.character == '?') {
      return &glyph;
    }
  }

  return nullptr;
}

}  // namespace

void clearAllPixels() {
  fillAllPixels(0x000000);
}

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

void drawTextCharacter(char character, uint32_t colorValue) {
  clearAllPixels();

  const Glyph* glyph = glyphForCharacter(character);
  if (glyph == nullptr) {
    return;
  }

  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      const bool on = (glyph->rows[y] >> (4 - x)) & 0x01;
      M5.dis.drawpix(y * 5 + x, on ? colorValue : 0x000000);
    }
  }
}

}  // namespace decaflash::brain::matrix
