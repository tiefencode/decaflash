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
static constexpr uint32_t kMeterFloorMargin = 12;
static constexpr uint32_t kMeterMinimumSpan = 120;

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
  Serial.println("mic=report fields=env avg peak dc raw_p2p samples");
  return true;
}

void PdmMicrophone::update() {
  if (!ready_) {
    return;
  }

  int16_t sampleBuffer[kReadBufferSampleCount] = {0};
  bool readAnySamples = false;

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

  const uint32_t now = millis();
  if (readAnySamples) {
    updateMeterLevel();
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

    centeredAbsSum_ += absCenteredSample;
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

  Serial.printf("mic=level env=%lu avg=%lu peak=%u dc=%ld raw_p2p=%ld samples=%lu\n",
                static_cast<unsigned long>(envelopeLevel_),
                static_cast<unsigned long>(averageLevel),
                static_cast<unsigned>(centeredPeakAbs_),
                static_cast<long>(dcEstimate_),
                static_cast<long>(rawPeakToPeak),
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

void PdmMicrophone::updateMeterLevel() {
  const uint32_t envelope = envelopeLevel_;

  if (noiseFloor_ == 0) {
    noiseFloor_ = envelope;
  } else if (envelope < noiseFloor_) {
    noiseFloor_ = ((noiseFloor_ * 7UL) + envelope) / 8UL;
  } else {
    noiseFloor_ = ((noiseFloor_ * 127UL) + envelope) / 128UL;
  }

  if (envelope > signalCeiling_) {
    signalCeiling_ = envelope;
  } else {
    signalCeiling_ = ((signalCeiling_ * 63UL) + envelope) / 64UL;
  }

  const uint32_t effectiveFloor = noiseFloor_ + kMeterFloorMargin;
  uint32_t span = 0;
  if (signalCeiling_ > effectiveFloor) {
    span = signalCeiling_ - effectiveFloor;
  }
  if (span < kMeterMinimumSpan) {
    span = kMeterMinimumSpan;
  }

  uint32_t normalized = 0;
  if (envelope > effectiveFloor) {
    normalized = envelope - effectiveFloor;
    if (normalized > span) {
      normalized = span;
    }
  }

  meterLevel_ = static_cast<uint8_t>((normalized * 25UL) / span);
}

}  // namespace decaflash::brain
