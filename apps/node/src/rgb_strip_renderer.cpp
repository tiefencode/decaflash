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

struct WavePhaseLayout {
  uint8_t deepBlueEnd;
  uint8_t lightBlueEnd;
  uint8_t whiteRiseEnd;
  uint8_t whiteHoldEnd;
};

struct WavePixelState {
  CRGB color = CRGB::Black;
  uint8_t level = 0;
};

uint8_t segmentMix8(uint8_t phase, uint8_t start, uint8_t end) {
  if (end <= start) {
    return 255U;
  }

  if (phase <= start) {
    return 0U;
  }

  if (phase >= end) {
    return 255U;
  }

  return static_cast<uint8_t>(
    (static_cast<uint16_t>(phase - start) * 255U) / static_cast<uint16_t>(end - start)
  );
}

uint8_t mixLevel(uint8_t from, uint8_t to, uint8_t mix) {
  const int16_t delta = static_cast<int16_t>(to) - static_cast<int16_t>(from);
  int16_t value = static_cast<int16_t>(from) + ((delta * mix) / 255);
  if (value < 0) {
    value = 0;
  } else if (value > 255) {
    value = 255;
  }

  return static_cast<uint8_t>(value);
}

uint8_t classicPulseEnvelope8(uint8_t phase) {
  if (phase < 78U) {
    return ease8InOutCubic(segmentMix8(phase, 0U, 78U));
  }

  if (phase < 112U) {
    return 255U;
  }

  return static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, 112U, 255U)));
}

uint8_t pulseCrestMix8(uint8_t phase) {
  if (phase < 62U || phase > 146U) {
    return 0U;
  }

  return scale8(wave8FromProgress(segmentMix8(phase, 62U, 146U)), 132U);
}

uint8_t pulseWindow8(uint8_t phase, uint8_t start, uint8_t peak, uint8_t end, uint8_t strength) {
  if (phase <= start || phase >= end) {
    return 0U;
  }

  if (phase < peak) {
    return scale8(ease8InOutCubic(segmentMix8(phase, start, peak)), strength);
  }

  return scale8(
    static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, peak, end))),
    strength
  );
}

uint8_t heartbeatEnvelope8(uint8_t phase) {
  const uint8_t firstHit = pulseWindow8(phase, 18U, 34U, 58U, 255U);
  const uint8_t secondHit = pulseWindow8(phase, 70U, 84U, 114U, 176U);
  uint8_t tail = 0U;
  if (phase >= 84U) {
    tail = scale8(
      static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, 84U, 255U))),
      92U
    );
  }

  return max(firstHit, max(secondHit, tail));
}

WavePhaseLayout barWaveLayout(const decaflash::RgbCommand& command, uint32_t phraseDurationMs) {
  const uint8_t deepBlueEnd = 96U;
  const uint8_t lightBlueEnd = 206U;
  const uint8_t whiteRiseEnd = 228U;
  uint8_t whiteHoldWidth = static_cast<uint8_t>(
    (static_cast<uint32_t>(command.accentDurationMs) * 255UL) / phraseDurationMs
  );
  if (whiteHoldWidth > 10U) {
    whiteHoldWidth = 10U;
  }

  return {
    deepBlueEnd,
    lightBlueEnd,
    whiteRiseEnd,
    static_cast<uint8_t>(whiteRiseEnd + whiteHoldWidth),
  };
}

WavePixelState baseBarWaveState(
  const decaflash::RgbCommand& command,
  const WavePhaseLayout& layout,
  uint8_t phase
) {
  const CRGB deepBlue = primaryColor(command);
  const CRGB lightBlue = secondaryColor(command);
  const CRGB white = CRGB(190, 220, 255);
  const uint8_t preWhiteLevel = mixLevel(command.baseLevel, command.peakLevel, 196U);
  WavePixelState state = {};

  if (phase < layout.deepBlueEnd) {
    const uint8_t mix = ease8InOutCubic(segmentMix8(phase, 0U, layout.deepBlueEnd));
    state.color = blend(CRGB::Black, deepBlue, mix);
    state.level = mixLevel(command.floorLevel, command.baseLevel, mix);
    return state;
  }

  if (phase < layout.lightBlueEnd) {
    const uint8_t mix = ease8InOutCubic(
      segmentMix8(phase, layout.deepBlueEnd, layout.lightBlueEnd)
    );
    state.color = blend(deepBlue, lightBlue, mix);
    state.level = mixLevel(command.baseLevel, preWhiteLevel, mix);
    return state;
  }

  if (phase < layout.whiteRiseEnd) {
    const uint8_t mix = ease8InOutCubic(
      segmentMix8(phase, layout.lightBlueEnd, layout.whiteRiseEnd)
    );
    state.color = blend(lightBlue, white, mix);
    state.level = mixLevel(preWhiteLevel, command.peakLevel, mix);
    return state;
  }

  if (phase < layout.whiteHoldEnd) {
    state.color = white;
    state.level = command.peakLevel;
    return state;
  }

  const uint8_t mix = ease8InOutCubic(segmentMix8(phase, layout.whiteHoldEnd, 255U));
  state.color = blend(white, CRGB::Black, mix);
  state.level = mixLevel(command.peakLevel, 0U, mix);
  return state;
}

void applyBarWaveModulation(
  const decaflash::RgbCommand& command,
  const WavePhaseLayout& layout,
  uint32_t now,
  uint8_t pixel,
  uint8_t phase,
  WavePixelState& state
) {
  // This is the seam where future audio-driven modulation can hook in.
  const uint8_t flicker = pseudoFlicker(now, pixel);
  if (phase < layout.whiteRiseEnd) {
    const uint8_t lift = static_cast<uint8_t>(flicker / 6U);
    state.level = mixLevel(
      state.level,
      static_cast<uint8_t>(state.level > (255U - lift) ? 255U : state.level + lift),
      116U
    );
  }

  const bool nearCrest = phase >= static_cast<uint8_t>(layout.lightBlueEnd - 8U) &&
                         phase <= static_cast<uint8_t>(layout.whiteHoldEnd + 10U);
  const uint8_t sparkleSeed = hash8((now / 65U) + static_cast<uint32_t>(pixel * 37U) + phase);
  if (nearCrest && sparkleSeed > 240U) {
    state.color = blend(state.color, CRGB::White, 170U);
    state.level = mixLevel(state.level, command.peakLevel, 188U);
    return;
  }

  const uint8_t driftSeed = segmentNoise8(now + static_cast<uint32_t>(pixel * 41U), 280U, 73U);
  if (phase >= layout.deepBlueEnd && phase < layout.whiteRiseEnd) {
    state.color = blend(state.color, secondaryColor(command), static_cast<uint8_t>(driftSeed / 4U));
  }
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
  beatStartedAtMs_ = effectStartedAtMs_;
}

void RgbStripRenderer::setCommand(const decaflash::RgbCommand& command) {
  currentCommand_ = command;
  effectStartedAtMs_ = millis();
  accentStartedAtMs_ = 0;
  accentEndsAtMs_ = 0;
  beatStartedAtMs_ = effectStartedAtMs_;
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

void RgbStripRenderer::syncBeatClock(
  uint32_t now,
  uint32_t beatIntervalMs,
  uint8_t beatsPerBar,
  uint8_t beatInBar,
  uint32_t currentBar
) {
  beatStartedAtMs_ = now;
  beatIntervalMs_ = (beatIntervalMs == 0) ? 500U : beatIntervalMs;
  beatsPerBar_ = (beatsPerBar == 0) ? 4U : beatsPerBar;
  beatInBar_ = (beatInBar == 0) ? 1U : beatInBar;
  currentBar_ = (currentBar == 0) ? 1U : currentBar;
}

void RgbStripRenderer::service(uint32_t now) {
  if (!initialized_) {
    begin();
  }

  switch (currentCommand_.pattern) {
    case decaflash::RgbPattern::BarWave:
      renderBarWave(now);
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

void RgbStripRenderer::renderBarWave(uint32_t now) {
  const uint8_t safeBeatsPerBar = (beatsPerBar_ == 0) ? 4U : beatsPerBar_;
  const uint8_t phraseBars = (currentCommand_.triggerEveryBars == 0) ? 4U : currentCommand_.triggerEveryBars;
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  const uint32_t phraseDurationMs =
    static_cast<uint32_t>(phraseBars) * static_cast<uint32_t>(safeBeatsPerBar) * beatIntervalMs;
  const uint8_t safeBeatInBar = (beatInBar_ == 0) ? 1U : beatInBar_;
  const uint8_t startBeat =
    (currentCommand_.triggerBeat == 0 || currentCommand_.triggerBeat > safeBeatsPerBar)
      ? 1U
      : currentCommand_.triggerBeat;
  const uint32_t totalPhraseBeats =
    static_cast<uint32_t>(phraseBars) * static_cast<uint32_t>(safeBeatsPerBar);
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0 ? 1U : currentBar_) - 1U) * static_cast<uint32_t>(safeBeatsPerBar) +
    static_cast<uint32_t>(safeBeatInBar - 1U);
  const uint32_t phraseBeatIndex =
    (absoluteBeatIndex + totalPhraseBeats - static_cast<uint32_t>(startBeat - 1U)) % totalPhraseBeats;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) {
    elapsedBeatMs = beatIntervalMs;
  }

  uint32_t elapsedPhraseMs = phraseBeatIndex * beatIntervalMs + elapsedBeatMs;
  if (elapsedPhraseMs > phraseDurationMs) {
    elapsedPhraseMs = phraseDurationMs;
  }

  const uint32_t travelMs = (currentCommand_.cycleMs == 0) ? (beatIntervalMs / 2U) : currentCommand_.cycleMs;
  const WavePhaseLayout layout = barWaveLayout(currentCommand_, phraseDurationMs);

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint32_t laneOffsetMs =
      (kLedCount <= 1U) ? 0U : (travelMs * static_cast<uint32_t>(i)) / static_cast<uint32_t>(kLedCount - 1U);
    uint32_t laneElapsedMs = elapsedPhraseMs + laneOffsetMs;
    if (laneElapsedMs > phraseDurationMs) {
      laneElapsedMs = phraseDurationMs;
    }
    const uint8_t phase = static_cast<uint8_t>((laneElapsedMs * 255UL) / phraseDurationMs);
    WavePixelState laneState = baseBarWaveState(currentCommand_, layout, phase);
    applyBarWaveModulation(currentCommand_, layout, now, i, phase, laneState);
    gStripLeds[i] = scaleColor(laneState.color, laneState.level);
  }
}

void RgbStripRenderer::renderBeatPulse(uint32_t now) {
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  uint32_t pulseDurationMs = (currentCommand_.cycleMs == 0) ? beatIntervalMs : currentCommand_.cycleMs;
  if (pulseDurationMs > beatIntervalMs) {
    pulseDurationMs = beatIntervalMs;
  }
  if (pulseDurationMs == 0) {
    pulseDurationMs = beatIntervalMs;
  }

  uint32_t elapsedMs = now - beatStartedAtMs_;
  if (elapsedMs > pulseDurationMs) {
    elapsedMs = pulseDurationMs;
  }

  const uint8_t phase = static_cast<uint8_t>((elapsedMs * 255UL) / pulseDurationMs);
  const uint8_t envelope = classicPulseEnvelope8(phase);
  const uint8_t colorMix = ease8InOutCubic(envelope);
  const uint8_t crestMix = pulseCrestMix8(phase);
  const uint8_t bodyLevel = mixLevel(
    currentCommand_.floorLevel,
    currentCommand_.baseLevel,
    envelope
  );
  const uint8_t finalLevel = mixLevel(
    bodyLevel,
    currentCommand_.peakLevel,
    crestMix
  );
  const CRGB bodyColor = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    colorMix
  );
  const CRGB pulseColor = blend(
    bodyColor,
    secondaryColor(currentCommand_),
    static_cast<uint8_t>(crestMix / 2U)
  );

  for (uint8_t i = 0; i < kLedCount; ++i) {
    gStripLeds[i] = scaleColor(pulseColor, finalLevel);
  }
}

void RgbStripRenderer::renderAccent(uint32_t now) {
  const uint8_t safeBeatsPerBar = (beatsPerBar_ == 0) ? 4U : beatsPerBar_;
  const uint8_t phraseBars = (currentCommand_.triggerEveryBars == 0) ? 2U : currentCommand_.triggerEveryBars;
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  const uint32_t phraseDurationMs =
    static_cast<uint32_t>(phraseBars) * static_cast<uint32_t>(safeBeatsPerBar) * beatIntervalMs;
  const uint8_t safeBeatInBar = (beatInBar_ == 0) ? 1U : beatInBar_;
  const uint8_t startBeat =
    (currentCommand_.triggerBeat == 0 || currentCommand_.triggerBeat > safeBeatsPerBar)
      ? 1U
      : currentCommand_.triggerBeat;
  const uint32_t totalPhraseBeats =
    static_cast<uint32_t>(phraseBars) * static_cast<uint32_t>(safeBeatsPerBar);
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0 ? 1U : currentBar_) - 1U) * static_cast<uint32_t>(safeBeatsPerBar) +
    static_cast<uint32_t>(safeBeatInBar - 1U);
  const uint32_t phraseBeatIndex =
    (absoluteBeatIndex + totalPhraseBeats - static_cast<uint32_t>(startBeat - 1U)) % totalPhraseBeats;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) {
    elapsedBeatMs = beatIntervalMs;
  }

  uint32_t elapsedPhraseMs = phraseBeatIndex * beatIntervalMs + elapsedBeatMs;
  if (elapsedPhraseMs > phraseDurationMs) {
    elapsedPhraseMs = phraseDurationMs;
  }

  const uint8_t phase = static_cast<uint8_t>((elapsedPhraseMs * 255UL) / phraseDurationMs);
  const uint8_t envelope = heartbeatEnvelope8(phase);
  const uint8_t level = mixLevel(
    currentCommand_.floorLevel,
    currentCommand_.peakLevel,
    envelope
  );
  const uint8_t colorMix = scale8(envelope, 120U);
  const CRGB pulseColor = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    colorMix
  );

  for (uint8_t i = 0; i < kLedCount; ++i) {
    gStripLeds[i] = scaleColor(pulseColor, level);
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
