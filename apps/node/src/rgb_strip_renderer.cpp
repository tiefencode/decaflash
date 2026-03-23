#include "rgb_strip_renderer.h"

#include <FastLED.h>

namespace {

static constexpr uint8_t kRgbDataPinPrimary = 26;
static constexpr uint8_t kRgbDataPinSecondary = 32;
static constexpr uint8_t kRgbLedCount = 15;
static constexpr uint16_t kStartupProbeStepMs = 220;

bool commandNameEquals(const decaflash::NodeCommand& command, const char* expected) {
  return strncmp(command.name, expected, decaflash::kCommandNameLength) == 0;
}

CRGB scaleColor(const CRGB& color, uint8_t intensity) {
  CRGB scaled = color;
  scaled.nscale8_video(intensity);
  return scaled;
}

}  // namespace

namespace {
CRGB gStripLeds[kRgbLedCount];
}

void RgbStripRenderer::begin() {
  if (initialized_) {
    allOff();
    return;
  }

  FastLED.addLeds<SK6812, kRgbDataPinPrimary, GRB>(gStripLeds, kLedCount);
  FastLED.addLeds<SK6812, kRgbDataPinSecondary, GRB>(gStripLeds, kLedCount);
  FastLED.setCorrection(TypicalLEDStrip);
  FastLED.setBrightness(255);
  initialized_ = true;
  runStartupProbe();
  allOff();
}

void RgbStripRenderer::allOff() {
  if (!initialized_) {
    return;
  }

  fill_solid(gStripLeds, kLedCount, CRGB::Black);
  FastLED.show();
  delay(8);
}

void RgbStripRenderer::setCommand(const decaflash::NodeCommand& command) {
  currentCommand_ = command;
  pulseIndex_ = 0;
}

void RgbStripRenderer::flash100(uint16_t flashMs) {
  if (!initialized_) {
    begin();
  }

  renderPulse();
  delay(flashMs);
  allOff();
  pulseIndex_++;
}

void RgbStripRenderer::renderSolid(uint8_t red, uint8_t green, uint8_t blue) {
  fill_solid(gStripLeds, kLedCount, CRGB(red, green, blue));
  FastLED.show();
}

void RgbStripRenderer::runStartupProbe() {
  renderSolid(255, 0, 0);
  delay(kStartupProbeStepMs);
  renderSolid(0, 255, 0);
  delay(kStartupProbeStepMs);
  renderSolid(0, 0, 255);
  delay(kStartupProbeStepMs);
}

RgbStripRenderer::Palette RgbStripRenderer::paletteForCommand(
  const decaflash::NodeCommand& command
) const {
  if (commandNameEquals(command, "Neon Pulse")) {
    return {0, 255, 170, 40, 100, 255};
  }

  if (commandNameEquals(command, "Rose Downbeat")) {
    return {255, 40, 120, 255, 220, 240};
  }

  if (commandNameEquals(command, "Cyan Double")) {
    return {0, 210, 255, 180, 255, 255};
  }

  if (commandNameEquals(command, "Chase Quad")) {
    return {255, 110, 0, 0, 160, 255};
  }

  if (commandNameEquals(command, "Sunrise 5x")) {
    return {255, 90, 20, 255, 210, 60};
  }

  return {255, 255, 255, 60, 110, 255};
}

void RgbStripRenderer::renderPulse() {
  const Palette palette = paletteForCommand(currentCommand_);
  const CRGB primary = scaleColor(
    CRGB(palette.primaryR, palette.primaryG, palette.primaryB),
    currentCommand_.intensity
  );
  const CRGB accent = scaleColor(
    CRGB(palette.accentR, palette.accentG, palette.accentB),
    currentCommand_.intensity
  );
  const CRGB background = scaleColor(accent, currentCommand_.intensity / 4U);

  fill_solid(gStripLeds, kLedCount, CRGB::Black);

  if (commandNameEquals(currentCommand_, "Rose Downbeat")) {
    fill_solid(gStripLeds, kLedCount, background);
    for (uint8_t i = 5; i <= 9; ++i) {
      gStripLeds[i] = accent;
    }
  } else if (commandNameEquals(currentCommand_, "Cyan Double")) {
    const bool leadingHalf = (pulseIndex_ % 2U) == 0U;
    for (uint8_t i = 0; i < kLedCount; ++i) {
      const bool firstHalf = i < (kLedCount / 2U);
      gStripLeds[i] = (firstHalf == leadingHalf) ? primary : background;
    }
    gStripLeds[kLedCount / 2U] = accent;
  } else if (commandNameEquals(currentCommand_, "Chase Quad")) {
    const uint8_t segment = pulseIndex_ % 5U;
    const uint8_t start = segment * 3U;
    fill_solid(gStripLeds, kLedCount, background);
    for (uint8_t offset = 0; offset < 3U; ++offset) {
      const uint8_t index = start + offset;
      if (index < kLedCount) {
        gStripLeds[index] = primary;
      }
    }
    if (start > 0) {
      gStripLeds[start - 1] = accent;
    }
  } else if (commandNameEquals(currentCommand_, "Sunrise 5x")) {
    const uint8_t steps = static_cast<uint8_t>((pulseIndex_ % 5U) + 1U);
    const uint8_t litCount = steps * 3U;
    for (uint8_t i = 0; i < kLedCount; ++i) {
      if (i < litCount) {
        gStripLeds[i] = blend(
          primary,
          accent,
          static_cast<uint8_t>((i * 255U) / (kLedCount - 1U))
        );
      } else {
        gStripLeds[i] = background;
      }
    }
  } else {
    fill_solid(gStripLeds, kLedCount, primary);
    for (uint8_t i = 2; i < kLedCount; i += 5U) {
      gStripLeds[i] = accent;
    }
  }

  FastLED.show();
}
