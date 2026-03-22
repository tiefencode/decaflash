#include "pdm_microphone.h"

#include <driver/i2s.h>

#include <cstdlib>

namespace decaflash::brain {

namespace {

static constexpr i2s_port_t kMicPort = I2S_NUM_0;
static constexpr gpio_num_t kMicDataPin = GPIO_NUM_26;
static constexpr gpio_num_t kMicClockPin = GPIO_NUM_32;
static constexpr uint32_t kSampleRateHz = 16000;
static constexpr size_t kReadBufferSampleCount = 256;
static constexpr uint32_t kReportIntervalMs = 1000;
static constexpr uint32_t kFramePrintIntervalMs = 4000;
static constexpr uint8_t kDcEstimateShift = 6;
static constexpr uint8_t kEnvelopeShift = 4;
static constexpr uint32_t kMeterFloorMargin = 6;
static constexpr uint32_t kMeterGateMargin = 10;
static constexpr uint32_t kMeterDisplayMargin = 18;
static constexpr uint32_t kMeterMinimumSpan = 180;
static constexpr uint32_t kMeterHeadroomExtra = 140;
static constexpr uint32_t kMeterBeatMargin = 18;
static constexpr uint32_t kMeterBeatSpan = 140;
static constexpr uint8_t kMeterBasePixels = 20;
static constexpr uint8_t kMeterBeatPixels = 5;
static constexpr uint8_t kMeterQuietCyclesForZero = 4;
static constexpr uint8_t kMeterReleaseStep = 2;
static constexpr uint32_t kAnalysisMusicOnMargin = 22;
static constexpr uint32_t kAnalysisMusicOffMargin = 12;
static constexpr uint32_t kAnalysisOnsetMinimum = 18;
static constexpr uint32_t kAnalysisOnsetDivisor = 3;
static constexpr uint32_t kAnalysisPeakRatioLimit = 18;
static constexpr uint8_t kAnalysisLockedConfidence = 50;
static constexpr uint8_t kAnalysisEarlyIntervalPercent = 80;
static constexpr uint8_t kAnalysisDoubleIntervalMinPercent = 170;
static constexpr uint8_t kAnalysisDoubleIntervalMaxPercent = 230;
static constexpr uint32_t kAnalysisOnsetCooldownMs = 300;
static constexpr uint32_t kAnalysisMinIntervalMs = 300;
static constexpr uint32_t kAnalysisMaxIntervalMs = 1200;
static constexpr uint32_t kAnalysisTempoHoldMs = 2200;
static constexpr uint16_t kAnalysisMinTempoBpm = 80;
static constexpr uint16_t kAnalysisMaxTempoBpm = 180;
static constexpr uint8_t kAnalysisHistorySize = 8;
static constexpr uint8_t kAnalysisTempoBucketCount =
  static_cast<uint8_t>(kAnalysisMaxTempoBpm - kAnalysisMinTempoBpm + 1U);
static constexpr uint8_t kAnalysisTempoMaxMultiple = 8;
static constexpr uint8_t kAnalysisTempoTolerancePercent = 12;
static constexpr uint8_t kAnalysisTempoContinuityBonus = 18;
static constexpr uint8_t kAnalysisTempoMinimumMatches = 3;
static constexpr uint8_t kAnalysisTempoCoverageTarget = 8;
static constexpr uint8_t kAnalysisTempoBucketDecayShift = 3;
static constexpr uint8_t kAnalysisTempoNeighborWeightPercent = 30;

}  // namespace

bool PdmMicrophone::begin() {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = kSampleRateHz;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 128;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  const esp_err_t installResult = i2s_driver_install(kMicPort, &config, 0, nullptr);
  if (installResult != ESP_OK) {
    Serial.printf("mic=setup step=driver_install err=%d\n", static_cast<int>(installResult));
    return false;
  }

  i2s_pin_config_t pinConfig = {};
  pinConfig.bck_io_num = I2S_PIN_NO_CHANGE;
  pinConfig.ws_io_num = kMicClockPin;
  pinConfig.data_out_num = I2S_PIN_NO_CHANGE;
  pinConfig.data_in_num = kMicDataPin;

  const esp_err_t pinResult = i2s_set_pin(kMicPort, &pinConfig);
  if (pinResult != ESP_OK) {
    Serial.printf("mic=setup step=set_pin err=%d\n", static_cast<int>(pinResult));
    i2s_driver_uninstall(kMicPort);
    return false;
  }

  const esp_err_t downSampleResult = i2s_set_pdm_rx_down_sample(kMicPort, I2S_PDM_DSR_16S);
  if (downSampleResult != ESP_OK) {
    Serial.printf("mic=setup step=downsample err=%d\n", static_cast<int>(downSampleResult));
    i2s_driver_uninstall(kMicPort);
    return false;
  }

  ready_ = true;
  resetWindowStats();
  lastReportAtMs_ = millis();
  lastFramePrintAtMs_ = millis();

  Serial.printf("mic=ready data_pin=%d clock_pin=%d sample_rate=%lu dma=%ux%u\n",
                static_cast<int>(kMicDataPin),
                static_cast<int>(kMicClockPin),
                static_cast<unsigned long>(kSampleRateHz),
                8u,
                128u);
  Serial.println("mic=report fields=env block mfast mslow disp avg peak dc raw_p2p floor ceil meter afast aslow afloor music onset bpm conf samples");
  return true;
}

void PdmMicrophone::update() {
  if (!ready_) {
    return;
  }

  int16_t sampleBuffer[kReadBufferSampleCount] = {0};
  bool readAnySamples = false;
  updateAbsSum_ = 0;
  updateSampleCount_ = 0;
  updatePeakAbs_ = 0;

  for (uint8_t attempt = 0; attempt < 4; ++attempt) {
    size_t bytesRead = 0;
    const esp_err_t readResult = i2s_read(
      kMicPort,
      sampleBuffer,
      sizeof(sampleBuffer),
      &bytesRead,
      0
    );

    if (readResult != ESP_OK) {
      if (!reportedReadError_) {
        Serial.printf("mic=read err=%d\n", static_cast<int>(readResult));
        reportedReadError_ = true;
      }
      break;
    }

    if (bytesRead == 0) {
      break;
    }

    reportedReadError_ = false;
    readAnySamples = true;
    accumulateSamples(sampleBuffer, bytesRead / sizeof(int16_t));
  }

  if (updateSampleCount_ > 0) {
    blockLevel_ = updateAbsSum_ / updateSampleCount_;
  }

  const uint32_t now = millis();
  if (readAnySamples) {
    updateAnalysis(now, blockLevel_, updatePeakAbs_);
    updateMeterLevel(blockLevel_);
  }
  if (readAnySamples && (now - lastReportAtMs_) >= kReportIntervalMs) {
    printReport(now);
  }
}

bool PdmMicrophone::ready() const {
  return ready_;
}

uint8_t PdmMicrophone::meterLevel() const {
  return meterLevel_;
}

bool PdmMicrophone::musicPresent() const {
  return musicPresent_;
}

uint16_t PdmMicrophone::detectedBpm() const {
  return detectedBpm_;
}

uint8_t PdmMicrophone::beatConfidence() const {
  return beatConfidence_;
}

uint32_t PdmMicrophone::lastOnsetAtMs() const {
  return lastOnsetAtMs_;
}

void PdmMicrophone::resetWindowStats() {
  sampleCount_ = 0;
  rawMinSample_ = INT16_MAX;
  rawMaxSample_ = INT16_MIN;
  centeredAbsSum_ = 0;
  centeredPeakAbs_ = 0;
}

void PdmMicrophone::accumulateSamples(const int16_t* samples, size_t sampleCount) {
  if (sampleCount == 0) {
    return;
  }

  lastFrameCount_ = static_cast<uint8_t>(sampleCount < 8 ? sampleCount : 8);

  for (size_t i = 0; i < sampleCount; ++i) {
    const int16_t sample = samples[i];
    if (sample < rawMinSample_) {
      rawMinSample_ = sample;
    }
    if (sample > rawMaxSample_) {
      rawMaxSample_ = sample;
    }

    dcEstimate_ += (static_cast<int32_t>(sample) - dcEstimate_) >> kDcEstimateShift;
    const int32_t centeredSample = static_cast<int32_t>(sample) - dcEstimate_;

    if (i < lastFrameCount_) {
      lastFrame_[i] = static_cast<int16_t>(centeredSample);
    }

    const uint16_t absCenteredSample = static_cast<uint16_t>(abs(centeredSample));
    if (absCenteredSample > centeredPeakAbs_) {
      centeredPeakAbs_ = absCenteredSample;
    }
    if (absCenteredSample > updatePeakAbs_) {
      updatePeakAbs_ = absCenteredSample;
    }

    centeredAbsSum_ += absCenteredSample;
    updateAbsSum_ += absCenteredSample;
    updateSampleCount_++;
    envelopeLevel_ =
      ((envelopeLevel_ * ((1UL << kEnvelopeShift) - 1UL)) + absCenteredSample) >> kEnvelopeShift;
    sampleCount_++;
  }
}

void PdmMicrophone::printReport(uint32_t now) {
  if (sampleCount_ == 0) {
    return;
  }

  const uint32_t averageLevel = centeredAbsSum_ / sampleCount_;
  const int32_t rawPeakToPeak =
    static_cast<int32_t>(rawMaxSample_) - static_cast<int32_t>(rawMinSample_);

  Serial.printf("mic=level env=%lu block=%lu mfast=%lu mslow=%lu disp=%lu avg=%lu peak=%u dc=%ld raw_p2p=%ld floor=%lu ceil=%lu meter=%u afast=%lu aslow=%lu afloor=%lu music=%u onset=%lu bpm=%u conf=%u samples=%lu\n",
                static_cast<unsigned long>(envelopeLevel_),
                static_cast<unsigned long>(blockLevel_),
                static_cast<unsigned long>(meterFastLevel_),
                static_cast<unsigned long>(meterSlowLevel_),
                static_cast<unsigned long>(meterDisplayLevel_),
                static_cast<unsigned long>(averageLevel),
                static_cast<unsigned>(centeredPeakAbs_),
                static_cast<long>(dcEstimate_),
                static_cast<long>(rawPeakToPeak),
                static_cast<unsigned long>(noiseFloor_),
                static_cast<unsigned long>(signalCeiling_),
                static_cast<unsigned>(meterLevel_),
                static_cast<unsigned long>(analysisFastLevel_),
                static_cast<unsigned long>(analysisSlowLevel_),
                static_cast<unsigned long>(analysisFloor_),
                static_cast<unsigned>(musicPresent_),
                static_cast<unsigned long>(onsetStrength_),
                static_cast<unsigned>(detectedBpm_),
                static_cast<unsigned>(beatConfidence_),
                static_cast<unsigned long>(sampleCount_));

  if (lastFrameCount_ > 0 && (now - lastFramePrintAtMs_) >= kFramePrintIntervalMs) {
    Serial.print("mic=frame_centered");
    for (uint8_t i = 0; i < lastFrameCount_; ++i) {
      Serial.printf(" %d", lastFrame_[i]);
    }
    Serial.println();
    lastFramePrintAtMs_ = now;
  }

  resetWindowStats();
  lastReportAtMs_ = now;
}

void PdmMicrophone::updateAnalysis(uint32_t now, uint32_t blockLevel, uint16_t peakLevel) {
  if (analysisFastLevel_ == 0) {
    analysisFastLevel_ = blockLevel;
  } else if (blockLevel > analysisFastLevel_) {
    analysisFastLevel_ = ((analysisFastLevel_ * 2UL) + blockLevel) / 3UL;
  } else {
    analysisFastLevel_ = ((analysisFastLevel_ * 5UL) + blockLevel) / 6UL;
  }

  if (analysisSlowLevel_ == 0) {
    analysisSlowLevel_ = blockLevel;
  } else if (blockLevel > analysisSlowLevel_) {
    analysisSlowLevel_ = ((analysisSlowLevel_ * 15UL) + blockLevel) / 16UL;
  } else {
    analysisSlowLevel_ = ((analysisSlowLevel_ * 31UL) + blockLevel) / 32UL;
  }

  if (analysisFloor_ == 0) {
    analysisFloor_ = analysisSlowLevel_;
  } else if (analysisSlowLevel_ < analysisFloor_) {
    analysisFloor_ = ((analysisFloor_ * 7UL) + analysisSlowLevel_) / 8UL;
  } else {
    analysisFloor_ = ((analysisFloor_ * 255UL) + analysisSlowLevel_) / 256UL;
  }

  const uint32_t musicOnThreshold = analysisFloor_ + kAnalysisMusicOnMargin;
  const uint32_t musicOffThreshold = analysisFloor_ + kAnalysisMusicOffMargin;
  if (musicPresent_) {
    musicPresent_ = analysisSlowLevel_ > musicOffThreshold;
  } else {
    musicPresent_ = analysisSlowLevel_ > musicOnThreshold;
  }

  const uint32_t transientLevel =
    (analysisFastLevel_ > analysisSlowLevel_) ? (analysisFastLevel_ - analysisSlowLevel_) : 0;
  onsetStrength_ = transientLevel;

  if ((now - lastOnsetAtMs_) > kAnalysisTempoHoldMs) {
    if (!musicPresent_) {
      detectedBpm_ = 0;
      onsetTimestampCount_ = 0;
      onsetIntervalCount_ = 0;
      for (uint32_t& bucketScore : tempoBucketScores_) {
        bucketScore = 0;
      }
    }
    beatConfidence_ = 0;
  }

  if (!musicPresent_) {
    return;
  }

  const uint32_t energySpan =
    (analysisSlowLevel_ > analysisFloor_) ? (analysisSlowLevel_ - analysisFloor_) : 0;
  const uint32_t onsetThreshold = kAnalysisOnsetMinimum + (energySpan / kAnalysisOnsetDivisor);
  const bool cooldownElapsed =
    (lastOnsetAtMs_ == 0) || ((now - lastOnsetAtMs_) >= kAnalysisOnsetCooldownMs);
  const bool peakLooksMusical =
    (blockLevel == 0) || (peakLevel <= (blockLevel * kAnalysisPeakRatioLimit));

  if (!cooldownElapsed || !peakLooksMusical || transientLevel <= onsetThreshold) {
    return;
  }

  uint32_t intervalMs = (lastOnsetAtMs_ == 0) ? 0 : (now - lastOnsetAtMs_);
  if (intervalMs != 0 && intervalMs < kAnalysisMinIntervalMs) {
    return;
  }

  if (intervalMs != 0 && detectedBpm_ != 0 && beatConfidence_ >= kAnalysisLockedConfidence) {
    const uint32_t expectedIntervalMs = 60000UL / detectedBpm_;
    const uint32_t earlyIntervalMs =
      (expectedIntervalMs * kAnalysisEarlyIntervalPercent) / 100UL;
    if (intervalMs < earlyIntervalMs) {
      return;
    }

    const uint32_t doubleIntervalMinMs =
      (expectedIntervalMs * kAnalysisDoubleIntervalMinPercent) / 100UL;
    const uint32_t doubleIntervalMaxMs =
      (expectedIntervalMs * kAnalysisDoubleIntervalMaxPercent) / 100UL;
    if (intervalMs >= doubleIntervalMinMs && intervalMs <= doubleIntervalMaxMs) {
      intervalMs /= 2UL;
    }
  }

  lastOnsetAtMs_ = now;
  registerOnset(now, transientLevel, intervalMs);
}

void PdmMicrophone::registerOnset(uint32_t now, uint32_t onsetStrength, uint32_t intervalMs) {
  if (onsetTimestampCount_ < kAnalysisHistorySize) {
    onsetTimestampsMs_[onsetTimestampCount_++] = now;
  } else {
    for (uint8_t i = 1; i < kAnalysisHistorySize; ++i) {
      onsetTimestampsMs_[i - 1] = onsetTimestampsMs_[i];
    }
    onsetTimestampsMs_[kAnalysisHistorySize - 1] = now;
  }

  if (intervalMs >= kAnalysisMinIntervalMs && intervalMs <= kAnalysisMaxIntervalMs) {
    if (onsetIntervalCount_ < kAnalysisHistorySize) {
      onsetIntervalsMs_[onsetIntervalCount_++] = intervalMs;
    } else {
      for (uint8_t i = 1; i < kAnalysisHistorySize; ++i) {
        onsetIntervalsMs_[i - 1] = onsetIntervalsMs_[i];
      }
      onsetIntervalsMs_[kAnalysisHistorySize - 1] = intervalMs;
    }
    updateTempoEstimate();
  } else if (intervalMs > kAnalysisMaxIntervalMs) {
    onsetTimestampCount_ = 1;
    onsetTimestampsMs_[0] = now;
    onsetIntervalCount_ = 0;
    beatConfidence_ = 0;
  }

  Serial.printf("mic=beat onset=%lu interval=%lu bpm=%u conf=%u block=%lu aslow=%lu peak=%u music=%u at=%lu\n",
                static_cast<unsigned long>(onsetStrength),
                static_cast<unsigned long>(intervalMs),
                static_cast<unsigned>(detectedBpm_),
                static_cast<unsigned>(beatConfidence_),
                static_cast<unsigned long>(blockLevel_),
                static_cast<unsigned long>(analysisSlowLevel_),
                static_cast<unsigned>(updatePeakAbs_),
                static_cast<unsigned>(musicPresent_),
                static_cast<unsigned long>(now));
}

void PdmMicrophone::updateTempoEstimate() {
  if (onsetTimestampCount_ < 3) {
    return;
  }

  const uint16_t previousDetectedBpm = detectedBpm_;
  uint32_t instantScores[kAnalysisTempoBucketCount] = {0};
  uint32_t instantErrorSums[kAnalysisTempoBucketCount] = {0};
  uint8_t instantMatchCounts[kAnalysisTempoBucketCount] = {0};

  for (uint16_t bpm = kAnalysisMinTempoBpm; bpm <= kAnalysisMaxTempoBpm; ++bpm) {
    uint32_t score = 0;
    uint32_t errorSum = 0;
    uint8_t matchCount = 0;

    for (uint8_t newer = 1; newer < onsetTimestampCount_; ++newer) {
      for (uint8_t older = 0; older < newer; ++older) {
        const uint32_t diffMs = onsetTimestampsMs_[newer] - onsetTimestampsMs_[older];
        if (diffMs == 0 || diffMs > kAnalysisTempoHoldMs) {
          continue;
        }

        const uint32_t candidateMultiple =
          ((diffMs * static_cast<uint32_t>(bpm)) + 30000UL) / 60000UL;
        if (candidateMultiple == 0 || candidateMultiple > kAnalysisTempoMaxMultiple) {
          continue;
        }

        const uint32_t expectedDiffMs =
          ((60000UL * candidateMultiple) + (bpm / 2U)) / bpm;
        uint32_t toleranceMs =
          (expectedDiffMs * kAnalysisTempoTolerancePercent) / 100UL;
        if (toleranceMs < 24UL) {
          toleranceMs = 24UL;
        }

        const uint32_t errorMs = static_cast<uint32_t>(abs(
          static_cast<int32_t>(diffMs) - static_cast<int32_t>(expectedDiffMs)
        ));
        if (errorMs > toleranceMs) {
          continue;
        }

        const uint32_t recencyWeight = 1UL + newer;
        const uint32_t multipleWeight =
          (kAnalysisTempoMaxMultiple + 1UL) - candidateMultiple;
        score += (recencyWeight * multipleWeight * 8UL) + (toleranceMs - errorMs);
        errorSum += errorMs;
        matchCount++;
      }
    }

    const uint8_t bucketIndex = static_cast<uint8_t>(bpm - kAnalysisMinTempoBpm);
    instantScores[bucketIndex] = score;
    instantErrorSums[bucketIndex] = errorSum;
    instantMatchCounts[bucketIndex] = matchCount;
  }

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    tempoBucketScores_[i] -= tempoBucketScores_[i] >> kAnalysisTempoBucketDecayShift;
    tempoBucketScores_[i] += instantScores[i];
  }

  uint16_t bestBpm = 0;
  uint8_t bestBucketIndex = 0;
  uint32_t bestScore = 0;
  uint32_t secondBestScore = 0;
  uint32_t bestErrorSum = 0;
  uint8_t bestMatchCount = 0;

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    if (instantMatchCounts[i] < kAnalysisTempoMinimumMatches || tempoBucketScores_[i] == 0) {
      continue;
    }

    uint32_t score = tempoBucketScores_[i];
    if (i > 0) {
      score += (tempoBucketScores_[i - 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }
    if ((i + 1U) < kAnalysisTempoBucketCount) {
      score += (tempoBucketScores_[i + 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }

    const uint16_t bpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + i);
    if (previousDetectedBpm != 0) {
      const uint16_t bpmDelta =
        (previousDetectedBpm > bpm) ? (previousDetectedBpm - bpm) : (bpm - previousDetectedBpm);
      if (bpmDelta <= 2U) {
        score += static_cast<uint32_t>((3U - bpmDelta) * kAnalysisTempoContinuityBonus * 4UL);
      }
    }

    if (score > bestScore) {
      secondBestScore = bestScore;
      bestScore = score;
      bestBpm = bpm;
      bestBucketIndex = i;
      bestErrorSum = instantErrorSums[i];
      bestMatchCount = instantMatchCounts[i];
    } else if (score > secondBestScore) {
      secondBestScore = score;
    }
  }

  if (bestBpm == 0 || bestMatchCount < kAnalysisTempoMinimumMatches || bestScore == 0) {
    beatConfidence_ = (beatConfidence_ > 6U) ? static_cast<uint8_t>(beatConfidence_ - 6U) : 0;
    return;
  }

  const uint32_t bestIntervalMs = 60000UL / bestBpm;
  const uint32_t averageErrorMs = bestErrorSum / bestMatchCount;
  uint32_t precisionScore = 100UL;
  if (bestIntervalMs > 0) {
    const uint32_t precisionPenalty = (averageErrorMs * 320UL) / bestIntervalMs;
    precisionScore = (precisionPenalty >= 100UL) ? 0UL : (100UL - precisionPenalty);
  }

  uint32_t coverageScore =
    (bestMatchCount * 100UL) / kAnalysisTempoCoverageTarget;
  if (coverageScore > 100UL) {
    coverageScore = 100UL;
  }

  const uint32_t separationScore =
    (bestScore > 0 && bestScore > secondBestScore)
      ? ((bestScore - secondBestScore) * 100UL) / bestScore
      : 0UL;

  uint32_t continuityScore = 60UL;
  if (previousDetectedBpm != 0) {
    const uint16_t bpmDelta =
      (previousDetectedBpm > bestBpm) ? (previousDetectedBpm - bestBpm) : (bestBpm - previousDetectedBpm);
    const uint32_t continuityPenalty = bpmDelta * 12UL;
    continuityScore = (continuityPenalty >= 100UL) ? 0UL : (100UL - continuityPenalty);
  }

  const uint32_t confidence =
    ((precisionScore * 2UL) + coverageScore + separationScore + continuityScore) / 5UL;
  beatConfidence_ = static_cast<uint8_t>((confidence > 100UL) ? 100UL : confidence);

  const uint16_t bestRawBpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + bestBucketIndex);
  if (previousDetectedBpm == 0 ||
      ((previousDetectedBpm > bestRawBpm) ? (previousDetectedBpm - bestRawBpm) : (bestRawBpm - previousDetectedBpm)) >= 6U) {
    detectedBpm_ = bestRawBpm;
  } else {
    detectedBpm_ = static_cast<uint16_t>((previousDetectedBpm + bestRawBpm + 1U) / 2U);
  }
}

void PdmMicrophone::updateMeterLevel(uint32_t blockLevel) {
  if (meterFastLevel_ == 0) {
    meterFastLevel_ = blockLevel;
  } else if (blockLevel > meterFastLevel_) {
    meterFastLevel_ = ((meterFastLevel_ * 3UL) + blockLevel) / 4UL;
  } else {
    meterFastLevel_ = ((meterFastLevel_ * 7UL) + blockLevel) / 8UL;
  }

  if (meterSlowLevel_ == 0) {
    meterSlowLevel_ = blockLevel;
  } else if (blockLevel > meterSlowLevel_) {
    meterSlowLevel_ = ((meterSlowLevel_ * 7UL) + blockLevel) / 8UL;
  } else {
    meterSlowLevel_ = ((meterSlowLevel_ * 15UL) + blockLevel) / 16UL;
  }

  const uint32_t transientLevel =
    (meterFastLevel_ > meterSlowLevel_) ? (meterFastLevel_ - meterSlowLevel_) : 0;

  meterDisplayLevel_ = meterSlowLevel_ + (transientLevel / 4UL);

  if (noiseFloor_ == 0) {
    noiseFloor_ = meterSlowLevel_;
  } else if (meterSlowLevel_ < noiseFloor_) {
    noiseFloor_ = ((noiseFloor_ * 7UL) + meterSlowLevel_) / 8UL;
  } else {
    noiseFloor_ = ((noiseFloor_ * 127UL) + meterSlowLevel_) / 128UL;
  }

  if (meterSlowLevel_ > signalCeiling_) {
    signalCeiling_ = ((signalCeiling_ * 3UL) + meterSlowLevel_) / 4UL;
  } else {
    signalCeiling_ = ((signalCeiling_ * 15UL) + meterSlowLevel_) / 16UL;
  }

  const uint32_t effectiveFloor = noiseFloor_ + kMeterFloorMargin;
  const uint32_t gateThreshold = effectiveFloor + kMeterGateMargin;
  const uint32_t displayThreshold = gateThreshold + kMeterDisplayMargin;

  if (meterSlowLevel_ <= gateThreshold) {
    if (quietCycleCount_ < kMeterQuietCyclesForZero) {
      quietCycleCount_++;
    }

    if (signalCeiling_ > gateThreshold) {
      signalCeiling_ = ((signalCeiling_ * 3UL) + gateThreshold) / 4UL;
    }

    if (quietCycleCount_ >= kMeterQuietCyclesForZero) {
      meterLevel_ = 0;
      return;
    }

    meterLevel_ = (meterLevel_ > kMeterReleaseStep) ? (meterLevel_ - kMeterReleaseStep) : 0;
    return;
  }

  quietCycleCount_ = 0;

  uint32_t baseSpan = kMeterMinimumSpan;
  if (signalCeiling_ > displayThreshold) {
    baseSpan = (signalCeiling_ - displayThreshold) + kMeterHeadroomExtra;
    if (baseSpan < kMeterMinimumSpan) {
      baseSpan = kMeterMinimumSpan;
    }
  }

  uint32_t baseNormalized = 0;
  if (meterSlowLevel_ > displayThreshold) {
    baseNormalized = meterSlowLevel_ - displayThreshold;
    if (baseNormalized > baseSpan) {
      baseNormalized = baseSpan;
    }
  }

  uint8_t baseLevel = static_cast<uint8_t>((baseNormalized * kMeterBasePixels) / baseSpan);
  if (baseLevel <= 1) {
    baseLevel = 0;
  }

  uint32_t beatNormalized = 0;
  if (transientLevel > kMeterBeatMargin) {
    beatNormalized = transientLevel - kMeterBeatMargin;
    if (beatNormalized > kMeterBeatSpan) {
      beatNormalized = kMeterBeatSpan;
    }
  }

  const uint8_t beatLevel =
    static_cast<uint8_t>((beatNormalized * kMeterBeatPixels) / kMeterBeatSpan);

  uint8_t targetLevel = static_cast<uint8_t>(baseLevel + beatLevel);
  if (targetLevel > 25) {
    targetLevel = 25;
  }

  if (targetLevel >= meterLevel_) {
    meterLevel_ = targetLevel;
    return;
  }

  const uint8_t releasedLevel =
    (meterLevel_ > kMeterReleaseStep) ? (meterLevel_ - kMeterReleaseStep) : 0;
  meterLevel_ = (releasedLevel > targetLevel) ? releasedLevel : targetLevel;
}

}  // namespace decaflash::brain
