#include "rgb_strip_renderer.h"

#include <FastLED.h>

namespace {

static constexpr uint8_t kRgbDataPinPrimary = 26;
static constexpr uint8_t kRgbDataPinSecondary = 32;
static constexpr uint8_t kRgbLedCount = 15;
static constexpr uint16_t kConnectPulseMs = 140;
static constexpr uint8_t kOverlayWhiteLevel = 128;

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

uint8_t smoothSegmentNoise8(uint32_t elapsedMs, uint32_t segmentMs, uint8_t salt) {
  const uint32_t safeSegmentMs = (segmentMs == 0U) ? 1U : segmentMs;
  const uint32_t segment = elapsedMs / safeSegmentMs;
  const uint32_t segmentOffset = elapsedMs % safeSegmentMs;
  const uint8_t from = hash8(segment + static_cast<uint32_t>(salt) * 131U);
  const uint8_t to = hash8(segment + 1U + static_cast<uint32_t>(salt) * 131U);
  const uint8_t mix = ease8InOutCubic(
    static_cast<uint8_t>((segmentOffset * 255UL) / safeSegmentMs)
  );
  return lerp8by8(from, to, mix);
}

uint8_t pendulumMix8(uint32_t elapsedMs, uint32_t swingMs, uint8_t salt) {
  const uint32_t safeSwingMs = (swingMs == 0U) ? 1U : swingMs;
  const uint32_t cycleMs = safeSwingMs * 2U;
  const uint32_t offsetMs =
    (static_cast<uint32_t>(hash8(static_cast<uint32_t>(salt) * 97U)) * cycleMs) / 255UL;
  const uint32_t cycleOffsetMs = (elapsedMs + offsetMs) % cycleMs;
  const uint8_t phase = static_cast<uint8_t>((cycleOffsetMs * 255UL) / cycleMs);
  const uint8_t triangle =
    (phase < 128U) ? static_cast<uint8_t>(phase * 2U) : static_cast<uint8_t>((255U - phase) * 2U);
  return ease8InOutCubic(triangle);
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

bool isScheduledBeat(const decaflash::RgbCommand& command,
                     uint32_t currentBar,
                     uint8_t beatInBar) {
  const uint8_t everyBars = (command.triggerEveryBars == 0U) ? 1U : command.triggerEveryBars;
  const bool matchingBar = (everyBars <= 1U) || ((currentBar % everyBars) == 0U);
  const bool matchingBeat = (command.triggerBeat == 0U) || (beatInBar == command.triggerBeat);
  return matchingBar && matchingBeat;
}

SurfaceModulationState buildSurfaceModulationState(uint32_t now) {
  static constexpr uint32_t kMacroSwingMs = 300000U;
  SurfaceModulationState state = {};
  state.active = true;
  const uint8_t activityPendulum = pendulumMix8(now, kMacroSwingMs, 5U);
  const uint8_t activityWobble = smoothSegmentNoise8(now + 5000U, 42000U, 17U);
  state.activity = mixLevel(
    64U,
    220U,
    mixLevel(activityPendulum, activityWobble, 38U)
  );

  const uint8_t shadowPendulum = pendulumMix8(now, kMacroSwingMs, 11U);
  const uint8_t shadowWobble = smoothSegmentNoise8(now, 36000U, 29U);
  state.shadowDepth = mixLevel(
    6U,
    76U,
    mixLevel(shadowPendulum, shadowWobble, 44U)
  );

  const uint8_t pocketPendulum = pendulumMix8(now, kMacroSwingMs, 23U);
  const uint8_t pocketWobble = smoothSegmentNoise8(now + 9000U, 50000U, 31U);
  state.pocketChance = mixLevel(
    2U,
    30U,
    mixLevel(pocketPendulum, pocketWobble, 36U)
  );

  const uint8_t coolPendulum = pendulumMix8(now, kMacroSwingMs, 37U);
  const uint8_t coolWobble = smoothSegmentNoise8(now + 17000U, 46000U, 41U);
  state.coolShift = mixLevel(
    2U,
    58U,
    mixLevel(coolPendulum, coolWobble, 32U)
  );

  const uint8_t colorPendulum = pendulumMix8(now, kMacroSwingMs, 53U);
  const uint8_t colorWobble = smoothSegmentNoise8(now + 23000U, 54000U, 47U);
  state.colorDrift = mixLevel(colorPendulum, colorWobble, 26U);
  return state;
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
  if (phase <= start || phase >= end) return 0U;
  if (phase < peak) return scale8(ease8InOutCubic(segmentMix8(phase, start, peak)), strength);
  return scale8(static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, peak, end))), strength);
}

uint8_t heartbeatEnvelope8(uint8_t phase) {
  const uint8_t firstHit = pulseWindow8(phase, 8U, 22U, 42U, 255U);
  const uint8_t secondHit = pulseWindow8(phase, 46U, 60U, 88U, 180U);
  const uint8_t tail = (phase < 60U) ? 0U : scale8(
    static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, 60U, 255U))), 88U);
  return max(firstHit, max(secondHit, tail));
}

uint8_t riserPulseEnvelope8(uint8_t phase) {
  // Four rising hits at the start of the technical bar, followed by the long
  // tail of the final hit. This is intentionally a row, not a heartbeat.
  const uint8_t first = pulseWindow8(phase, 4U, 10U, 18U, 64U);
  const uint8_t second = pulseWindow8(phase, 26U, 33U, 43U, 128U);
  const uint8_t third = pulseWindow8(phase, 52U, 60U, 72U, 191U);
  const uint8_t fourth = pulseWindow8(phase, 82U, 92U, 108U, 255U);
  const uint8_t tail = (phase < 92U) ? 0U : scale8(
    static_cast<uint8_t>(255U - ease8InOutCubic(segmentMix8(phase, 92U, 255U))), 168U);
  return max(first, max(second, max(third, max(fourth, tail))));
}

uint8_t bandLevel8(uint8_t positionPercent,
                   uint8_t widthPercent,
                   uint8_t edgePercent) {
  const uint8_t width = widthPercent > 100U ? 100U : widthPercent;
  if (width == 0U || positionPercent >= width) {
    return 0U;
  }

  // Edges are part of a band's stated width. With edge=0 a band is a hard
  // block; two 30% bands always leave 40% of the physical strip black.
  const uint8_t edge = min<uint8_t>(edgePercent, width / 2U);
  if (edge == 0U || (positionPercent >= edge && positionPercent < width - edge)) {
    return 255U;
  }

  if (positionPercent < edge) {
    return ease8InOutCubic(static_cast<uint8_t>((positionPercent * 255U) / edge));
  }

  const uint8_t edgeProgress = static_cast<uint8_t>(positionPercent - (width - edge));
  return static_cast<uint8_t>(255U - ease8InOutCubic(
    static_cast<uint8_t>((edgeProgress * 255U) / edge)
  ));
}

uint8_t centeredBandLevel8(uint8_t distancePercent,
                           uint8_t widthPercent,
                           uint8_t edgePercent) {
  const uint8_t halfWidth = widthPercent / 2U;
  if (halfWidth == 0U || distancePercent > halfWidth) {
    return 0U;
  }

  const uint8_t edge = min<uint8_t>(edgePercent, halfWidth);
  const uint8_t coreRadius = static_cast<uint8_t>(halfWidth - edge);
  if (edge == 0U || distancePercent <= coreRadius) {
    return 255U;
  }

  const uint8_t edgeProgress = static_cast<uint8_t>(distancePercent - coreRadius);
  return static_cast<uint8_t>(255U - ease8InOutCubic(
    static_cast<uint8_t>((edgeProgress * 255U) / edge)
  ));
}

WavePhaseLayout barWaveLayout(const decaflash::RgbCommand& command, uint32_t phraseDurationMs) {
  const uint8_t requestedFadeWidth = static_cast<uint8_t>(
    min<uint32_t>(
      120U,
      (static_cast<uint32_t>(command.fadeOutMs) * 255UL) / phraseDurationMs
    )
  );
  uint8_t whiteHoldWidth = static_cast<uint8_t>(
    (static_cast<uint32_t>(command.peakHoldMs) * 255UL) / phraseDurationMs
  );
  if (whiteHoldWidth > 10U) {
    whiteHoldWidth = 10U;
  }

  const uint8_t fadeWidth = requestedFadeWidth == 0U ? 27U : requestedFadeWidth;
  const uint8_t whiteRiseEnd = static_cast<uint8_t>(255U - fadeWidth - whiteHoldWidth);
  const uint8_t deepBlueEnd = static_cast<uint8_t>((static_cast<uint16_t>(whiteRiseEnd) * 42U) / 100U);
  const uint8_t lightBlueEnd = static_cast<uint8_t>((static_cast<uint16_t>(whiteRiseEnd) * 90U) / 100U);
  return {deepBlueEnd, lightBlueEnd, whiteRiseEnd, static_cast<uint8_t>(whiteRiseEnd + whiteHoldWidth)};
}

WavePixelState baseBarWaveState(
  const decaflash::RgbCommand& command,
  const WavePhaseLayout& layout,
  uint8_t phase
) {
  const CRGB deepBlue = primaryColor(command);
  const CRGB lightBlue = secondaryColor(command);
  const CRGB white = CRGB(148, 182, 232);
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
  uint8_t activity,
  WavePixelState& state
) {
  // This is the seam where future audio-driven modulation can hook in.
  const uint8_t flicker = pseudoFlicker(now, pixel);
  if (phase < layout.whiteRiseEnd) {
    const uint8_t lift = scale8(static_cast<uint8_t>(flicker / 4U), mixLevel(68U, 220U, activity));
    state.level = mixLevel(
      state.level,
      static_cast<uint8_t>(state.level > (255U - lift) ? 255U : state.level + lift),
      116U
    );
  }

  const bool nearCrest = phase >= static_cast<uint8_t>(layout.lightBlueEnd - 8U) &&
                         phase <= static_cast<uint8_t>(layout.whiteHoldEnd + 10U);
  const uint8_t sparkleSeed = hash8((now / 65U) + static_cast<uint32_t>(pixel * 37U) + phase);
  if (nearCrest && sparkleSeed > mixLevel(252U, 236U, activity)) {
    state.color = blend(state.color, CRGB::White, mixLevel(74U, 132U, activity));
    state.level = mixLevel(state.level, command.peakLevel, mixLevel(120U, 204U, activity));
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
  pulseRowStartedAtMs_ = 0;
  pulseRowEndsAtMs_ = 0;
  beatStartedAtMs_ = effectStartedAtMs_;
}

void RgbStripRenderer::setCommand(const decaflash::RgbCommand& command) {
  currentCommand_ = command;
  effectStartedAtMs_ = millis();
  pulseRowStartedAtMs_ = 0;
  pulseRowEndsAtMs_ = 0;
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

void RgbStripRenderer::setLit(bool lit) {
  if (!initialized_) {
    begin();
  }

  if (lit) {
    renderSolid(kOverlayWhiteLevel, kOverlayWhiteLevel, kOverlayWhiteLevel);
    return;
  }

  fill_solid(gStripLeds, kLedCount, CRGB::Black);
  FastLED.show();
}

void RgbStripRenderer::triggerPulseRow() {
  const uint32_t now = millis();
  const uint8_t pulseCount = (currentCommand_.pulseCount == 0U) ? 1U : currentCommand_.pulseCount;
  const uint16_t pulseDurationMs =
    (currentCommand_.durationMs == 0U) ? 80U : currentCommand_.durationMs;
  const uint32_t rowDurationMs =
    static_cast<uint32_t>(pulseCount) * pulseDurationMs +
    static_cast<uint32_t>(pulseCount - 1U) * currentCommand_.pulseGapMs;
  pulseRowStartedAtMs_ = now + currentCommand_.startOffsetMs;
  pulseRowEndsAtMs_ = pulseRowStartedAtMs_ + rowDurationMs;
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
    case decaflash::RgbPattern::Wave:
      renderWave(now);
      break;

    case decaflash::RgbPattern::Pulse:
      renderPulse(now);
      break;

    case decaflash::RgbPattern::PulseRow:
      renderPulseRow(now);
      break;

    case decaflash::RgbPattern::Heartbeat:
      renderHeartbeat(now);
      break;

    case decaflash::RgbPattern::RiserPulse:
      renderRiserPulse(now);
      break;

    case decaflash::RgbPattern::Runner:
      renderRunner(now);
      break;

    case decaflash::RgbPattern::Off:
    default:
      fill_solid(gStripLeds, kLedCount, CRGB::Black);
      break;
  }

  applySurfaceModulation(now);
  FastLED.show();
}

SurfaceModulationState RgbStripRenderer::surfaceModulationState(uint32_t now) const {
  if (currentCommand_.pattern == decaflash::RgbPattern::Off) {
    return {};
  }

  return buildSurfaceModulationState(now);
}

void RgbStripRenderer::renderSolid(uint8_t red, uint8_t green, uint8_t blue) {
  fill_solid(gStripLeds, kLedCount, CRGB(red, green, blue));
  FastLED.show();
}

void RgbStripRenderer::renderWave(uint32_t now) {
  const uint8_t safeBeatsPerBar = (beatsPerBar_ == 0) ? 4U : beatsPerBar_;
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  const uint8_t fallbackCycleBeats = static_cast<uint8_t>(
    ((currentCommand_.triggerEveryBars == 0) ? 1U : currentCommand_.triggerEveryBars) * safeBeatsPerBar
  );
  const uint8_t cycleBeats = currentCommand_.waveCycleBeats == 0U
    ? fallbackCycleBeats : currentCommand_.waveCycleBeats;
  const uint32_t phraseDurationMs =
    static_cast<uint32_t>(cycleBeats) * beatIntervalMs;
  const uint8_t safeBeatInBar = (beatInBar_ == 0) ? 1U : beatInBar_;
  const uint8_t startBeat =
    (currentCommand_.triggerBeat == 0 || currentCommand_.triggerBeat > safeBeatsPerBar)
      ? 1U
      : currentCommand_.triggerBeat;
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0 ? 1U : currentBar_) - 1U) * static_cast<uint32_t>(safeBeatsPerBar) +
    static_cast<uint32_t>(safeBeatInBar - 1U);
  const uint32_t phraseBeatIndex =
    (absoluteBeatIndex + cycleBeats - static_cast<uint32_t>(startBeat - 1U)) % cycleBeats;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) {
    elapsedBeatMs = beatIntervalMs;
  }

  uint32_t elapsedPhraseMs = phraseBeatIndex * beatIntervalMs + elapsedBeatMs;
  if (elapsedPhraseMs > phraseDurationMs) {
    elapsedPhraseMs = phraseDurationMs;
  }

  const uint32_t travelMs =
    (currentCommand_.durationMs == 0) ? (beatIntervalMs / 2U) : currentCommand_.durationMs;
  const WavePhaseLayout layout = barWaveLayout(currentCommand_, phraseDurationMs);
  const SurfaceModulationState modulation = surfaceModulationState(now);

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint32_t laneOffsetMs =
      (kLedCount <= 1U) ? 0U : (travelMs * static_cast<uint32_t>(i)) / static_cast<uint32_t>(kLedCount - 1U);
    uint32_t laneElapsedMs = elapsedPhraseMs + laneOffsetMs;
    if (laneElapsedMs > phraseDurationMs) {
      laneElapsedMs = phraseDurationMs;
    }
    const uint8_t phase = static_cast<uint8_t>((laneElapsedMs * 255UL) / phraseDurationMs);
    WavePixelState laneState = baseBarWaveState(currentCommand_, layout, phase);
    applyBarWaveModulation(currentCommand_, layout, now, i, phase, modulation.activity, laneState);
    gStripLeds[i] = scaleColor(laneState.color, laneState.level);
  }
}

void RgbStripRenderer::renderPulse(uint32_t now) {
  if (!isScheduledBeat(currentCommand_, currentBar_, beatInBar_)) {
    const CRGB idle = scaleColor(primaryColor(currentCommand_), currentCommand_.floorLevel);
    fill_solid(gStripLeds, kLedCount, idle);
    return;
  }

  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  uint32_t pulseDurationMs =
    (currentCommand_.durationMs == 0) ? beatIntervalMs : currentCommand_.durationMs;
  if (pulseDurationMs > beatIntervalMs) {
    pulseDurationMs = beatIntervalMs;
  }
  if (pulseDurationMs == 0) {
    pulseDurationMs = beatIntervalMs;
  }

  uint32_t elapsedMs = now - beatStartedAtMs_;
  if (elapsedMs < currentCommand_.startOffsetMs) {
    const CRGB idle = scaleColor(primaryColor(currentCommand_), currentCommand_.floorLevel);
    fill_solid(gStripLeds, kLedCount, idle);
    return;
  }
  elapsedMs -= currentCommand_.startOffsetMs;
  if (elapsedMs > pulseDurationMs) {
    elapsedMs = pulseDurationMs;
  }

  const uint8_t phase = static_cast<uint8_t>((elapsedMs * 255UL) / pulseDurationMs);
  const SurfaceModulationState modulation = surfaceModulationState(now);
  const uint8_t envelope = classicPulseEnvelope8(phase);
  const uint8_t colorMix = ease8InOutCubic(envelope);
  const uint8_t crestMix = scale8(
    pulseCrestMix8(phase),
    mixLevel(92U, 224U, modulation.activity)
  );
  const uint8_t bodyLevel = mixLevel(
    currentCommand_.floorLevel,
    currentCommand_.baseLevel,
    scale8(envelope, mixLevel(208U, 255U, modulation.activity))
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

void RgbStripRenderer::renderPulseRow(uint32_t now) {
  const CRGB idle = scaleColor(primaryColor(currentCommand_), currentCommand_.floorLevel);
  if (pulseRowStartedAtMs_ == 0 || now < pulseRowStartedAtMs_ || now >= pulseRowEndsAtMs_) {
    fill_solid(gStripLeds, kLedCount, idle);
    return;
  }

  const uint16_t pulseDurationMs =
    (currentCommand_.durationMs == 0U) ? 80U : currentCommand_.durationMs;
  const uint32_t elapsedMs = now - pulseRowStartedAtMs_;
  const uint32_t pulseSlotMs = static_cast<uint32_t>(pulseDurationMs) + currentCommand_.pulseGapMs;
  const uint8_t pulseIndex = static_cast<uint8_t>(elapsedMs / pulseSlotMs);
  const uint16_t pulseElapsedMs = static_cast<uint16_t>(elapsedMs % pulseSlotMs);
  if (pulseIndex >= currentCommand_.pulseCount || pulseElapsedMs >= pulseDurationMs) {
    fill_solid(gStripLeds, kLedCount, idle);
    return;
  }

  const uint8_t phase = static_cast<uint8_t>((static_cast<uint32_t>(pulseElapsedMs) * 255UL) / pulseDurationMs);
  const uint8_t envelope = classicPulseEnvelope8(phase);
  const uint8_t pulsePeak =
    (pulseIndex == 0U)
      ? currentCommand_.peakLevel
      : scale8(currentCommand_.peakLevel, currentCommand_.subsequentPulseLevel);
  const uint8_t level = mixLevel(currentCommand_.floorLevel, pulsePeak, envelope);
  const CRGB color = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    envelope
  );
  for (uint8_t i = 0; i < kLedCount; ++i) {
    gStripLeds[i] = scaleColor(color, level);
  }
}

void RgbStripRenderer::renderHeartbeat(uint32_t now) {
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0U) ? 500U : beatIntervalMs_;
  const uint8_t safeBeatInBar = (beatInBar_ == 0U) ? 1U : beatInBar_;
  const uint8_t startBeat = (currentCommand_.triggerBeat == 0U || currentCommand_.triggerBeat > 4U)
    ? 1U : currentCommand_.triggerBeat;
  const uint32_t phraseDurationMs = 4U * beatIntervalMs;
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0U ? 1U : currentBar_) - 1U) * static_cast<uint32_t>(beatsPerBar_ == 0U ? 4U : beatsPerBar_) +
    static_cast<uint32_t>(safeBeatInBar - 1U);
  const uint32_t phraseBeatIndex =
    (absoluteBeatIndex + 4U - static_cast<uint32_t>(startBeat - 1U)) % 4U;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) elapsedBeatMs = beatIntervalMs;
  const uint32_t elapsedPhraseMs = min(phraseBeatIndex * beatIntervalMs + elapsedBeatMs, phraseDurationMs);
  const uint8_t phase = static_cast<uint8_t>((elapsedPhraseMs * 255UL) / phraseDurationMs);
  const SurfaceModulationState modulation = surfaceModulationState(now);
  const uint8_t envelope = scale8(
    heartbeatEnvelope8(phase),
    mixLevel(190U, 255U, modulation.activity)
  );
  const uint8_t level = mixLevel(currentCommand_.floorLevel, currentCommand_.peakLevel, envelope);
  const CRGB color = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    scale8(envelope, mixLevel(92U, 148U, modulation.activity))
  );
  fill_solid(gStripLeds, kLedCount, scaleColor(color, level));
}

void RgbStripRenderer::renderRiserPulse(uint32_t now) {
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0U) ? 500U : beatIntervalMs_;
  const uint8_t safeBeatInBar = (beatInBar_ == 0U) ? 1U : beatInBar_;
  const uint8_t startBeat = (currentCommand_.triggerBeat == 0U || currentCommand_.triggerBeat > 4U)
    ? 1U : currentCommand_.triggerBeat;
  const uint32_t phraseDurationMs = 4U * beatIntervalMs;
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0U ? 1U : currentBar_) - 1U) * static_cast<uint32_t>(beatsPerBar_ == 0U ? 4U : beatsPerBar_) +
    static_cast<uint32_t>(safeBeatInBar - 1U);
  const uint32_t phraseBeatIndex =
    (absoluteBeatIndex + 4U - static_cast<uint32_t>(startBeat - 1U)) % 4U;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) elapsedBeatMs = beatIntervalMs;
  const uint32_t elapsedPhraseMs = min(phraseBeatIndex * beatIntervalMs + elapsedBeatMs, phraseDurationMs);
  const uint8_t phase = static_cast<uint8_t>((elapsedPhraseMs * 255UL) / phraseDurationMs);
  const SurfaceModulationState modulation = surfaceModulationState(now);
  const uint8_t envelope = scale8(
    riserPulseEnvelope8(phase),
    mixLevel(210U, 255U, modulation.activity)
  );
  const uint8_t level = mixLevel(currentCommand_.floorLevel, currentCommand_.peakLevel, envelope);
  const CRGB color = blend(
    primaryColor(currentCommand_),
    secondaryColor(currentCommand_),
    scale8(envelope, mixLevel(120U, 188U, modulation.activity))
  );
  fill_solid(gStripLeds, kLedCount, scaleColor(color, level));
}

void RgbStripRenderer::renderRunner(uint32_t now) {
  const uint32_t beatIntervalMs = (beatIntervalMs_ == 0) ? 500U : beatIntervalMs_;
  uint32_t elapsedBeatMs = now - beatStartedAtMs_;
  if (elapsedBeatMs > beatIntervalMs) {
    elapsedBeatMs = beatIntervalMs;
  }
  const uint8_t phase = static_cast<uint8_t>((elapsedBeatMs * 255UL) / beatIntervalMs);
  const uint8_t bandCount = currentCommand_.runnerBandCount > decaflash::kMaxRunnerBands
    ? decaflash::kMaxRunnerBands : currentCommand_.runnerBandCount;
  const bool loops = currentCommand_.runnerMotion == decaflash::RunnerMotion::Loop;
  const bool sequencesBands =
    currentCommand_.runnerPresentation == decaflash::RunnerPresentation::Sequence;
  const uint8_t safeBeatsPerBar = (beatsPerBar_ == 0U) ? 4U : beatsPerBar_;
  const uint8_t safeBeatInBar = (beatInBar_ == 0U) ? 1U : beatInBar_;
  const uint32_t absoluteBeatIndex =
    ((currentBar_ == 0U ? 1U : currentBar_) - 1U) * safeBeatsPerBar + (safeBeatInBar - 1U);
  const uint8_t loopProgressPercent = static_cast<uint8_t>((static_cast<uint16_t>(phase) * 100U) / 256U);
  const uint8_t activeSequenceBand = bandCount == 0U
    ? 0U : static_cast<uint8_t>(absoluteBeatIndex % bandCount);

  for (uint8_t i = 0; i < kLedCount; ++i) {
    const uint8_t pixelPositionPercent = static_cast<uint8_t>(
      (static_cast<uint16_t>(i) * 100U) / kLedCount
    );
    uint8_t strongestMix = 0U;
    uint8_t strongestBand = 0U;
    for (uint8_t band = 0; band < bandCount; ++band) {
      if (sequencesBands && band != activeSequenceBand) {
        continue;
      }
      const decaflash::RgbRunnerBand& configuredBand = currentCommand_.runnerBands[band];
      uint8_t mix = 0U;
      if (loops) {
        const uint8_t bandStartPercent = static_cast<uint8_t>(
          (loopProgressPercent + configuredBand.phasePercent) % 100U
        );
        const uint8_t positionInBandPercent = static_cast<uint8_t>(
          (pixelPositionPercent + 100U - bandStartPercent) % 100U
        );
        mix = bandLevel8(
          positionInBandPercent,
          configuredBand.widthPercent,
          configuredBand.edgePercent
        );
      } else {
        const uint16_t bandOffset =
          (static_cast<uint16_t>(configuredBand.phasePercent) * 510U) / 100U;
        const uint16_t bouncePhase = static_cast<uint16_t>(
          (absoluteBeatIndex * 255UL + phase + bandOffset) % 510U
        );
        const uint8_t centerPercent = bouncePhase <= 255U
          ? static_cast<uint8_t>(bouncePhase)
          : static_cast<uint8_t>(510U - bouncePhase);
        const uint8_t centerPositionPercent = static_cast<uint8_t>(
          (static_cast<uint16_t>(centerPercent) * 100U) / 255U
        );
        const uint8_t distancePercent = pixelPositionPercent > centerPositionPercent
          ? static_cast<uint8_t>(pixelPositionPercent - centerPositionPercent)
          : static_cast<uint8_t>(centerPositionPercent - pixelPositionPercent);
        mix = centeredBandLevel8(
          distancePercent,
          configuredBand.widthPercent,
          configuredBand.edgePercent
        );
      }
      if (mix > strongestMix) {
        strongestMix = mix;
        strongestBand = band;
      }
    }

    const uint8_t level = mixLevel(currentCommand_.floorLevel, currentCommand_.peakLevel, strongestMix);
    const decaflash::RgbRunnerBand& strongestConfiguredBand =
      currentCommand_.runnerBands[strongestBand];
    const CRGB bandColor = CRGB(
      strongestConfiguredBand.r,
      strongestConfiguredBand.g,
      strongestConfiguredBand.b
    );
    gStripLeds[i] = scaleColor(bandColor, level);
  }
}

void RgbStripRenderer::applySurfaceModulation(uint32_t now) {
  const SurfaceModulationState modulation = surfaceModulationState(now);
  if (!modulation.active) {
    return;
  }

  const uint8_t shadowDepth = modulation.shadowDepth;
  const uint8_t pocketChance = modulation.pocketChance;
  const uint8_t coolShift = modulation.coolShift;
  const uint8_t colorDrift = modulation.colorDrift;
  const CRGB shadowBlue = blend(CRGB(0, 8, 24), CRGB(0, 18, 64), modulation.coolShift);
  const CRGB deepBlueTint = CRGB(0, 18, 88);
  const CRGB indigoMagentaTint = CRGB(68, 0, 92);
  const CRGB colorTint = blend(deepBlueTint, indigoMagentaTint, colorDrift);
  const uint8_t distanceFromCenter =
    (colorDrift > 127U)
      ? static_cast<uint8_t>((colorDrift - 127U) * 2U)
      : static_cast<uint8_t>((127U - colorDrift) * 2U);
  const uint8_t colorDriftLimit =
    (currentCommand_.pattern == decaflash::RgbPattern::PulseRow ||
     currentCommand_.pattern == decaflash::RgbPattern::Heartbeat ||
     currentCommand_.pattern == decaflash::RgbPattern::RiserPulse) ? 20U : 60U;
  const uint8_t colorDriftMix = scale8(distanceFromCenter, colorDriftLimit);

  for (uint8_t i = 0; i < kLedCount; ++i) {
    if (gStripLeds[i].r == 0U && gStripLeds[i].g == 0U && gStripLeds[i].b == 0U) {
      continue;
    }

    const uint8_t shadowSeed = hash8((now / 48U) + static_cast<uint32_t>(i * 47U));
    const uint8_t shadowMix = scale8(ease8InOutCubic(shadowSeed), shadowDepth);
    const uint8_t levelScale = static_cast<uint8_t>(255U - mixLevel(0U, shadowMix, 180U));
    gStripLeds[i].nscale8_video(levelScale);

    const uint8_t tintSeed = hash8((now / 88U) + static_cast<uint32_t>(i * 59U) + 17U);
    const uint8_t tintMix = scale8(ease8InOutCubic(tintSeed), static_cast<uint8_t>(coolShift / 2U));
    gStripLeds[i] = blend(gStripLeds[i], shadowBlue, tintMix);
    gStripLeds[i] = blend(gStripLeds[i], colorTint, colorDriftMix);

    const uint8_t pocketSeed = hash8((now / 110U) + static_cast<uint32_t>(i * 61U) + 91U);
    if (pocketSeed > static_cast<uint8_t>(255U - pocketChance)) {
      gStripLeds[i].nscale8_video(mixLevel(124U, 76U, shadowDepth));
    }
  }
}
