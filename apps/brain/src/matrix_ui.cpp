#include "matrix_ui.h"

#include <Arduino.h>
#include <M5Atom.h>

#include <cctype>

namespace decaflash::brain::matrix {

namespace {

static constexpr uint8_t kMatrixPixelCount = 25;
static constexpr uint8_t kStatusPixelIndex = 0;
static constexpr uint8_t kBeatDotPixelIndex = 4;
static constexpr uint8_t kMatrixWidth = 5;
static constexpr uint8_t kMatrixHeight = 5;
static constexpr size_t kDisplayBufferLength = 2 + (kMatrixPixelCount * 3);
static constexpr uint8_t kGlyphUmlautA = 0x80;
static constexpr uint8_t kGlyphUmlautO = 0x81;
static constexpr uint8_t kGlyphUmlautU = 0x82;
static constexpr uint8_t kGlyphSharpS = 0x83;

struct Glyph {
  uint8_t character;
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
  {'"', {0b01010, 0b01010, 0b00000, 0b00000, 0b00000}},
  {'#', {0b01010, 0b11111, 0b01010, 0b11111, 0b01010}},
  {'&', {0b01100, 0b10010, 0b01100, 0b10010, 0b01101}},
  {'\'', {0b00100, 0b00100, 0b00000, 0b00000, 0b00000}},
  {'(', {0b00010, 0b00100, 0b00100, 0b00100, 0b00010}},
  {')', {0b01000, 0b00100, 0b00100, 0b00100, 0b01000}},
  {'*', {0b00100, 0b10101, 0b01110, 0b10101, 0b00100}},
  {'+', {0b00000, 0b00100, 0b11111, 0b00100, 0b00000}},
  {'-', {0b00000, 0b00000, 0b11111, 0b00000, 0b00000}},
  {',', {0b00000, 0b00000, 0b00000, 0b00110, 0b00100}},
  {'.', {0b00000, 0b00000, 0b00000, 0b00110, 0b00110}},
  {'/', {0b00001, 0b00010, 0b00100, 0b01000, 0b10000}},
  {':', {0b00000, 0b00110, 0b00000, 0b00110, 0b00000}},
  {';', {0b00000, 0b00110, 0b00000, 0b00110, 0b00100}},
  {'=', {0b00000, 0b11111, 0b00000, 0b11111, 0b00000}},
  {'?', {0b01110, 0b00001, 0b00110, 0b00000, 0b00100}},
  {'_', {0b00000, 0b00000, 0b00000, 0b00000, 0b11111}},
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
  {kGlyphUmlautA, {0b01010, 0b01110, 0b10001, 0b11111, 0b10001}},
  {kGlyphUmlautO, {0b01010, 0b01110, 0b10001, 0b10001, 0b01110}},
  {kGlyphUmlautU, {0b01010, 0b10001, 0b10001, 0b10001, 0b01110}},
  {kGlyphSharpS, {0b01110, 0b10000, 0b01110, 0b10001, 0b11110}},
};

static constexpr uint8_t kSpeakerIconMask[5] = {
  0b00101,
  0b01101,
  0b11111,
  0b01101,
  0b00101,
};

static constexpr uint8_t kSpeakerMutedIconMask[5] = {
  0b10100,
  0b01110,
  0b11111,
  0b01110,
  0b00101,
};

uint8_t frameBuffer[kDisplayBufferLength] = {
  kMatrixWidth,
  kMatrixHeight,
};

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

uint32_t scaleColor(uint32_t colorValue, uint8_t scale) {
  const uint8_t red = static_cast<uint8_t>((((colorValue >> 16) & 0xFFU) * scale) / 255U);
  const uint8_t green = static_cast<uint8_t>((((colorValue >> 8) & 0xFFU) * scale) / 255U);
  const uint8_t blue = static_cast<uint8_t>(((colorValue & 0xFFU) * scale) / 255U);
  return color(red, green, blue);
}

void fillAllPixels(uint32_t pixelColor) {
  for (uint8_t i = 0; i < kMatrixPixelCount; ++i) {
    M5.dis.drawpix(i, pixelColor);
  }
}

void clearFrameBuffer() {
  frameBuffer[0] = kMatrixWidth;
  frameBuffer[1] = kMatrixHeight;
  memset(frameBuffer + 2, 0, kDisplayBufferLength - 2);
}

void setFramePixel(uint8_t pixelIndex, uint32_t colorValue) {
  if (pixelIndex >= kMatrixPixelCount) {
    return;
  }

  const size_t base = 2 + (static_cast<size_t>(pixelIndex) * 3U);
  frameBuffer[base + 0] = static_cast<uint8_t>((colorValue >> 8) & 0xFFU);
  frameBuffer[base + 1] = static_cast<uint8_t>((colorValue >> 16) & 0xFFU);
  frameBuffer[base + 2] = static_cast<uint8_t>(colorValue & 0xFFU);
}

void flushFrameBuffer() {
  M5.dis.displaybuff(frameBuffer);
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

uint8_t uppercaseAsciiCharacter(uint8_t character) {
  if (character >= 'a' && character <= 'z') {
    return static_cast<uint8_t>(character - 'a' + 'A');
  }

  return character;
}

const Glyph* glyphForCharacter(uint8_t character) {
  const uint8_t uppercaseCharacter = uppercaseAsciiCharacter(character);

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

uint8_t glyphColumnMask(const Glyph& glyph, uint8_t glyphColumn) {
  if (glyphColumn >= kMatrixWidth) {
    return 0;
  }

  uint8_t mask = 0;
  for (uint8_t row = 0; row < kMatrixHeight; ++row) {
    if (((glyph.rows[row] >> (4U - glyphColumn)) & 0x01U) != 0) {
      mask |= static_cast<uint8_t>(1U << row);
    }
  }

  return mask;
}

uint8_t glyphTrimmedStartColumn(const Glyph& glyph) {
  for (uint8_t column = 0; column < kMatrixWidth; ++column) {
    if (glyphColumnMask(glyph, column) != 0) {
      return column;
    }
  }

  return 0;
}

uint8_t glyphTrimmedEndColumn(const Glyph& glyph) {
  for (int8_t column = static_cast<int8_t>(kMatrixWidth - 1U); column >= 0; --column) {
    if (glyphColumnMask(glyph, static_cast<uint8_t>(column)) != 0) {
      return static_cast<uint8_t>(column);
    }
  }

  return 0;
}

uint8_t glyphColumnCount(const Glyph& glyph) {
  const uint8_t start = glyphTrimmedStartColumn(glyph);
  const uint8_t end = glyphTrimmedEndColumn(glyph);

  if (glyphColumnMask(glyph, start) == 0 && glyphColumnMask(glyph, end) == 0) {
    return 2;
  }

  return static_cast<uint8_t>((end - start) + 1U);
}

uint8_t glyphSpacingColumns(uint8_t character) {
  return (character == ' ') ? 2U : 1U;
}

void drawMask(const uint8_t rows[5], uint32_t colorValue) {
  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      const bool on = (rows[y] >> (4 - x)) & 0x01;
      setFramePixel(static_cast<uint8_t>(y * 5 + x), on ? colorValue : 0x000000);
    }
  }
}

void drawMaskOverlay(const uint8_t rows[5], uint32_t colorValue) {
  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      const bool on = (rows[y] >> (4 - x)) & 0x01;
      if (!on) {
        continue;
      }

      setFramePixel(static_cast<uint8_t>(y * 5 + x), colorValue);
    }
  }
}

void drawWaveRow(int8_t rowIndex, uint8_t rowMask, uint32_t colorValue) {
  if (rowIndex < 0 || rowIndex > 4) {
    return;
  }

  for (uint8_t x = 0; x < 5; ++x) {
    const bool on = (rowMask >> (4 - x)) & 0x01;
    if (!on) {
      continue;
    }

    setFramePixel(static_cast<uint8_t>(rowIndex) * 5U + x, colorValue);
  }
}

void drawPixel(int8_t x, int8_t y, uint32_t colorValue) {
  if (x < 0 || x > 4 || y < 0 || y > 4) {
    return;
  }

  setFramePixel(static_cast<uint8_t>(y) * 5U + static_cast<uint8_t>(x), colorValue);
}

}  // namespace

void clearAllPixels() {
  clearFrameBuffer();
  flushFrameBuffer();
}

void clearMatrix() {
  clearFrameBuffer();
  setFramePixel(kStatusPixelIndex, 0x000000);
  setFramePixel(kBeatDotPixelIndex, 0x000000);
  flushFrameBuffer();
}

void clearStatusPixel() {
  M5.dis.drawpix(kStatusPixelIndex, 0x000000);
}

void clearBeatDotPixel() {
  M5.dis.drawpix(kBeatDotPixelIndex, 0x000000);
}

void drawSceneNumber(size_t sceneIndex) {
  clearFrameBuffer();

  if (sceneIndex >= sceneSlotCount()) {
    flushFrameBuffer();
    return;
  }

  const uint8_t* rows = kDigitMasks[sceneIndex];
  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      if ((y * 5U + x) == kBeatDotPixelIndex) {
        continue;
      }

      setFramePixel(static_cast<uint8_t>(y * 5 + x), scenePixelColor(sceneIndex, x, y));
    }
  }

  flushFrameBuffer();
}

void drawBeatDotOverlay(uint8_t beatDotBeat, uint32_t beatDotColorOverride) {
  M5.dis.drawpix(kBeatDotPixelIndex, beatDotColor(beatDotBeat, beatDotColorOverride));
}

void drawStatusPixelOverlay(uint32_t colorValue) {
  M5.dis.drawpix(kStatusPixelIndex, colorValue);
}

size_t measureTextColumns(const uint8_t* text, size_t length) {
  if (text == nullptr || length == 0) {
    return 0;
  }

  size_t totalColumns = 0;
  for (size_t index = 0; index < length; ++index) {
    const Glyph* glyph = glyphForCharacter(text[index]);
    if (glyph == nullptr) {
      continue;
    }

    totalColumns += glyphColumnCount(*glyph);
    if ((index + 1U) < length) {
      totalColumns += glyphSpacingColumns(text[index]);
    }
  }

  return totalColumns;
}

void drawTextCharacter(uint8_t character, uint32_t colorValue) {
  clearFrameBuffer();

  colorValue = scaleColor(colorValue, 204);

  const Glyph* glyph = glyphForCharacter(character);
  if (glyph == nullptr) {
    flushFrameBuffer();
    return;
  }

  const uint8_t startColumn = glyphTrimmedStartColumn(*glyph);
  const uint8_t width = glyphColumnCount(*glyph);
  const uint8_t xOffset = (width < kMatrixWidth)
    ? static_cast<uint8_t>((kMatrixWidth - width) / 2U)
    : 0;

  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t glyphColumn = 0; glyphColumn < width; ++glyphColumn) {
      const uint8_t sourceColumn = static_cast<uint8_t>(startColumn + glyphColumn);
      const bool on = (glyph->rows[y] >> (4U - sourceColumn)) & 0x01U;
      if (!on) {
        continue;
      }

      const uint8_t x = static_cast<uint8_t>(xOffset + glyphColumn);
      setFramePixel(static_cast<uint8_t>(y * 5U + x), colorValue);
    }
  }

  flushFrameBuffer();
}

void drawTextWindow(const uint8_t* text,
                    size_t length,
                    int16_t startColumn,
                    uint32_t colorValue) {
  clearFrameBuffer();

  colorValue = scaleColor(colorValue, 204);

  if (text == nullptr || length == 0) {
    flushFrameBuffer();
    return;
  }

  for (uint8_t screenColumn = 0; screenColumn < kMatrixWidth; ++screenColumn) {
    const int16_t textColumn = static_cast<int16_t>(startColumn + screenColumn);
    if (textColumn < 0) {
      continue;
    }

    int16_t remainingColumns = textColumn;
    for (size_t index = 0; index < length; ++index) {
      const uint8_t character = text[index];
      const Glyph* glyph = glyphForCharacter(character);
      if (glyph == nullptr) {
        continue;
      }

      const uint8_t glyphWidth = glyphColumnCount(*glyph);
      if (remainingColumns < glyphWidth) {
        const uint8_t sourceColumn = static_cast<uint8_t>(
          glyphTrimmedStartColumn(*glyph) + remainingColumns);
        const uint8_t columnMask = glyphColumnMask(*glyph, sourceColumn);
        for (uint8_t row = 0; row < kMatrixHeight; ++row) {
          if (((columnMask >> row) & 0x01U) == 0) {
            continue;
          }

          setFramePixel(static_cast<uint8_t>(row * 5U + screenColumn), colorValue);
        }
        break;
      }

      remainingColumns -= glyphWidth;
      const uint8_t spacing = ((index + 1U) < length)
        ? glyphSpacingColumns(character)
        : 0;
      if (remainingColumns < spacing) {
        break;
      }
      remainingColumns -= spacing;
    }
  }

  flushFrameBuffer();
}

void drawSpeakerIcon(uint32_t colorValue) {
  clearFrameBuffer();
  drawMask(kSpeakerIconMask, colorValue);
  flushFrameBuffer();
}

void drawSpeakerMutedIcon(uint32_t colorValue) {
  clearFrameBuffer();
  drawMask(kSpeakerMutedIconMask, colorValue);
  flushFrameBuffer();
}

void drawAiWaveAnimation(uint32_t elapsedMs,
                         uint32_t durationMs,
                         bool bottomToTop,
                         uint32_t colorValue) {
  clearFrameBuffer();

  if (durationMs == 0) {
    flushFrameBuffer();
    return;
  }

  const uint32_t clampedElapsed = (elapsedMs > durationMs) ? durationMs : elapsedMs;
  uint8_t step = static_cast<uint8_t>((clampedElapsed * 7U) / durationMs);
  if (step > 6U) {
    step = 6U;
  }

  const int8_t brightRow = bottomToTop
    ? static_cast<int8_t>(5 - step)
    : static_cast<int8_t>(step - 1);
  const int8_t nearTrailRow = bottomToTop ? (brightRow + 1) : (brightRow - 1);
  const int8_t midTrailRow = bottomToTop ? (brightRow + 2) : (brightRow - 2);
  const int8_t farTrailRow = bottomToTop ? (brightRow + 3) : (brightRow - 3);
  const uint32_t nearTrailColor = scaleColor(colorValue, 138);
  const uint32_t midTrailColor = scaleColor(colorValue, 84);
  const uint32_t farTrailColor = scaleColor(colorValue, 36);

  drawWaveRow(farTrailRow, 0b11111, farTrailColor);
  drawWaveRow(midTrailRow, 0b11111, midTrailColor);
  drawWaveRow(nearTrailRow, 0b11111, nearTrailColor);
  drawWaveRow(brightRow, 0b11111, colorValue);
  flushFrameBuffer();
}

}  // namespace decaflash::brain::matrix
