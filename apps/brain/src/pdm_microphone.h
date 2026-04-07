#pragma once

#include <Arduino.h>

namespace decaflash::brain {

class PdmMicrophone {
 public:
  bool begin();
  void update();
  bool requestRecording(uint32_t durationMs);
  bool ready() const;
  bool recordingActive() const;
  bool recordingReady() const;
  uint32_t recordingSampleRateHz() const;
  size_t recordedSampleCount() const;
  const int16_t* recordedSamples() const;
  void clearRecording();
  uint8_t meterLevel() const;
  bool musicPresent() const;
  uint16_t detectedBpm() const;
  uint16_t clockBpm() const;
  uint8_t beatConfidence() const;
  uint32_t lastOnsetAtMs() const;

 private:
  bool ensureRecordingBufferCapacity(size_t sampleCapacity);
  void beginRequestedRecording(uint32_t now);
  void finishRecording(uint32_t now);
  void resetWindowStats();
  void accumulateSamples(const int16_t* samples, size_t sampleCount);
  void printReport(uint32_t now);
  void recordPulseFrame(uint32_t now, uint32_t transientLevel, uint32_t onsetThreshold);
  void updateAnalysis(uint32_t now, uint32_t blockLevel, uint16_t peakLevel);
  void updateMeterLevel(uint32_t blockLevel);
  void registerOnset(uint32_t now, uint32_t onsetStrength, uint32_t intervalMs);
  void updateTempoEstimate();

  bool ready_ = false;
  bool reportedReadError_ = false;
  int32_t dcEstimate_ = 0;
  uint32_t envelopeLevel_ = 0;
  uint32_t blockLevel_ = 0;
  uint32_t meterFastLevel_ = 0;
  uint32_t meterSlowLevel_ = 0;
  uint32_t meterDisplayLevel_ = 0;
  uint32_t noiseFloor_ = 0;
  uint32_t signalCeiling_ = 160;
  uint8_t meterLevel_ = 0;
  uint8_t quietCycleCount_ = 0;
  uint32_t lastReportAtMs_ = 0;
  uint32_t lastFramePrintAtMs_ = 0;
  uint32_t sampleCount_ = 0;
  int16_t rawMinSample_ = INT16_MAX;
  int16_t rawMaxSample_ = INT16_MIN;
  uint32_t centeredAbsSum_ = 0;
  uint32_t updateAbsSum_ = 0;
  uint32_t updateSampleCount_ = 0;
  uint16_t centeredPeakAbs_ = 0;
  uint16_t updatePeakAbs_ = 0;
  uint8_t lastFrameCount_ = 0;
  int16_t lastFrame_[8] = {0};
  uint32_t analysisFastLevel_ = 0;
  uint32_t analysisSlowLevel_ = 0;
  uint32_t analysisFloor_ = 0;
  uint32_t onsetStrength_ = 0;
  uint32_t lastOnsetAtMs_ = 0;
  uint16_t detectedBpm_ = 0;
  uint16_t clockBpm_ = 0;
  uint16_t clockSubdivisionCandidateBpm_ = 0;
  uint8_t clockSubdivisionCandidateCount_ = 0;
  uint8_t beatConfidence_ = 0;
  bool musicPresent_ = false;
  uint8_t onsetTimestampCount_ = 0;
  uint32_t onsetTimestampsMs_[8] = {0};
  uint8_t onsetIntervalCount_ = 0;
  uint32_t onsetIntervalsMs_[8] = {0};
  uint32_t tempoBucketScores_[101] = {0};
  uint32_t lastAnalysisFrameAtMs_ = 0;
  uint16_t analysisFrameIntervalMs_ = 64;
  uint8_t pulseHistoryCount_ = 0;
  uint8_t pulseHistoryIndex_ = 0;
  uint8_t pulseHistory_[64] = {0};
  bool recordingRequested_ = false;
  bool recordingActive_ = false;
  bool recordingReady_ = false;
  uint32_t requestedRecordingDurationMs_ = 0;
  uint32_t recordingStartedAtMs_ = 0;
  uint32_t recordingFinishedAtMs_ = 0;
  size_t recordingBufferCapacity_ = 0;
  size_t recordingTargetSampleCount_ = 0;
  size_t recordingSampleCount_ = 0;
  uint32_t recordingInputSampleCount_ = 0;
  int16_t* recordingBuffer_ = nullptr;
};

bool startMicrophoneRecording(uint32_t durationMs = 0);

}  // namespace decaflash::brain
