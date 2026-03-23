#include "matrix_meter.h"

#include <Arduino.h>
#include <M5Atom.h>

namespace decaflash::brain::matrix {

namespace {

static constexpr uint8_t kMatrixPixelCount = 25;
static constexpr uint8_t kBeatDotPixelIndex = 4;
static constexpr uint8_t kMeterDrawablePixelCount = kMatrixPixelCount - 1;
static constexpr uint32_t kMeterDriftIntervalMs = 220;
static constexpr uint8_t kMeterRiseStep = 18;
static constexpr uint8_t kMeterFallStep = 14;

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

static constexpr RgbColor kMeterGradient[] = {
  {  0,   0,   0},
  {  0, 105, 255},
  {255,  40, 150},
  {235,   0, 255},
};
uint8_t meterPixelOrder[kMeterDrawablePixelCount] = {};
uint8_t meterPixelLevels[kMatrixPixelCount] = {};
bool meterPixelActive[kMatrixPixelCount] = {};
bool meterOrderInitialized = false;
uint32_t lastMeterDriftAtMs = 0;

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

void clearMatrix() {
  for (uint8_t i = 0; i < kMatrixPixelCount; ++i) {
    if (i == kBeatDotPixelIndex) {
      continue;
    }

    M5.dis.drawpix(i, 0x000000);
  }
}

void swapPixelOrder(uint8_t left, uint8_t right) {
  const uint8_t pixel = meterPixelOrder[left];
  meterPixelOrder[left] = meterPixelOrder[right];
  meterPixelOrder[right] = pixel;
}

void initializeMeterPixelOrder() {
  uint8_t slot = 0;
  for (uint8_t pixel = 0; pixel < kMatrixPixelCount; ++pixel) {
    if (pixel == kBeatDotPixelIndex) {
      continue;
    }

    meterPixelOrder[slot++] = pixel;
  }

  randomSeed(static_cast<uint32_t>(micros()) ^ 0x4D455445UL);
  for (uint8_t i = kMeterDrawablePixelCount - 1; i > 0; --i) {
    swapPixelOrder(i, static_cast<uint8_t>(random(i + 1)));
  }

  meterOrderInitialized = true;
  lastMeterDriftAtMs = millis();
}

void driftMeterPixelOrder(uint32_t now, uint8_t filledPixels) {
  if (!meterOrderInitialized) {
    initializeMeterPixelOrder();
  }

  if (filledPixels > kMeterDrawablePixelCount) {
    filledPixels = kMeterDrawablePixelCount;
  }

  const uint32_t elapsed = now - lastMeterDriftAtMs;
  if (elapsed < kMeterDriftIntervalMs) {
    return;
  }

  uint8_t driftSteps = elapsed / kMeterDriftIntervalMs;
  if (driftSteps > 4) {
    driftSteps = 4;
  }
  lastMeterDriftAtMs += static_cast<uint32_t>(driftSteps) * kMeterDriftIntervalMs;

  for (uint8_t step = 0; step < driftSteps; ++step) {
    if (filledPixels == 0 || filledPixels >= kMeterDrawablePixelCount) {
      swapPixelOrder(
        static_cast<uint8_t>(random(kMeterDrawablePixelCount)),
        static_cast<uint8_t>(random(kMeterDrawablePixelCount))
      );
      continue;
    }

    const uint8_t activeSlot = static_cast<uint8_t>(random(filledPixels));
    const uint8_t inactiveSlot = static_cast<uint8_t>(
      filledPixels + random(kMeterDrawablePixelCount - filledPixels)
    );
    swapPixelOrder(activeSlot, inactiveSlot);
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

  if (pixelIndex == 0) {
    return 0x000000;
  }

  if (pixelIndex >= 255U) {
    const RgbColor& last = kMeterGradient[kGradientStopCount - 1];
    return color(last.r, last.g, last.b);
  }

  const uint16_t scaledPosition =
    static_cast<uint16_t>(pixelIndex * kGradientSegments);
  const uint8_t segment = scaledPosition / 255U;
  const uint8_t mix = scaledPosition % 255U;
  return blendColor(kMeterGradient[segment], kMeterGradient[segment + 1], mix);
}

void updateMeterPixelLevels(uint8_t filledPixels) {
  for (uint8_t pixel = 0; pixel < kMatrixPixelCount; ++pixel) {
    meterPixelActive[pixel] = false;
  }

  for (uint8_t slot = 0; slot < filledPixels; ++slot) {
    meterPixelActive[meterPixelOrder[slot]] = true;
  }

  for (uint8_t pixel = 0; pixel < kMatrixPixelCount; ++pixel) {
    if (pixel == kBeatDotPixelIndex) {
      continue;
    }

    uint8_t level = meterPixelLevels[pixel];
    if (meterPixelActive[pixel]) {
      const uint16_t nextLevel = static_cast<uint16_t>(level) + kMeterRiseStep;
      level = (nextLevel > 255U) ? 255U : static_cast<uint8_t>(nextLevel);
    } else if (level > kMeterFallStep) {
      level = static_cast<uint8_t>(level - kMeterFallStep);
    } else {
      level = 0;
    }

    meterPixelLevels[pixel] = level;
  }
}

}  // namespace

void drawMicrophoneMeter(uint8_t filledPixels) {
  clearMatrix();

  driftMeterPixelOrder(millis(), filledPixels);

  if (filledPixels > kMeterDrawablePixelCount) {
    filledPixels = kMeterDrawablePixelCount;
  }

  updateMeterPixelLevels(filledPixels);

  for (uint8_t pixel = 0; pixel < kMatrixPixelCount; ++pixel) {
    if (pixel == kBeatDotPixelIndex || meterPixelLevels[pixel] == 0) {
      continue;
    }

    M5.dis.drawpix(pixel, meterColorForPixel(meterPixelLevels[pixel]));
  }
}

}  // namespace decaflash::brain::matrix
