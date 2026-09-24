#pragma once

#include "decaflash_types.h"

namespace decaflash::scenes {

struct SceneDefinition {
  const char* name;
  FlashCommand flash;
  RgbCommand wash;
  RgbCommand pulse;
  RgbCommand accent;
  RgbCommand flicker;
};

namespace detail {

struct RgbColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
};

struct RgbColors {
  RgbColor primary;
  RgbColor secondary;
};

struct RgbLevels {
  uint8_t floor;
  uint8_t base;
  uint8_t peak;
};

// A schedule is deliberately based on the Brain's technical four-beat bar.
// It does not claim to identify the musical downbeat of a song.
struct BeatSchedule {
  uint8_t beat;
  uint8_t everyBars;
  uint16_t offsetMs;

  BeatSchedule everyBar(uint8_t bars) const {
    return {beat, static_cast<uint8_t>(bars == 0U ? 1U : bars), offsetMs};
  }

  BeatSchedule after(uint16_t milliseconds) const {
    return {beat, everyBars, milliseconds};
  }
};

inline BeatSchedule onBeat(uint8_t beat) {
  return {beat, 1, 0};
}

inline BeatSchedule onEveryBeat() {
  return {0, 1, 0};
}

struct WaveSpan {
  uint8_t cycleBeats;
  uint8_t startBeat;

  WaveSpan startsOnBeat(uint8_t beat) const {
    return {cycleBeats, beat};
  }
};

inline WaveSpan overBars(uint8_t bars) {
  const uint8_t safeBars = bars == 0U ? 1U : bars;
  return {static_cast<uint8_t>(safeBars > 63U ? 252U : safeBars * 4U), 1};
}

inline WaveSpan overBeats(uint8_t beats) {
  return {static_cast<uint8_t>(beats == 0U ? 1U : beats), 1};
}

struct PulseRow {
  uint8_t count;
  uint16_t pulseDurationMs;
  uint16_t gapMs;
  uint8_t subsequentPulseLevel;
};

struct RunnerBands {
  RunnerPresentation presentation;
  uint8_t bandCount;
  RgbRunnerBand bands[kMaxRunnerBands];
};

inline RgbRunnerBand band(RgbColor color,
                          uint8_t widthPercent,
                          uint8_t phasePercent,
                          uint8_t edgePercent = 7U) {
  return {
    color.r,
    color.g,
    color.b,
    static_cast<uint8_t>(widthPercent > 100U ? 100U : widthPercent),
    static_cast<uint8_t>(phasePercent > 100U ? 100U : phasePercent),
    static_cast<uint8_t>(edgePercent > 100U ? 100U : edgePercent),
  };
}

inline RunnerBands runnerBands(RunnerPresentation presentation,
                               RgbRunnerBand first,
                               RgbRunnerBand second = {},
                               RgbRunnerBand third = {},
                               RgbRunnerBand fourth = {}) {
  const uint8_t bandCount = (fourth.widthPercent != 0U) ? 4U
    : (third.widthPercent != 0U) ? 3U
    : (second.widthPercent != 0U) ? 2U
    : (first.widthPercent != 0U) ? 1U
    : 0U;
  return {presentation, bandCount, {first, second, third, fourth}};
}

inline constexpr RunnerMotion bounce() {
  return RunnerMotion::Bounce;
}

inline constexpr RunnerMotion loop() {
  return RunnerMotion::Loop;
}

inline RunnerBands parallel(RgbRunnerBand first,
                            RgbRunnerBand second = {},
                            RgbRunnerBand third = {},
                            RgbRunnerBand fourth = {}) {
  return runnerBands(RunnerPresentation::Parallel, first, second, third, fourth);
}

// Each band takes one full pass in turn. It is for effects such as a rotating
// red/blue siren, where colours should not appear at the same time.
inline RunnerBands sequence(RgbRunnerBand first,
                            RgbRunnerBand second,
                            RgbRunnerBand third = {},
                            RgbRunnerBand fourth = {}) {
  return runnerBands(RunnerPresentation::Sequence, first, second, third, fourth);
}

inline RgbColors colors(RgbColor primary, RgbColor secondary) {
  return {primary, secondary};
}

inline RgbLevels brightness(uint8_t floor, uint8_t base, uint8_t peak) {
  return {floor, base, peak};
}

inline PulseRow pulseRow(uint8_t count,
                          uint16_t pulseDurationMs,
                          uint16_t gapMs,
                          uint8_t subsequentPulseLevel = 255) {
  return {
    static_cast<uint8_t>(count == 0U ? 1U : count),
    pulseDurationMs,
    gapMs,
    subsequentPulseLevel,
  };
}

inline PulseRow heartbeat() {
  return pulseRow(2, 115, 85, 180);
}

inline PulseRow triplePulse(uint16_t gapMs = 110) {
  return pulseRow(3, 48, gapMs);
}

inline void copyCommandName(char* destination, const char* source) {
  size_t index = 0;
  while (source != nullptr && source[index] != '\0' && index + 1U < kCommandNameLength) {
    destination[index] = source[index];
    ++index;
  }

  while (index < kCommandNameLength) {
    destination[index++] = '\0';
  }
}

inline FlashCommand flashProfileCommand(
  const char* name,
  uint8_t variationWindowBars,
  uint16_t profileSeed,
  uint8_t pulseWeight,
  uint8_t slowPulseWeight,
  uint8_t doublePulseWeight,
  uint8_t quadPulseWeight,
  uint8_t riserWeight
) {
  FlashCommand command = {};
  copyCommandName(command.name, name);
  command.mode = FlashCommandMode::VariationProfile;
  command.variationWindowBars = variationWindowBars;
  command.profileSeed = profileSeed;
  command.pulseWeight = pulseWeight;
  command.slowPulseWeight = slowPulseWeight;
  command.doublePulseWeight = doublePulseWeight;
  command.quadPulseWeight = quadPulseWeight;
  command.riserWeight = riserWeight;
  return command;
}

// Flashlight and RGB use different hardware renderers, but the scene file uses
// the same vocabulary. A flash mix decides which local pulse motif a flashlight
// uses for a few technical bars; it does not add radio traffic or state.
namespace flash {

enum class MotifKind : uint8_t {
  None = 0,
  Pulse = 1,
  SlowPulse = 2,
  PulseRow2 = 3,
  PulseRow4 = 4,
  Riser = 5,
};

struct MotifWeight {
  constexpr MotifWeight(MotifKind selectedKind = MotifKind::None,
                        uint8_t selectedWeight = 0)
    : kind(selectedKind), weight(selectedWeight) {}

  MotifKind kind;
  uint8_t weight;
};

struct Mix {
  uint8_t pulseWeight;
  uint8_t slowPulseWeight;
  uint8_t pulseRow2Weight;
  uint8_t pulseRow4Weight;
  uint8_t riserWeight;
};

inline MotifWeight pulse(uint8_t weight) {
  return {MotifKind::Pulse, weight};
}

inline MotifWeight slowPulse(uint8_t weight) {
  return {MotifKind::SlowPulse, weight};
}

inline MotifWeight doublePulse(uint8_t weight) {
  return {MotifKind::PulseRow2, weight};
}

inline MotifWeight quadPulse(uint8_t weight) {
  return {MotifKind::PulseRow4, weight};
}

inline MotifWeight riser(uint8_t weight) {
  return {MotifKind::Riser, weight};
}

inline void add(Mix& mix, MotifWeight motif) {
  switch (motif.kind) {
    case MotifKind::None:
      break;
    case MotifKind::Pulse:
      mix.pulseWeight = motif.weight;
      break;
    case MotifKind::SlowPulse:
      mix.slowPulseWeight = motif.weight;
      break;
    case MotifKind::PulseRow2:
      mix.pulseRow2Weight = motif.weight;
      break;
    case MotifKind::PulseRow4:
      mix.pulseRow4Weight = motif.weight;
      break;
    case MotifKind::Riser:
      mix.riserWeight = motif.weight;
      break;
  }
}

inline Mix mix(
  MotifWeight first,
  MotifWeight second = {},
  MotifWeight third = {},
  MotifWeight fourth = {},
  MotifWeight fifth = {}
) {
  Mix result = {};
  add(result, first);
  add(result, second);
  add(result, third);
  add(result, fourth);
  add(result, fifth);
  return result;
}

inline FlashCommand variation(
  const char* name,
  uint8_t everyBars,
  uint16_t seed,
  Mix motifs
) {
  return flashProfileCommand(
    name,
    everyBars,
    seed,
    motifs.pulseWeight,
    motifs.slowPulseWeight,
    motifs.pulseRow2Weight,
    motifs.pulseRow4Weight,
    motifs.riserWeight
  );
}

}  // namespace flash

inline FlashRenderCommand flashRenderCommand(
  const char* name,
  FlashPattern pattern,
  uint8_t everyBars,
  uint8_t beat,
  uint8_t burstCount,
  uint16_t burstIntervalMs,
  int16_t burstIntervalStepMs,
  uint16_t flashDurationMs
) {
  FlashRenderCommand command = {};
  copyCommandName(command.name, name);
  command.pattern = pattern;
  command.triggerEveryBars = everyBars;
  command.triggerBeat = beat;
  command.burstCount = burstCount;
  command.burstIntervalMs = burstIntervalMs;
  command.burstIntervalStepMs = burstIntervalStepMs;
  command.flashDurationMs = flashDurationMs;
  return command;
}

inline RgbCommand makeRgbCommand(
  const char* name,
  RgbPattern pattern,
  RgbColors colorSet,
  RgbLevels levels,
  BeatSchedule schedule,
  uint16_t durationMs = 0,
  uint16_t peakHoldMs = 0,
  PulseRow row = pulseRow(1, 0, 0),
  uint8_t waveCycleBeats = 0,
  uint16_t fadeOutMs = 0
) {
  RgbCommand command = {};
  copyCommandName(command.name, name);
  command.pattern = pattern;
  command.primaryR = colorSet.primary.r;
  command.primaryG = colorSet.primary.g;
  command.primaryB = colorSet.primary.b;
  command.secondaryR = colorSet.secondary.r;
  command.secondaryG = colorSet.secondary.g;
  command.secondaryB = colorSet.secondary.b;
  command.floorLevel = levels.floor;
  command.baseLevel = levels.base;
  command.peakLevel = levels.peak;
  command.triggerEveryBars = schedule.everyBars;
  command.triggerBeat = schedule.beat;
  command.startOffsetMs = schedule.offsetMs;
  command.durationMs = durationMs;
  command.peakHoldMs = peakHoldMs;
  command.fadeOutMs = fadeOutMs;
  command.waveCycleBeats = waveCycleBeats;
  command.pulseCount = row.count;
  command.pulseGapMs = row.gapMs;
  command.subsequentPulseLevel = row.subsequentPulseLevel;
  command.runnerMotion = RunnerMotion::Bounce;
  command.runnerPresentation = RunnerPresentation::Parallel;
  command.runnerBandCount = 0;
  return command;
}

inline RgbCommand wave(const char* name,
                       RgbColors colorSet,
                       RgbLevels levels,
                       WaveSpan span,
                       uint16_t travelMs,
                       uint16_t peakHoldMs,
                       uint16_t fadeOutMs = 0) {
  return makeRgbCommand(name,
                        RgbPattern::Wave,
                        colorSet,
                        levels,
                        onBeat(span.startBeat),
                        travelMs,
                        peakHoldMs,
                        pulseRow(1, 0, 0),
                        span.cycleBeats,
                        fadeOutMs);
}

inline RgbCommand pulse(const char* name,
                        RgbColors colorSet,
                        RgbLevels levels,
                        BeatSchedule schedule,
                        uint16_t durationMs) {
  return makeRgbCommand(name,
                        RgbPattern::Pulse,
                        colorSet,
                        levels,
                        schedule,
                        durationMs);
}

inline RgbCommand pulseRow(const char* name,
                           RgbColors colorSet,
                           RgbLevels levels,
                           BeatSchedule schedule,
                           PulseRow row) {
  return makeRgbCommand(name,
                        RgbPattern::PulseRow,
                        colorSet,
                        levels,
                        schedule,
                        row.pulseDurationMs,
                        0,
                        row);
}

inline RgbCommand heartbeat(const char* name,
                            RgbColors colorSet,
                            RgbLevels levels,
                            BeatSchedule schedule) {
  return makeRgbCommand(name,
                        RgbPattern::Heartbeat,
                        colorSet,
                        levels,
                        schedule);
}

inline RgbCommand riserPulse(const char* name,
                             RgbColors colorSet,
                             RgbLevels levels,
                             BeatSchedule schedule) {
  return makeRgbCommand(name,
                        RgbPattern::RiserPulse,
                        colorSet,
                        levels,
                        schedule);
}

inline RgbCommand runner(const char* name,
                         RgbLevels levels,
                         RunnerMotion motion,
                         RunnerBands configuredBands) {
  const uint8_t bandCount = configuredBands.bandCount > kMaxRunnerBands
    ? kMaxRunnerBands : configuredBands.bandCount;
  const RgbRunnerBand firstBand = bandCount == 0U ? RgbRunnerBand{} : configuredBands.bands[0];
  const RgbRunnerBand secondBand = bandCount < 2U ? firstBand : configuredBands.bands[1];
  RgbCommand command = makeRgbCommand(name,
                        RgbPattern::Runner,
                        colors(
                          {firstBand.r, firstBand.g, firstBand.b},
                          {secondBand.r, secondBand.g, secondBand.b}
                        ),
                        levels,
                        onEveryBeat());
  command.runnerMotion = motion;
  command.runnerPresentation = configuredBands.presentation;
  command.runnerBandCount = bandCount;
  for (uint8_t index = 0; index < bandCount; ++index) {
    command.runnerBands[index] = configuredBands.bands[index];
  }
  return command;
}

inline uint32_t mixSeed(uint32_t value) {
  value ^= value >> 16;
  value *= 0x7feb352dUL;
  value ^= value >> 15;
  value *= 0x846ca68bUL;
  value ^= value >> 16;
  return value;
}

inline uint32_t flashSeed(const FlashCommand& command, uint32_t variationEpoch, uint32_t salt) {
  return mixSeed(
    static_cast<uint32_t>(command.profileSeed) ^
    static_cast<uint32_t>((variationEpoch + 1U) * 0x9E3779B9UL) ^
    salt
  );
}

inline uint32_t rangeUint32(uint32_t seed, uint32_t minimum, uint32_t maximum) {
  if (minimum >= maximum) {
    return minimum;
  }

  return minimum + (seed % (maximum - minimum + 1U));
}

inline int32_t rangeInt32(uint32_t seed, int32_t minimum, int32_t maximum) {
  if (minimum >= maximum) {
    return minimum;
  }

  const uint32_t span = static_cast<uint32_t>(maximum - minimum + 1);
  return minimum + static_cast<int32_t>(seed % span);
}

inline uint32_t variationEpochFor(const FlashCommand& command, uint32_t currentBar) {
  const uint32_t safeWindowBars =
    (command.variationWindowBars == 0U) ? 1U : static_cast<uint32_t>(command.variationWindowBars);
  const uint32_t zeroBasedBar = (currentBar == 0U) ? 0U : (currentBar - 1U);
  return zeroBasedBar / safeWindowBars;
}

enum class FlashMotif : uint8_t {
  Pulse = 0,
  SlowPulse = 1,
  DoublePulse = 2,
  QuadPulse = 3,
  Riser = 4,
};

inline FlashMotif pickFlashMotif(const FlashCommand& command, uint32_t variationEpoch) {
  const uint16_t totalWeight =
    static_cast<uint16_t>(command.pulseWeight) +
    static_cast<uint16_t>(command.slowPulseWeight) +
    static_cast<uint16_t>(command.doublePulseWeight) +
    static_cast<uint16_t>(command.quadPulseWeight) +
    static_cast<uint16_t>(command.riserWeight);

  if (totalWeight == 0U) {
    return FlashMotif::Pulse;
  }

  const uint32_t roll = flashSeed(command, variationEpoch, 0xA341316CUL) % totalWeight;
  uint16_t cursor = command.pulseWeight;
  if (roll < cursor) {
    return FlashMotif::Pulse;
  }

  cursor = static_cast<uint16_t>(cursor + command.slowPulseWeight);
  if (roll < cursor) {
    return FlashMotif::SlowPulse;
  }

  cursor = static_cast<uint16_t>(cursor + command.doublePulseWeight);
  if (roll < cursor) {
    return FlashMotif::DoublePulse;
  }

  cursor = static_cast<uint16_t>(cursor + command.quadPulseWeight);
  if (roll < cursor) {
    return FlashMotif::QuadPulse;
  }

  return FlashMotif::Riser;
}

inline FlashRenderCommand flashRenderCommandForMotif(
  FlashMotif motif,
  const FlashCommand& command,
  uint32_t variationEpoch
) {
  const uint32_t seed0 = flashSeed(command, variationEpoch, 0x1234567UL);
  const uint32_t seed1 = flashSeed(command, variationEpoch, 0x2345678UL);
  const uint32_t seed2 = flashSeed(command, variationEpoch, 0x3456789UL);
  const uint32_t seed3 = flashSeed(command, variationEpoch, 0x456789AUL);

  switch (motif) {
    case FlashMotif::SlowPulse:
      return flashRenderCommand(
        "Slow Pulse",
        FlashPattern::Pulse,
        (rangeUint32(seed1, 0, 1) == 0U) ? 2U : 4U,
        1,
        1,
        0,
        0,
        static_cast<uint16_t>(rangeUint32(seed0, 105, 135))
      );

    case FlashMotif::DoublePulse:
      return flashRenderCommand(
        "Double Pulse",
        FlashPattern::PulseRow,
        (rangeUint32(seed2, 0, 1) == 0U) ? 1U : 2U,
        1,
        2,
        static_cast<uint16_t>(rangeUint32(seed0, 330, 380)),
        0,
        static_cast<uint16_t>(rangeUint32(seed1, 60, 85))
      );

    case FlashMotif::QuadPulse:
      return flashRenderCommand(
        "Quad Pulse",
        FlashPattern::PulseRow,
        (rangeUint32(seed3, 0, 1) == 0U) ? 2U : 4U,
        1,
        4,
        static_cast<uint16_t>(rangeUint32(seed0, 260, 310)),
        static_cast<int16_t>(rangeInt32(seed1, -20, -10)),
        static_cast<uint16_t>(rangeUint32(seed2, 45, 60))
      );

    case FlashMotif::Riser:
      return flashRenderCommand(
        "Riser 5x",
        FlashPattern::PulseRow,
        (rangeUint32(seed0, 0, 1) == 0U) ? 2U : 4U,
        1,
        5,
        static_cast<uint16_t>(rangeUint32(seed1, 340, 410)),
        static_cast<int16_t>(rangeInt32(seed2, -40, -25)),
        static_cast<uint16_t>(rangeUint32(seed3, 40, 55))
      );

    case FlashMotif::Pulse:
    default:
      return flashRenderCommand(
        "Beat Pulse",
        FlashPattern::Pulse,
        1,
        (rangeUint32(seed2, 0, 1) == 0U) ? 0U : 1U,
        1,
        0,
        0,
        static_cast<uint16_t>(rangeUint32(seed0, 60, 85))
      );
  }
}

inline SceneDefinition makeScene1() {
  constexpr RgbColor kDeepBlue = {0, 24, 110};
  constexpr RgbColor kIceBlue = {92, 214, 255};
  constexpr RgbColor kPulseBlue = {0, 78, 255};
  constexpr RgbColor kHeartDarkRed = {84, 0, 6};
  constexpr RgbColor kHeartRed = {255, 18, 0};
  constexpr uint8_t kWashFloor = 0;
  constexpr uint8_t kWashBase = 18;
  constexpr uint8_t kWashPeak = 152;
  constexpr uint16_t kWashTravelMs = 380;
  constexpr uint16_t kWashWhiteHoldMs = 70;

  // Eine gemeinsame Szene fuer die ganze Node-Welt.
  // Flash lebt innerhalb dieser Szene ueber lokale Puls-Variationen.
  return {
    "szene 1",

    flash::variation(
      "Scene 1 Flash",
      12,
      17,
      flash::mix(
        flash::pulse(30),
        flash::slowPulse(5),
        flash::doublePulse(24),
        flash::quadPulse(20),
        flash::riser(5)
      )
    ),

    wave(
      "Scene 1 Wash",
      colors(kDeepBlue, kIceBlue),
      brightness(kWashFloor, kWashBase, kWashPeak),
      overBars(4).startsOnBeat(1),
      kWashTravelMs,
      kWashWhiteHoldMs
    ),

    pulse(
      "Scene 1 Pulse",
      colors(kPulseBlue, kPulseBlue),
      brightness(0, 86, 116),
      onEveryBeat(),
      380
    ),

    heartbeat(
      "Scene 1 Accent",
      colors(kHeartDarkRed, kHeartRed),
      brightness(0, 14, 176),
      onBeat(1)
    ),

    runner(
      "Scene 1 Flicker",
      brightness(0, 92, 176),
      bounce(),
      parallel(
        band(kDeepBlue, 24, 0),
        band(kPulseBlue, 24, 50)
      )
    ),
  };
}

inline SceneDefinition makeScene2() {
  // Wiederhergestellt aus dem frueheren gemeinsamen Szenenmodell:
  // "dynamisch" (Commit 07f4003). Der damalige Breathe-Renderer
  // existiert nicht mehr; BarWave ist sein heutiges, beatgebundenes
  // Gegenstueck. Die Farben, Pegel und Taktmuster sind sonst erhalten.
  constexpr RgbColor kWashPrimary = {5, 0, 45};
  constexpr RgbColor kWashSecondary = {0, 115, 175};
  constexpr RgbColor kSirenBlue = {0, 28, 150};
  constexpr RgbColor kSirenMagentaRed = {224, 0, 92};
  constexpr RgbColor kAccentPrimary = {14, 0, 0};
  constexpr RgbColor kAccentSecondary = {255, 12, 0};
  constexpr RgbColor kFlickerPrimary = {0, 96, 210};
  constexpr RgbColor kFlickerSecondary = {255, 0, 70};

  return {
    "dynamisch",

    // Das alte Modell verwendete 2er- und 3er-Bursts. Das aktuelle
    // Variationsprofil bildet diese mit Puls, Doppel- und Viererpuls ab.
    flash::variation(
      "Drive Flash",
      8,
      71,
      flash::mix(
        flash::pulse(118),
        flash::slowPulse(35),
        flash::doublePulse(92),
        flash::quadPulse(45),
        flash::riser(40)
      )
    ),

    wave(
      "Drive Electric Wash",
      colors(kWashPrimary, kWashSecondary),
      brightness(0, 18, 150),
      overBars(1).startsOnBeat(1),
      1100,
      40,
      900  // long fade to black
    ),

    runner(
      "Drive Siren",
      brightness(0, 42, 244),
      loop(),
      sequence(
        band(kSirenBlue, 50, 0, 0),
        band(kSirenMagentaRed, 50, 0, 0)
      )
    ),

    riserPulse(
      "Drive Riser",
      colors(kAccentPrimary, kAccentSecondary),
      brightness(0, 44, 255),
      onBeat(1)
    ),

    runner(
      "Drive Flicker",
      brightness(12, 92, 230),
      bounce(),
      parallel(
        band(kFlickerPrimary, 24, 0),
        band(kFlickerSecondary, 24, 50)
      )
    ),
  };
}

}  // namespace detail

static const SceneDefinition kScenes[] = {
  detail::makeScene1(),
  detail::makeScene2(),
};

static constexpr size_t kSceneCount = sizeof(kScenes) / sizeof(kScenes[0]);

inline const SceneDefinition& sceneDefinitionFor(size_t sceneIndex) {
  return kScenes[sceneIndex % kSceneCount];
}

inline const char* sceneName(size_t sceneIndex) {
  return sceneDefinitionFor(sceneIndex).name;
}

inline uint32_t flashVariationEpochFor(const FlashCommand& command, uint32_t currentBar) {
  return detail::variationEpochFor(command, currentBar);
}

inline FlashCommand flashSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  (void)effect;
  return sceneDefinitionFor(sceneIndex).flash;
}

inline FlashRenderCommand flashRenderCommandFor(const FlashCommand& command, uint32_t currentBar) {
  if (command.mode == FlashCommandMode::Off) {
    return detail::flashRenderCommand("Off", FlashPattern::Off, 1, 1, 0, 0, 0, 0);
  }

  const uint32_t variationEpoch = detail::variationEpochFor(command, currentBar);
  const detail::FlashMotif motif = detail::pickFlashMotif(command, variationEpoch);
  return detail::flashRenderCommandForMotif(motif, command, variationEpoch);
}

inline const RgbCommand& rgbSceneCommandFor(NodeEffect effect, size_t sceneIndex) {
  const auto& scene = sceneDefinitionFor(sceneIndex);

  switch (effect) {
    case NodeEffect::Wash:
      return scene.wash;

    case NodeEffect::Accent:
      return scene.accent;

    case NodeEffect::Flicker:
      return scene.flicker;

    case NodeEffect::Pulse:
    default:
      return scene.pulse;
  }
}

static const FlashCommand kFlashReference = kScenes[0].flash;
static const RgbCommand& kPulseReference = kScenes[0].pulse;

}  // namespace decaflash::scenes
