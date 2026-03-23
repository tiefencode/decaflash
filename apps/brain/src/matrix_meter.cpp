#include "matrix_meter.h"

#include <Arduino.h>
#include <M5Atom.h>

namespace decaflash::brain::matrix {

namespace {

static constexpr uint8_t kMatrixPixelCount = 25;
static constexpr uint8_t kMeterPixelOrder[kMatrixPixelCount] = {
  12,  0, 24,  4, 20,
   7, 17, 11, 13,  2,
  22,  6,  8, 16, 18,
   1,  3,  5,  9, 15,
  19, 21, 23, 10, 14,
};

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static constexpr RgbColor kMeterGradient[] = {
  {  0,   0,   0},
  {  0, 105, 255},
  {210,   0, 170},
  {255,  60,  35},
};

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

void clearMatrix() {
  for (uint8_t i = 0; i < kMatrixPixelCount; ++i) {
    M5.dis.drawpix(i, 0x000000);
  }
}

uint8_t blendChannel(uint8_t start, uint8_t end, uint8_t mix) {
  const uint16_t inverseMix = 255U - mix;
  return static_cast<uint8_t>(
    ((static_cast<uint16_t>(start) * inverseMix) +
     (static_cast<uint16_t>(end) * mix)) / 255U
  );
}

uint32_t blendColor(const RgbColor& start, const RgbColor& end, uint8_t mix) {
  return color(
    blendChannel(start.r, end.r, mix),
    blendChannel(start.g, end.g, mix),
    blendChannel(start.b, end.b, mix)
  );
}

uint32_t meterColorForPixel(uint8_t pixelIndex) {
  constexpr size_t kGradientStopCount = sizeof(kMeterGradient) / sizeof(kMeterGradient[0]);
  constexpr uint16_t kGradientSegments = kGradientStopCount - 1;

  if (pixelIndex >= (kMatrixPixelCount - 1)) {
    const RgbColor& last = kMeterGradient[kGradientStopCount - 1];
    return color(last.r, last.g, last.b);
  }

  const uint16_t scaledPosition =
    static_cast<uint16_t>((static_cast<uint32_t>(pixelIndex) * kGradientSegments * 255U) /
                          (kMatrixPixelCount - 1));
  const uint8_t segment = scaledPosition / 255U;
  const uint8_t mix = scaledPosition % 255U;
  return blendColor(kMeterGradient[segment], kMeterGradient[segment + 1], mix);
}

}  // namespace

void drawMicrophoneMeter(uint8_t filledPixels) {
  clearMatrix();

  if (filledPixels > kMatrixPixelCount) {
    filledPixels = kMatrixPixelCount;
  }

  for (uint8_t slot = 0; slot < filledPixels; ++slot) {
    M5.dis.drawpix(kMeterPixelOrder[slot], meterColorForPixel(slot));
  }
}

}  // namespace decaflash::brain::matrix
