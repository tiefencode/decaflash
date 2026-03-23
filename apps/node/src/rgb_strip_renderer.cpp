#include "rgb_strip_renderer.h"

#include <FastLED.h>

namespace {

static constexpr uint8_t kRgbDataPinPrimary = 26;
static constexpr uint8_t kRgbDataPinSecondary = 32;
static constexpr uint8_t kRgbLedCount = 15;
static constexpr uint16_t kConnectPulseMs = 140;

CRGB gStripLeds[kRgbLedCount];

CRGB scaleColor(const CRGB& color, uint8_t level) {
  CRGB scaled = color;
  scaled.nscale8_video(level);
  return scaled;
}

uint8_t hash8(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dUL;
  value ^= value >> 15;
  value *= 0x846ca68bUL;
  value ^= value >> 16;
  return static_cast<uint8_t>(value & 0xFFU);
}

uint8_t segmentNoise8(uint32_t elapsedMs, uint16_t segmentMs, uint8_t salt) {
  const uint32_t safeSegmentMs = (segmentMs == 0) ? 1U : static_cast<uint32_t>(segmentMs);
  const uint32_t segment = elapsedMs / safeSegmentMs;
  return hash8(segment + static_cast<uint32_t>(salt) * 131U);
}

uint8_t pseudoFlicker(uint32_t now, uint8_t pixel) {
  const uint32_t value = now / 35U + static_cast<uint32_t>(pixel * 23U);
  return static_cast<uint8_t>((value * 37U + pixel * 19U) & 0x3F);
}

uint8_t wave8FromProgress(uint8_t progress) {
  return (progress < 128U)
    ? static_cast<uint8_t>(progress * 2U)
    : static_cast<uint8_t>((255U - progress) * 2U);
}

CRGB primaryColor(const decaflash::RgbCommand& command) {
  return CRGB(command.primaryR, command.primaryG, command.primaryB);
}

CRGB secondaryColor(const decaflash::RgbCommand& command) {
  return CRGB(command.secondaryR, command.secondaryG, command.secondaryB);
}

}  // namespace

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

void RgbStripRenderer::setNodeEffect(decaflash::NodeEffect nodeEffect) {
  nodeEffect_ = nodeEffect;
  effectStartedAtMs_ = millis();
  accentStartedAtMs_ = 0;
  accentEndsAtMs_ = 0;
}

void RgbStripRenderer::setCommand(const decaflash::RgbCommand& command) {
  currentCommand_ = command;
  effectStartedAtMs_ = millis();
  accentStartedAtMs_ = 0;
  accentEndsAtMs_ = 0;
}

void RgbStripRenderer::flash100(uint16_t flashMs) {
  if (!initialized_) {
    begin();
  }

  renderSolid(255, 255, 255);
  delay(flashMs == 0 ? kConnectPulseMs : flashMs);
  allOff();
}

void RgbStripRenderer::triggerAccent() {
  const uint32_t now = millis();
  accentStartedAtMs_ = now;
  const uint16_t accentDurationMs =
    (currentCommand_.accentDurationMs == 0) ? 160 : currentCommand_.accentDurationMs;
  accentEndsAtMs_ = now + accentDurationMs;
}

void RgbStripRenderer::service(uint32_t now) {
  if (!initialized_) {
    begin();
  }

  switch (currentCommand_.pattern) {
    case decaflash::RgbPattern::Breathe:
      renderBreathe(now);
      break;

    case decaflash::RgbPattern::BeatPulse:
      renderBeatPulse(now);
      break;

    case decaflash::RgbPattern::Accent:
      renderAccent(now);
      break;

    case decaflash::RgbPattern::RunnerFlicker:
      renderRunnerFlicker(now);
      break;

    case decaflash::RgbPattern::Off:
    default:
      fill_solid(gStripLeds, kLedCount, CRGB::Black);
      break;
  }

  FastLED.show();
}

void RgbStripRenderer::renderSolid(uint8_t red, uint8_t green, uint8_t blue) {
  fill_solid(gStripLeds, kLedCount, CRGB(red, green, blue));
  FastLED.show();
}

void RgbStripRenderer::renderBreathe(uint32_t now) {
  const uint32_t elapsedMs = now - effectStartedAtMs_;
  const uint8_t level =
    breatheLevel(now, currentCommand_.floorLevel, currentCommand_.peakLevel);
  const uint16_t colorCycleMs =
    (currentCommand_.cycleMs == 0) ? 6000U : static_cast<uint16_t>(currentCommand_.cycleMs * 2U);
  const uint8_t colorPhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / colorCycleMs
  );
  const uint8_t tidePhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / static_cast<uint32_t>(colorCycleMs * 3UL)
  );
  const uint8_t macroNoise = segmentNoise8(elapsedMs, static_cast<uint16_t>(colorCycleMs * 2U), 17U);
  const CRGB primary = primaryColor(currentCommand_);
  const CRGB secondary = secondaryColor(currentCommand_);
  const CRGB driftColor = blend(
    primary,
    secondary,
    ease8InOutCubic(colorPhase)
  );

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint8_t lanePhase = tidePhase + static_cast<uint8_t>(i * 19U);
    const uint8_t tideMix = scale8(
      wave8FromProgress(lanePhase),
      static_cast<uint8_t>(74U + macroNoise / 4U)
    );
    uint16_t pixelLevel = level;
    if (pixelLevel > 10U) {
      pixelLevel -= 10U;
    } else {
      pixelLevel = 0U;
    }
    pixelLevel += scale8(tideMix, 24U);
    pixelLevel += macroNoise / 20U;
    gStripLeds[i] = scaleColor(
      blend(driftColor, secondary, tideMix),
      clampLevel(pixelLevel)
    );
  }
}

void RgbStripRenderer::renderBeatPulse(uint32_t now) {
  const uint32_t elapsedMs = now - effectStartedAtMs_;
  const uint16_t macroCycleMs =
    (currentCommand_.cycleMs == 0) ? 2200U : static_cast<uint16_t>(currentCommand_.cycleMs * 3U);
  const uint8_t macroPhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / macroCycleMs
  );
  const uint8_t macroNoise = segmentNoise8(elapsedMs, static_cast<uint16_t>(macroCycleMs * 2U), 29U);
  const uint8_t trailPhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / static_cast<uint32_t>(macroCycleMs * 2UL)
  );
  const uint8_t baseLevel =
    breatheLevel(now, currentCommand_.floorLevel, currentCommand_.baseLevel);
  const uint8_t overlayLevel =
    accentLevel(now, baseLevel, currentCommand_.peakLevel);
  const CRGB baseBlend = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    static_cast<uint8_t>(scale8(ease8InOutCubic(macroPhase), static_cast<uint8_t>(80U + macroNoise / 3U)))
  );
  const CRGB accentTint = blend(
    secondaryColor(currentCommand_),
    primaryColor(currentCommand_),
    static_cast<uint8_t>(macroNoise / 2U)
  );
  const CRGB accentColor = scaleColor(
    accentTint,
    overlayLevel
  );
  const uint8_t bandCenter = static_cast<uint8_t>(
    (wave8FromProgress(static_cast<uint8_t>(macroPhase + macroNoise / 3U)) * (kLedCount - 1U)) / 255U
  );
  const uint8_t bandHalfWidth = static_cast<uint8_t>(1U + (wave8FromProgress(static_cast<uint8_t>(macroPhase + macroNoise)) / 72U));

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint8_t distance = (i > bandCenter) ? (i - bandCenter) : (bandCenter - i);
    const uint8_t laneMix = scale8(
      wave8FromProgress(static_cast<uint8_t>(trailPhase + i * 17U)),
      static_cast<uint8_t>(36U + macroNoise / 5U)
    );
    const CRGB laneBase = blend(baseBlend, accentTint, laneMix);
    const CRGB baseColor = scaleColor(laneBase, baseLevel);
    gStripLeds[i] = baseColor;
    if (distance <= bandHalfWidth) {
      const uint8_t overlayMix = static_cast<uint8_t>(210U - distance * 46U);
      gStripLeds[i] = blend(baseColor, accentColor, overlayMix);
    } else if (((i + macroNoise / 48U) % 5U) == 0U && overlayLevel > baseLevel) {
      gStripLeds[i] = blend(baseColor, accentColor, 58U);
    }
  }
}

void RgbStripRenderer::renderAccent(uint32_t now) {
  const uint32_t elapsedMs = now - effectStartedAtMs_;
  const uint16_t rotateCycleMs =
    (currentCommand_.cycleMs == 0) ? 2400U : currentCommand_.cycleMs;
  const uint8_t rotation = static_cast<uint8_t>((elapsedMs / (rotateCycleMs / 3U + 1U)) % 5U);
  const uint8_t macroNoise = segmentNoise8(elapsedMs, static_cast<uint16_t>(rotateCycleMs * 2U), 43U);
  const bool dormantWindow = macroNoise < 72U;
  const uint8_t stripeWidth = (macroNoise > 180U) ? 2U : 1U;
  const uint8_t baseLevel = dormantWindow ? static_cast<uint8_t>(currentCommand_.floorLevel / 2U)
                                          : currentCommand_.floorLevel;
  const uint8_t overlayLevel =
    accentLevel(now, currentCommand_.baseLevel, currentCommand_.peakLevel);
  const CRGB baseColor = scaleColor(
    primaryColor(currentCommand_),
    baseLevel
  );
  const CRGB accentColor = scaleColor(
    secondaryColor(currentCommand_),
    overlayLevel
  );

  fill_solid(gStripLeds, kLedCount, dormantWindow ? CRGB::Black : baseColor);
  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint8_t lane = static_cast<uint8_t>((i + rotation) % 5U);
    if (lane < stripeWidth) {
      gStripLeds[i] = accentColor;
    } else if (!dormantWindow && ((i + macroNoise / 32U) % 7U) == 0U) {
      gStripLeds[i] = baseColor;
    }
  }
}

void RgbStripRenderer::renderRunnerFlicker(uint32_t now) {
  const uint32_t elapsedMs = now - effectStartedAtMs_;
  const uint16_t cycleMs = (currentCommand_.cycleMs == 0) ? 900 : currentCommand_.cycleMs;
  const uint16_t bounceCycleMs = static_cast<uint16_t>(cycleMs * 2U);
  const uint8_t motionNoise = segmentNoise8(elapsedMs, static_cast<uint16_t>(cycleMs * 4U), 61U);
  const uint8_t bouncePhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / bounceCycleMs
  );
  uint8_t runner = static_cast<uint8_t>(
    (wave8FromProgress(bouncePhase) * (kLedCount - 1U)) / 255U
  );
  if ((motionNoise & 0x80U) != 0U) {
    runner = static_cast<uint8_t>((kLedCount - 1U) - runner);
  }
  const uint8_t accentBoost = accentLevel(now, 0, currentCommand_.peakLevel);
  const uint8_t colorPhase = static_cast<uint8_t>(
    (elapsedMs * 255UL) / static_cast<uint16_t>(cycleMs * 4U)
  );
  const uint8_t stripeStride = static_cast<uint8_t>(3U + (motionNoise % 3U));
  const uint8_t stripePhase = static_cast<uint8_t>(motionNoise / 40U);
  const uint8_t sparkleThreshold = static_cast<uint8_t>(184U + motionNoise / 4U);
  const CRGB primary = primaryColor(currentCommand_);
  const CRGB secondary = secondaryColor(currentCommand_);

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint8_t distance = (runner > i) ? (runner - i) : (i - runner);
    const uint8_t runnerLevel = (distance == 0) ? currentCommand_.peakLevel :
                                (distance == 1) ? currentCommand_.baseLevel :
                                currentCommand_.floorLevel;
    uint16_t flickerLevel = currentCommand_.floorLevel + pseudoFlicker(now, i);
    if (flickerLevel > currentCommand_.baseLevel) {
      flickerLevel = currentCommand_.baseLevel;
    }

    const uint8_t sparkleSeed = hash8((elapsedMs / 85U) + static_cast<uint32_t>(i * 29U) + motionNoise);
    const bool sparkle = sparkleSeed > sparkleThreshold;
    const bool useAccentColor =
      sparkle ||
      ((i + stripePhase + (wave8FromProgress(colorPhase) / 64U) + (now / 110U)) % stripeStride) == 0U;
    const uint8_t finalLevel = clampLevel(
      runnerLevel + static_cast<uint8_t>(accentBoost / 3U)
    );
    const CRGB movingPrimary = blend(
      primary,
      secondary,
      static_cast<uint8_t>(scale8(colorPhase, static_cast<uint8_t>(90U + motionNoise / 2U)))
    );
    const CRGB primary = scaleColor(
      movingPrimary,
      finalLevel
    );
    const CRGB secondary = scaleColor(
      CRGB(currentCommand_.secondaryR, currentCommand_.secondaryG, currentCommand_.secondaryB),
      clampLevel(flickerLevel + accentBoost)
    );
    gStripLeds[i] = useAccentColor ? secondary : primary;
  }
}

uint8_t RgbStripRenderer::breatheLevel(uint32_t now, uint8_t low, uint8_t high) const {
  if (high <= low) {
    return low;
  }

  const uint16_t cycleMs = (currentCommand_.cycleMs == 0) ? 1800 : currentCommand_.cycleMs;
  const uint8_t wave = ease8InOutCubic(static_cast<uint8_t>(
    ((now - effectStartedAtMs_) * 255UL) / cycleMs
  ));
  const uint8_t span = high - low;
  return low + scale8(span, wave);
}

uint8_t RgbStripRenderer::accentLevel(uint32_t now, uint8_t low, uint8_t high) const {
  if (high <= low || accentEndsAtMs_ == 0 || now >= accentEndsAtMs_) {
    return low;
  }

  const uint32_t accentDurationMs = accentEndsAtMs_ - accentStartedAtMs_;
  if (accentDurationMs == 0) {
    return high;
  }

  const uint32_t elapsedMs = now - accentStartedAtMs_;
  if (elapsedMs >= accentDurationMs) {
    return low;
  }

  const uint8_t wave = 255U - static_cast<uint8_t>((elapsedMs * 255UL) / accentDurationMs);
  const uint8_t span = high - low;
  return low + scale8(span, ease8InOutCubic(wave));
}

uint8_t RgbStripRenderer::clampLevel(uint16_t level) const {
  return static_cast<uint8_t>(level > 255U ? 255U : level);
}
