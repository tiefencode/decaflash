#include "pdm_microphone.h"

#include <driver/i2s.h>

#include <cstdlib>
#include <cstring>

namespace decaflash::brain {

namespace {

static constexpr i2s_port_t kMicPort = I2S_NUM_0;
static constexpr gpio_num_t kMicDataPin = GPIO_NUM_26;
static constexpr gpio_num_t kMicClockPin = GPIO_NUM_32;
static constexpr uint32_t kSampleRateHz = 16000;
static constexpr uint32_t kRecordingStoredSampleRateHz = 16000;
static constexpr uint8_t kRecordingDecimationFactor = 1;
static constexpr size_t kReadBufferSampleCount = 256;
static constexpr uint32_t kDefaultRecordingDurationMs = 12000;
static constexpr uint32_t kMinimumRecordingDurationMs = 1000;
static constexpr uint32_t kMaximumRecordingDurationMs = 12000;
static constexpr uint32_t kRecordingClampStepMs = 250;
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
static constexpr uint8_t kAnalysisEarlyIntervalPercent = 88;
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
static constexpr uint8_t kAnalysisTempoTolerancePercent = 9;
static constexpr uint8_t kAnalysisTempoContinuityBonus = 18;
static constexpr uint8_t kAnalysisTempoMinimumMatches = 3;
static constexpr uint8_t kAnalysisTempoCoverageTarget = 8;
static constexpr uint8_t kAnalysisTempoBucketDecayShift = 3;
static constexpr uint8_t kAnalysisTempoNeighborWeightPercent = 30;
static constexpr uint8_t kAnalysisTempoFamilyWeightPercent = 45;
static constexpr uint8_t kAnalysisTempoDirectTolerancePercent = 8;
static constexpr uint8_t kAnalysisTempoHoldConfidence = 60;
static constexpr uint8_t kAnalysisTempoHoldPercent = 112;
static constexpr uint8_t kAnalysisTempoFamilyHoldPercent = 105;
static constexpr uint8_t kAnalysisTempoFamilyToleranceBpm = 4;
static constexpr uint16_t kAnalysisTempoFamilyPreferredMinBpm = 90;
static constexpr uint16_t kAnalysisTempoFamilyPreferredMaxBpm = 170;
static constexpr uint16_t kAnalysisTempoFamilyPreferredCenterBpm = 130;
static constexpr uint8_t kAnalysisTempoRepresentativePairwiseWeight = 2;
static constexpr uint8_t kAnalysisTempoRepresentativeMemoryDivisor = 4;
static constexpr uint8_t kAnalysisTempoRepresentativeFamilyPenalty = 5;
static constexpr uint8_t kAnalysisTempoRepresentativeFamilyBonus = 10;
static constexpr uint8_t kAnalysisTempoPrimaryScoreWeight = 6;
static constexpr uint8_t kAnalysisTempoPrimaryCoverageTarget = 4;
static constexpr uint8_t kAnalysisTempoSameFamilySwitchPercent = 125;
static constexpr uint8_t kAnalysisClockSubdivisionSwitchHits = 4;
static constexpr uint8_t kAnalysisHalfIntervalMinPercent = 45;
static constexpr uint8_t kAnalysisHalfIntervalMaxPercent = 65;
static constexpr uint8_t kAnalysisPulseHistorySize = 64;
static constexpr uint8_t kAnalysisPulseScaleShift = 2;
static constexpr uint8_t kAnalysisPulseScoreWeight = 6;
static constexpr bool kVerboseMicLevelReports = false;
static constexpr bool kVerboseMicBeatReports = false;
PdmMicrophone* gPrimaryMicrophone = nullptr;

uint16_t bpmDifference(uint16_t left, uint16_t right) {
  return (left > right) ? (left - right) : (right - left);
}

uint16_t distanceToTempoWindow(uint16_t bpm) {
  if (bpm < kAnalysisTempoFamilyPreferredMinBpm) {
    return kAnalysisTempoFamilyPreferredMinBpm - bpm;
  }
  if (bpm > kAnalysisTempoFamilyPreferredMaxBpm) {
    return bpm - kAnalysisTempoFamilyPreferredMaxBpm;
  }
  return 0;
}

uint16_t canonicalTempoFamilyBpm(uint16_t bpm) {
  uint16_t bestBpm = bpm;
  uint16_t bestWindowDistance = distanceToTempoWindow(bestBpm);
  uint16_t bestCenterDistance = bpmDifference(bestBpm, kAnalysisTempoFamilyPreferredCenterBpm);

  const uint32_t candidateValues[3] = {
    static_cast<uint32_t>(bpm),
    static_cast<uint32_t>(bpm / 2U),
    static_cast<uint32_t>(bpm) * 2UL,
  };

  for (uint32_t candidateValue : candidateValues) {
    if (candidateValue < kAnalysisMinTempoBpm || candidateValue > kAnalysisMaxTempoBpm) {
      continue;
    }

    const uint16_t candidateBpm = static_cast<uint16_t>(candidateValue);
    const uint16_t candidateWindowDistance = distanceToTempoWindow(candidateBpm);
    const uint16_t candidateCenterDistance =
      bpmDifference(candidateBpm, kAnalysisTempoFamilyPreferredCenterBpm);

    if (candidateWindowDistance < bestWindowDistance ||
        (candidateWindowDistance == bestWindowDistance &&
         candidateCenterDistance < bestCenterDistance)) {
      bestBpm = candidateBpm;
      bestWindowDistance = candidateWindowDistance;
      bestCenterDistance = candidateCenterDistance;
    }
  }

  return bestBpm;
}

bool inSameTempoFamily(uint16_t left, uint16_t right) {
  if (left == 0 || right == 0) {
    return false;
  }

  const uint16_t leftCanonical = canonicalTempoFamilyBpm(left);
  const uint16_t rightCanonical = canonicalTempoFamilyBpm(right);
  if (leftCanonical == rightCanonical) {
    return true;
  }

  const uint16_t lower = (leftCanonical < rightCanonical) ? leftCanonical : rightCanonical;
  const uint16_t higher = (leftCanonical < rightCanonical) ? rightCanonical : leftCanonical;
  return static_cast<uint16_t>(abs(
           static_cast<int32_t>(lower * 2U) - static_cast<int32_t>(higher)
         )) <= kAnalysisTempoFamilyToleranceBpm;
}

}  // namespace

void releaseRecordedAudioClip(RecordedAudioClip& clip) {
  free(clip.data);
  clip = {};
}

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
  gPrimaryMicrophone = this;
  resetWindowStats();
  lastReportAtMs_ = millis();
  lastFramePrintAtMs_ = millis();

  Serial.printf("mic=ready data_pin=%d clock_pin=%d sample_rate=%lu dma=%ux%u\n",
                static_cast<int>(kMicDataPin),
                static_cast<int>(kMicClockPin),
                static_cast<unsigned long>(kSampleRateHz),
                8u,
                128u);
  if (kVerboseMicLevelReports) {
    Serial.println("mic=report fields=env block mfast mslow disp avg peak dc raw_p2p floor ceil meter afast aslow afloor music onset bpm conf samples");
  } else {
    Serial.println("mic=report compact");
  }
  return true;
}

void PdmMicrophone::update() {
  if (!ready_) {
    return;
  }

  const uint32_t startedAtMs = millis();
  if (recordingRequested_ && !recordingActive_) {
    beginRequestedRecording(startedAtMs);
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
  if (recordingActive_ && recordingSampleCount_ >= recordingTargetSampleCount_) {
    finishRecording(now);
  }
  if (readAnySamples && (now - lastReportAtMs_) >= kReportIntervalMs) {
    printReport(now);
  }
}

bool PdmMicrophone::requestRecording(uint32_t durationMs) {
  if (!ready_) {
    Serial.println("record=abort reason=mic_not_ready");
    return false;
  }

  if (recordingActive_) {
    Serial.printf("record=busy samples=%u\n",
                  static_cast<unsigned>(recordingSampleCount_));
    return false;
  }

  if (durationMs == 0) {
    durationMs = kDefaultRecordingDurationMs;
  }

  if (durationMs < kMinimumRecordingDurationMs ||
      durationMs > kMaximumRecordingDurationMs) {
    Serial.printf("record=invalid_duration expected=%lu..%lu\n",
                  static_cast<unsigned long>(kMinimumRecordingDurationMs),
                  static_cast<unsigned long>(kMaximumRecordingDurationMs));
    return false;
  }

  const size_t minimumSampleCount =
    (static_cast<size_t>(kMinimumRecordingDurationMs) * kRecordingStoredSampleRateHz) / 1000U;
  const size_t clampStepSampleCount =
    (static_cast<size_t>(kRecordingClampStepMs) * kRecordingStoredSampleRateHz) / 1000U;
  const size_t requestedSampleCount =
    (static_cast<size_t>(durationMs) * kRecordingStoredSampleRateHz) / 1000U;

  size_t targetSampleCount = requestedSampleCount;
  while (targetSampleCount >= minimumSampleCount &&
         !ensureRecordingBufferCapacity(targetSampleCount)) {
    if (targetSampleCount <= clampStepSampleCount ||
        targetSampleCount <= minimumSampleCount) {
      break;
    }

    targetSampleCount -= clampStepSampleCount;
  }

  if (recordingBuffer_ == nullptr || targetSampleCount < minimumSampleCount) {
    Serial.printf("record=abort reason=no_memory bytes=%u\n",
                  static_cast<unsigned>(ima_adpcm::requiredBytesForSamples(requestedSampleCount)));
    return false;
  }

  if (targetSampleCount != requestedSampleCount) {
    durationMs = static_cast<uint32_t>(
      (static_cast<uint64_t>(targetSampleCount) * 1000ULL) / kRecordingStoredSampleRateHz
    );
    Serial.printf("record=clamp duration_ms=%lu\n",
                  static_cast<unsigned long>(durationMs));
  }

  requestedRecordingDurationMs_ = durationMs;
  recordingTargetSampleCount_ = targetSampleCount;
  recordingRequested_ = true;
  recordingReady_ = false;
  recordingFinishedAtMs_ = 0;
  recordingSampleCount_ = 0;
  recordingEncodedByteCount_ = 0;
  Serial.printf("record=request duration_ms=%lu\n",
                static_cast<unsigned long>(durationMs));
  return true;
}

bool PdmMicrophone::ready() const {
  return ready_;
}

bool PdmMicrophone::recordingActive() const {
  return recordingActive_;
}

bool PdmMicrophone::recordingReady() const {
  return recordingReady_;
}

uint32_t PdmMicrophone::recordingSampleRateHz() const {
  return kRecordingStoredSampleRateHz;
}

size_t PdmMicrophone::recordedSampleCount() const {
  return recordingSampleCount_;
}

size_t PdmMicrophone::recordedByteCount() const {
  return recordingEncodedByteCount_;
}

bool PdmMicrophone::takeRecording(RecordedAudioClip& clip) {
  clip = {};

  if (!recordingReady_ || recordingBuffer_ == nullptr || recordingSampleCount_ == 0) {
    return false;
  }

  clip.data = recordingBuffer_;
  clip.byteCount = recordingEncodedByteCount_;
  clip.sampleCount = recordingSampleCount_;
  clip.sampleRateHz = kRecordingStoredSampleRateHz;

  recordingBuffer_ = nullptr;
  recordingBufferCapacity_ = 0;
  recordingReady_ = false;
  recordingFinishedAtMs_ = 0;
  recordingSampleCount_ = 0;
  recordingEncodedByteCount_ = 0;
  recordingTargetSampleCount_ = 0;
  recordingInputSampleCount_ = 0;
  requestedRecordingDurationMs_ = 0;
  resetRecordingEncoding();
  return true;
}

bool PdmMicrophone::cancelRecording() {
  if (!recordingRequested_ && !recordingActive_ && !recordingReady_) {
    return false;
  }

  const bool wasActive = recordingActive_;
  recordingRequested_ = false;
  recordingActive_ = false;
  recordingReady_ = false;
  recordingStartedAtMs_ = 0;
  recordingFinishedAtMs_ = 0;
  recordingSampleCount_ = 0;
  recordingEncodedByteCount_ = 0;
  recordingTargetSampleCount_ = 0;
  recordingInputSampleCount_ = 0;
  requestedRecordingDurationMs_ = 0;
  resetRecordingEncoding();

  Serial.printf("record=cancel state=%s\n", wasActive ? "active" : "pending");
  return true;
}

void PdmMicrophone::clearRecording() {
  recordingReady_ = false;
  recordingFinishedAtMs_ = 0;
  recordingSampleCount_ = 0;
  recordingEncodedByteCount_ = 0;
  resetRecordingEncoding();
}

bool PdmMicrophone::ensureRecordingBufferCapacity(size_t sampleCapacity) {
  const size_t byteCapacity = ima_adpcm::requiredBytesForSamples(sampleCapacity);
  if (byteCapacity == 0) {
    return false;
  }

  if (byteCapacity <= recordingBufferCapacity_ && recordingBuffer_ != nullptr) {
    return true;
  }

  free(recordingBuffer_);
  recordingBuffer_ = static_cast<uint8_t*>(malloc(byteCapacity));
  if (recordingBuffer_ == nullptr) {
    recordingBufferCapacity_ = 0;
    return false;
  }

  recordingBufferCapacity_ = byteCapacity;
  return true;
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

uint16_t PdmMicrophone::clockBpm() const {
  return clockBpm_;
}

uint32_t PdmMicrophone::lastOnsetAtMs() const {
  return lastOnsetAtMs_;
}

void PdmMicrophone::resetRecordingEncoding() {
  recordingAdpcmState_ = {};
  recordingAdpcmPartialByte_ = 0;
  recordingAdpcmHalfBytePending_ = false;
  recordingAdpcmSeeded_ = false;
}

bool PdmMicrophone::appendRecordingSample(int16_t sample) {
  if (recordingBuffer_ == nullptr || recordingSampleCount_ >= recordingTargetSampleCount_) {
    return false;
  }

  if (!recordingAdpcmSeeded_) {
    recordingAdpcmState_.predictor = sample;
    recordingAdpcmState_.index = 0;
    ima_adpcm::writeHeader(recordingBuffer_, recordingAdpcmState_);
    recordingEncodedByteCount_ = ima_adpcm::kHeaderSize;
    recordingAdpcmSeeded_ = true;
    recordingSampleCount_ = 1;
    return true;
  }

  const uint8_t code = ima_adpcm::encodeSample(sample, recordingAdpcmState_);
  if (!recordingAdpcmHalfBytePending_) {
    if (recordingEncodedByteCount_ >= recordingBufferCapacity_) {
      return false;
    }

    recordingAdpcmPartialByte_ = static_cast<uint8_t>(code & 0x0F);
    recordingAdpcmHalfBytePending_ = true;
  } else {
    recordingAdpcmPartialByte_ |= static_cast<uint8_t>((code & 0x0F) << 4);
    if (recordingEncodedByteCount_ >= recordingBufferCapacity_) {
      return false;
    }
    recordingBuffer_[recordingEncodedByteCount_++] = recordingAdpcmPartialByte_;
    recordingAdpcmPartialByte_ = 0;
    recordingAdpcmHalfBytePending_ = false;
  }

  recordingSampleCount_++;
  return true;
}

void PdmMicrophone::beginRequestedRecording(uint32_t now) {
  recordingRequested_ = false;
  recordingActive_ = true;
  recordingReady_ = false;
  recordingStartedAtMs_ = now;
  recordingFinishedAtMs_ = 0;
  recordingSampleCount_ = 0;
  recordingEncodedByteCount_ = 0;
  recordingInputSampleCount_ = 0;
  resetRecordingEncoding();

  Serial.printf("record=start duration_ms=%lu target_samples=%u sample_rate=%lu\n",
                static_cast<unsigned long>(requestedRecordingDurationMs_),
                static_cast<unsigned>(recordingTargetSampleCount_),
                static_cast<unsigned long>(kRecordingStoredSampleRateHz));
}

void PdmMicrophone::finishRecording(uint32_t now) {
  if (recordingAdpcmHalfBytePending_) {
    if (recordingEncodedByteCount_ < recordingBufferCapacity_) {
      recordingBuffer_[recordingEncodedByteCount_++] = recordingAdpcmPartialByte_;
    }
    recordingAdpcmPartialByte_ = 0;
    recordingAdpcmHalfBytePending_ = false;
  }

  recordingActive_ = false;
  recordingReady_ = true;
  recordingFinishedAtMs_ = now;

  const uint32_t capturedDurationMs = static_cast<uint32_t>(
    (static_cast<uint64_t>(recordingSampleCount_) * 1000ULL) /
    kRecordingStoredSampleRateHz
  );

  Serial.printf("record=done duration_ms=%lu samples=%u bytes=%u elapsed_ms=%lu format=ima_adpcm\n",
                static_cast<unsigned long>(capturedDurationMs),
                static_cast<unsigned>(recordingSampleCount_),
                static_cast<unsigned>(recordingEncodedByteCount_),
                static_cast<unsigned long>(recordingFinishedAtMs_ - recordingStartedAtMs_));
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

    if (recordingActive_ && recordingSampleCount_ < recordingTargetSampleCount_ &&
        ((recordingInputSampleCount_++ % kRecordingDecimationFactor) == 0U)) {
      int32_t clippedSample = centeredSample;
      if (clippedSample > INT16_MAX) {
        clippedSample = INT16_MAX;
      } else if (clippedSample < INT16_MIN) {
        clippedSample = INT16_MIN;
      }

      if (!appendRecordingSample(static_cast<int16_t>(clippedSample))) {
        recordingActive_ = false;
        recordingRequested_ = false;
        recordingReady_ = false;
        recordingSampleCount_ = 0;
        recordingEncodedByteCount_ = 0;
        recordingTargetSampleCount_ = 0;
        requestedRecordingDurationMs_ = 0;
        resetRecordingEncoding();
        Serial.println("record=abort reason=no_memory");
        break;
      }
    }
  }
}

void PdmMicrophone::printReport(uint32_t now) {
  if (sampleCount_ == 0) {
    return;
  }

  resetWindowStats();
  lastReportAtMs_ = now;
}

void PdmMicrophone::recordPulseFrame(uint32_t now, uint32_t transientLevel, uint32_t onsetThreshold) {
  if (lastAnalysisFrameAtMs_ != 0) {
    const uint32_t frameIntervalMs = now - lastAnalysisFrameAtMs_;
    if (frameIntervalMs >= 20UL && frameIntervalMs <= 120UL) {
      analysisFrameIntervalMs_ = static_cast<uint16_t>(
        ((analysisFrameIntervalMs_ * 7UL) + frameIntervalMs) / 8UL
      );
    }
  }
  lastAnalysisFrameAtMs_ = now;

  uint32_t pulseStrength =
    (transientLevel > onsetThreshold) ? (transientLevel - onsetThreshold) : 0UL;
  pulseStrength >>= kAnalysisPulseScaleShift;
  if (pulseStrength > 255UL) {
    pulseStrength = 255UL;
  }

  pulseHistory_[pulseHistoryIndex_] = static_cast<uint8_t>(pulseStrength);
  pulseHistoryIndex_ = static_cast<uint8_t>((pulseHistoryIndex_ + 1U) % kAnalysisPulseHistorySize);
  if (pulseHistoryCount_ < kAnalysisPulseHistorySize) {
    pulseHistoryCount_++;
  }
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
      clockBpm_ = 0;
      clockSubdivisionCandidateBpm_ = 0;
      clockSubdivisionCandidateCount_ = 0;
      onsetTimestampCount_ = 0;
      onsetIntervalCount_ = 0;
      pulseHistoryCount_ = 0;
      pulseHistoryIndex_ = 0;
      lastAnalysisFrameAtMs_ = 0;
      for (uint32_t& bucketScore : tempoBucketScores_) {
        bucketScore = 0;
      }
    }
    beatConfidence_ = 0;
  }

  const uint32_t energySpan =
    (analysisSlowLevel_ > analysisFloor_) ? (analysisSlowLevel_ - analysisFloor_) : 0;
  const uint32_t onsetThreshold = kAnalysisOnsetMinimum + (energySpan / kAnalysisOnsetDivisor);
  recordPulseFrame(now, transientLevel, musicPresent_ ? onsetThreshold : UINT32_MAX);

  if (!musicPresent_) {
    return;
  }

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
    const uint32_t halfIntervalMinMs =
      (expectedIntervalMs * kAnalysisHalfIntervalMinPercent) / 100UL;
    const uint32_t halfIntervalMaxMs =
      (expectedIntervalMs * kAnalysisHalfIntervalMaxPercent) / 100UL;
    if (intervalMs < earlyIntervalMs &&
        (intervalMs < halfIntervalMinMs || intervalMs > halfIntervalMaxMs)) {
      return;
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

}


void PdmMicrophone::updateTempoEstimate() {
  if (onsetTimestampCount_ < 3) {
    return;
  }

  const uint16_t previousDetectedBpm = detectedBpm_;
  const uint16_t previousClockBpm = clockBpm_;
  const uint16_t previousFamilyBpm =
    (previousDetectedBpm == 0) ? 0 : canonicalTempoFamilyBpm(previousDetectedBpm);
  uint32_t instantScores[kAnalysisTempoBucketCount] = {0};
  uint32_t instantErrorSums[kAnalysisTempoBucketCount] = {0};
  uint8_t instantMatchCounts[kAnalysisTempoBucketCount] = {0};
  uint32_t primaryScores[kAnalysisTempoBucketCount] = {0};
  uint8_t primaryMatchCounts[kAnalysisTempoBucketCount] = {0};
  uint32_t exactIntervalScores[kAnalysisTempoBucketCount] = {0};
  uint32_t pulseScores[kAnalysisTempoBucketCount] = {0};
  uint32_t combinedScores[kAnalysisTempoBucketCount] = {0};
  uint32_t familyScores[kAnalysisTempoBucketCount] = {0};
  uint8_t pulseFrames[kAnalysisPulseHistorySize] = {0};

  if (pulseHistoryCount_ > 0) {
    for (uint8_t i = 0; i < pulseHistoryCount_; ++i) {
      const uint8_t historyIndex = static_cast<uint8_t>(
        (pulseHistoryIndex_ + kAnalysisPulseHistorySize - pulseHistoryCount_ + i) %
        kAnalysisPulseHistorySize
      );
      pulseFrames[i] = pulseHistory_[historyIndex];
    }
  }

  for (uint16_t bpm = kAnalysisMinTempoBpm; bpm <= kAnalysisMaxTempoBpm; ++bpm) {
    uint32_t score = 0;
    uint32_t errorSum = 0;
    uint8_t matchCount = 0;
    uint32_t primaryScore = 0;
    uint8_t primaryMatchCount = 0;

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
        const uint32_t matchScore =
          (recencyWeight * multipleWeight * 8UL) + (toleranceMs - errorMs);
        score += matchScore;
        errorSum += errorMs;
        matchCount++;

        if (candidateMultiple == 1UL) {
          primaryScore += matchScore * 2UL;
          if (primaryMatchCount < 0xFFU) {
            primaryMatchCount++;
          }
        } else if (candidateMultiple == 2UL) {
          primaryScore += matchScore / 4UL;
        }
      }
    }

    const uint8_t bucketIndex = static_cast<uint8_t>(bpm - kAnalysisMinTempoBpm);
    instantScores[bucketIndex] = score;
    instantErrorSums[bucketIndex] = errorSum;
    instantMatchCounts[bucketIndex] = matchCount;
    primaryScores[bucketIndex] = primaryScore;
    primaryMatchCounts[bucketIndex] = primaryMatchCount;
  }

  for (uint8_t intervalIndex = 0; intervalIndex < onsetIntervalCount_; ++intervalIndex) {
    const uint32_t intervalMs = onsetIntervalsMs_[intervalIndex];
    const uint32_t recencyWeight = 2UL + intervalIndex;

    for (uint16_t bpm = kAnalysisMinTempoBpm; bpm <= kAnalysisMaxTempoBpm; ++bpm) {
      const uint8_t bucketIndex = static_cast<uint8_t>(bpm - kAnalysisMinTempoBpm);
      const uint32_t expectedIntervalMs = 60000UL / bpm;
      uint32_t toleranceMs =
        (expectedIntervalMs * kAnalysisTempoDirectTolerancePercent) / 100UL;
      if (toleranceMs < 24UL) {
        toleranceMs = 24UL;
      }

      const uint32_t errorMs = static_cast<uint32_t>(abs(
        static_cast<int32_t>(intervalMs) - static_cast<int32_t>(expectedIntervalMs)
      ));
      if (errorMs > toleranceMs) {
        continue;
      }

      exactIntervalScores[bucketIndex] +=
        (recencyWeight * 24UL) + ((toleranceMs - errorMs) * 2UL);
    }
  }

  if (pulseHistoryCount_ >= 8 && analysisFrameIntervalMs_ != 0) {
    for (uint16_t bpm = kAnalysisMinTempoBpm; bpm <= kAnalysisMaxTempoBpm; ++bpm) {
      const uint8_t bucketIndex = static_cast<uint8_t>(bpm - kAnalysisMinTempoBpm);
      const uint32_t expectedIntervalMs = 60000UL / bpm;
      const uint32_t lagFrames =
        (expectedIntervalMs + (analysisFrameIntervalMs_ / 2U)) / analysisFrameIntervalMs_;
      if (lagFrames < 2UL || lagFrames >= pulseHistoryCount_) {
        continue;
      }

      uint32_t pulseScore = 0;
      for (uint8_t newer = static_cast<uint8_t>(lagFrames); newer < pulseHistoryCount_; ++newer) {
        const uint8_t currentPulse = pulseFrames[newer];
        const uint8_t laggedPulse = pulseFrames[newer - static_cast<uint8_t>(lagFrames)];
        if (currentPulse == 0 || laggedPulse == 0) {
          continue;
        }
        pulseScore += (currentPulse < laggedPulse) ? currentPulse : laggedPulse;
      }

      pulseScores[bucketIndex] = pulseScore;
    }
  }

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    tempoBucketScores_[i] -= tempoBucketScores_[i] >> kAnalysisTempoBucketDecayShift;
    tempoBucketScores_[i] += instantScores[i] + primaryScores[i] + (pulseScores[i] / 2UL);
  }

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    if (instantMatchCounts[i] == 0 || tempoBucketScores_[i] == 0) {
      continue;
    }

    const uint16_t bpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + i);
    uint32_t score = tempoBucketScores_[i] +
                     (exactIntervalScores[i] * 2UL) +
                     (primaryScores[i] * kAnalysisTempoPrimaryScoreWeight) +
                     (pulseScores[i] * kAnalysisPulseScoreWeight);
    if (i > 0) {
      score += (tempoBucketScores_[i - 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }
    if ((i + 1U) < kAnalysisTempoBucketCount) {
      score += (tempoBucketScores_[i + 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }

    if (previousDetectedBpm != 0) {
      const uint16_t bpmDelta = bpmDifference(previousDetectedBpm, bpm);
      if (bpmDelta <= 2U) {
        score += static_cast<uint32_t>((3U - bpmDelta) * kAnalysisTempoContinuityBonus * 6UL);
      } else if (beatConfidence_ >= kAnalysisTempoHoldConfidence &&
                 inSameTempoFamily(previousDetectedBpm, bpm)) {
        score += static_cast<uint32_t>(kAnalysisTempoContinuityBonus * 4UL);
      }
    }

    combinedScores[i] = score;
    const uint16_t familyBpm = canonicalTempoFamilyBpm(bpm);
    const uint8_t familyIndex = static_cast<uint8_t>(familyBpm - kAnalysisMinTempoBpm);
    familyScores[familyIndex] += score;
  }

  uint16_t bestFamilyBpm = 0;
  uint32_t bestFamilyScore = 0;
  uint32_t secondBestFamilyScore = 0;

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    if (familyScores[i] == 0) {
      continue;
    }

    uint32_t familyScore = familyScores[i];
    const uint16_t familyBpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + i);
    if (previousFamilyBpm != 0 && familyBpm == previousFamilyBpm) {
      familyScore += static_cast<uint32_t>(kAnalysisTempoContinuityBonus * 12UL);
    }

    if (familyScore > bestFamilyScore) {
      secondBestFamilyScore = bestFamilyScore;
      bestFamilyScore = familyScore;
      bestFamilyBpm = familyBpm;
    } else if (familyScore > secondBestFamilyScore) {
      secondBestFamilyScore = familyScore;
    }
  }

  if (previousFamilyBpm >= kAnalysisMinTempoBpm && previousFamilyBpm <= kAnalysisMaxTempoBpm &&
      beatConfidence_ >= kAnalysisTempoHoldConfidence) {
    const uint8_t currentFamilyIndex =
      static_cast<uint8_t>(previousFamilyBpm - kAnalysisMinTempoBpm);
    const uint32_t currentFamilyScore = familyScores[currentFamilyIndex];
    if (bestFamilyBpm == 0 && currentFamilyScore > 0) {
      bestFamilyBpm = previousFamilyBpm;
      bestFamilyScore = currentFamilyScore;
    } else if (bestFamilyBpm != 0 && currentFamilyScore > 0 &&
               bestFamilyBpm != previousFamilyBpm) {
      if ((bestFamilyScore * 100UL) < (currentFamilyScore * kAnalysisTempoHoldPercent)) {
        secondBestFamilyScore = bestFamilyScore;
        bestFamilyBpm = previousFamilyBpm;
        bestFamilyScore = currentFamilyScore;
      }
    }
  }

  if (bestFamilyBpm == 0 || bestFamilyScore == 0) {
    beatConfidence_ = (beatConfidence_ > 6U) ? static_cast<uint8_t>(beatConfidence_ - 6U) : 0;
    return;
  }

  uint16_t bestBpm = 0;
  uint8_t bestBucketIndex = 0;
  uint32_t bestRepresentativeScore = 0;
  uint32_t bestErrorSum = 0;
  uint8_t bestMatchCount = 0;
  const uint8_t familyAnchorIndex =
    static_cast<uint8_t>(bestFamilyBpm - kAnalysisMinTempoBpm);
  const uint32_t familyAnchorPrimaryScore = primaryScores[familyAnchorIndex];
  const uint8_t familyAnchorPrimaryMatches = primaryMatchCounts[familyAnchorIndex];

  for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
    const uint16_t bpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + i);
    if (canonicalTempoFamilyBpm(bpm) != bestFamilyBpm) {
      continue;
    }
    if (instantMatchCounts[i] == 0 || combinedScores[i] == 0) {
      continue;
    }

    uint32_t candidateScore =
      (instantScores[i] * kAnalysisTempoRepresentativePairwiseWeight) +
      (primaryScores[i] * (kAnalysisTempoPrimaryScoreWeight + 2UL)) +
      exactIntervalScores[i] +
      (pulseScores[i] * (kAnalysisPulseScoreWeight + 2UL)) +
      (tempoBucketScores_[i] / kAnalysisTempoRepresentativeMemoryDivisor);

    if (i > 0) {
      candidateScore += (instantScores[i - 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }
    if ((i + 1U) < kAnalysisTempoBucketCount) {
      candidateScore += (instantScores[i + 1] * kAnalysisTempoNeighborWeightPercent) / 100UL;
    }

    const uint16_t familyDistance = bpmDifference(bestFamilyBpm, bpm);
    if (familyDistance == 0) {
      candidateScore += static_cast<uint32_t>(kAnalysisTempoContinuityBonus *
                                              kAnalysisTempoRepresentativeFamilyBonus);
    } else {
      uint32_t familyPenalty = static_cast<uint32_t>(familyDistance) *
                               kAnalysisTempoContinuityBonus *
                               kAnalysisTempoRepresentativeFamilyPenalty;
      if (primaryScores[i] > familyAnchorPrimaryScore ||
          primaryMatchCounts[i] > familyAnchorPrimaryMatches) {
        familyPenalty /= 4UL;
      }
      candidateScore = (candidateScore > familyPenalty) ? (candidateScore - familyPenalty) : 0UL;
    }

    if (previousDetectedBpm != 0 && inSameTempoFamily(previousDetectedBpm, bpm)) {
      const uint16_t bpmDelta = bpmDifference(previousDetectedBpm, bpm);
      if (bpmDelta <= 2U) {
        candidateScore += static_cast<uint32_t>((3U - bpmDelta) * kAnalysisTempoContinuityBonus * 8UL);
      } else if (bpmDelta <= 6U) {
        candidateScore += static_cast<uint32_t>((7U - bpmDelta) * kAnalysisTempoContinuityBonus * 2UL);
      }
    }

    if (candidateScore > bestRepresentativeScore) {
      bestRepresentativeScore = candidateScore;
      bestBpm = bpm;
      bestBucketIndex = i;
      bestErrorSum = instantErrorSums[i];
      bestMatchCount = instantMatchCounts[i];
    }
  }

  if (bestBpm == 0 || bestMatchCount == 0 || bestRepresentativeScore == 0) {
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

  uint32_t coverageScore = (bestMatchCount * 100UL) / kAnalysisTempoCoverageTarget;
  if (coverageScore > 100UL) {
    coverageScore = 100UL;
  }

  const uint32_t separationScore =
    (bestFamilyScore > 0 && bestFamilyScore > secondBestFamilyScore)
      ? ((bestFamilyScore - secondBestFamilyScore) * 100UL) / bestFamilyScore
      : 0UL;

  uint32_t primaryCoverageScore =
    (primaryMatchCounts[bestBucketIndex] * 100UL) / kAnalysisTempoPrimaryCoverageTarget;
  if (primaryCoverageScore > 100UL) {
    primaryCoverageScore = 100UL;
  }

  uint32_t continuityScore = 60UL;
  if (previousFamilyBpm != 0) {
    if (previousFamilyBpm == bestFamilyBpm) {
      continuityScore = 100UL;
    } else {
      const uint16_t familyDelta = bpmDifference(previousFamilyBpm, bestFamilyBpm);
      const uint32_t continuityPenalty = familyDelta * 6UL;
      continuityScore = (continuityPenalty >= 100UL) ? 0UL : (100UL - continuityPenalty);
    }
  }

  const uint32_t confidence =
    ((precisionScore * 2UL) + coverageScore + primaryCoverageScore + separationScore +
     continuityScore) / 6UL;
  beatConfidence_ = static_cast<uint8_t>((confidence > 100UL) ? 100UL : confidence);

  const uint16_t bestRawBpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + bestBucketIndex);
  uint16_t resolvedRawBpm = bestRawBpm;
  if (previousDetectedBpm != 0 && inSameTempoFamily(previousDetectedBpm, bestRawBpm)) {
    const uint8_t previousBucketIndex =
      static_cast<uint8_t>(previousDetectedBpm - kAnalysisMinTempoBpm);
    const uint32_t previousFamilySupport =
      (primaryScores[previousBucketIndex] * kAnalysisTempoPrimaryScoreWeight) +
      exactIntervalScores[previousBucketIndex] + pulseScores[previousBucketIndex];
    const uint32_t bestFamilySupport =
      (primaryScores[bestBucketIndex] * kAnalysisTempoPrimaryScoreWeight) +
      exactIntervalScores[bestBucketIndex] + pulseScores[bestBucketIndex];

    if (bpmDifference(previousDetectedBpm, bestRawBpm) >= 6U && previousFamilySupport > 0 &&
        (bestFamilySupport * 100UL) <
          (previousFamilySupport * kAnalysisTempoSameFamilySwitchPercent)) {
      resolvedRawBpm = previousDetectedBpm;
    } else if (bpmDifference(previousDetectedBpm, bestRawBpm) <= 2U) {
      resolvedRawBpm =
        static_cast<uint16_t>((previousDetectedBpm + bestRawBpm + 1U) / 2U);
    }
  }
  detectedBpm_ = resolvedRawBpm;

  uint16_t resolvedClockBpm = detectedBpm_;
  const uint16_t canonicalClockBpm = canonicalTempoFamilyBpm(detectedBpm_);
  if (canonicalClockBpm > detectedBpm_ && canonicalClockBpm >= kAnalysisTempoFamilyPreferredMinBpm) {
    uint32_t fastFamilyEvidence = 0;
    uint8_t fastFamilyMatches = 0;

    for (uint8_t i = 0; i < kAnalysisTempoBucketCount; ++i) {
      const uint16_t bpm = static_cast<uint16_t>(kAnalysisMinTempoBpm + i);
      if (!inSameTempoFamily(detectedBpm_, bpm)) {
        continue;
      }
      if ((bpm + 6U) < canonicalClockBpm) {
        continue;
      }

      const uint32_t evidence =
        (primaryScores[i] * kAnalysisTempoPrimaryScoreWeight) +
        exactIntervalScores[i] +
        (pulseScores[i] * kAnalysisPulseScoreWeight);
      if (evidence > fastFamilyEvidence) {
        fastFamilyEvidence = evidence;
        fastFamilyMatches = primaryMatchCounts[i];
      }
    }

    if (fastFamilyEvidence > 0 &&
        (fastFamilyMatches > 0 ||
         (fastFamilyEvidence * 100UL) >= (bestRepresentativeScore * 35UL))) {
      resolvedClockBpm = canonicalClockBpm;
    }
  }

  uint16_t targetClockBpm = resolvedClockBpm;
  if (previousClockBpm != 0 && inSameTempoFamily(previousClockBpm, resolvedClockBpm) &&
      (resolvedClockBpm + 6U) < previousClockBpm) {
    if (clockSubdivisionCandidateCount_ == 0 ||
        bpmDifference(clockSubdivisionCandidateBpm_, resolvedClockBpm) > 2U) {
      clockSubdivisionCandidateBpm_ = resolvedClockBpm;
      clockSubdivisionCandidateCount_ = 1;
      targetClockBpm = previousClockBpm;
    } else if (clockSubdivisionCandidateCount_ < kAnalysisClockSubdivisionSwitchHits) {
      clockSubdivisionCandidateCount_++;
      targetClockBpm = previousClockBpm;
    } else {
      targetClockBpm = clockSubdivisionCandidateBpm_;
    }
  } else {
    clockSubdivisionCandidateBpm_ = 0;
    clockSubdivisionCandidateCount_ = 0;
  }

  if (previousClockBpm != 0 && inSameTempoFamily(previousClockBpm, targetClockBpm) &&
      bpmDifference(previousClockBpm, targetClockBpm) <= 2U) {
    clockBpm_ = static_cast<uint16_t>((previousClockBpm + targetClockBpm + 1U) / 2U);
  } else {
    clockBpm_ = targetClockBpm;
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

bool startMicrophoneRecording(uint32_t durationMs) {
  if (gPrimaryMicrophone == nullptr) {
    Serial.println("record=abort reason=mic_not_available");
    return false;
  }

  return gPrimaryMicrophone->requestRecording(durationMs);
}

}  // namespace decaflash::brain
