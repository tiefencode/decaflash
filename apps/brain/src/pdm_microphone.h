#pragma once

#include <Arduino.h>

namespace decaflash::brain {

class PdmMicrophone {
 public:
  bool begin();
  void update();
  bool ready() const;
  uint8_t meterLevel() const;

 private:
  void resetWindowStats();
  void accumulateSamples(const int16_t* samples, size_t sampleCount);
  void printReport(uint32_t now);
  void updateMeterLevel();

  bool ready_ = false;
  bool reportedReadError_ = false;
  int32_t dcEstimate_ = 0;
  uint32_t envelopeLevel_ = 0;
  uint32_t noiseFloor_ = 0;
  uint32_t signalCeiling_ = 160;
  uint8_t meterLevel_ = 0;
  uint32_t lastReportAtMs_ = 0;
  uint32_t lastFramePrintAtMs_ = 0;
  uint32_t sampleCount_ = 0;
  int16_t rawMinSample_ = INT16_MAX;
  int16_t rawMaxSample_ = INT16_MIN;
  uint32_t centeredAbsSum_ = 0;
  uint16_t centeredPeakAbs_ = 0;
  uint8_t lastFrameCount_ = 0;
  int16_t lastFrame_[8] = {0};
};

}  // namespace decaflash::brain
