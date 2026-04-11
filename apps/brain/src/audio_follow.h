#pragma once

#include <stdint.h>

namespace decaflash::brain::audio_follow {

struct Input {
  bool brainLive = false;
  bool musicPresent = false;
  uint16_t detectedBpm = 0;
  uint8_t beatConfidence = 0;
  uint32_t onsetAtMs = 0;
  uint16_t currentBpm = 0;
  uint16_t minBpm = 0;
  uint16_t maxBpm = 0;
  uint32_t beatIntervalMs = 0;
  uint32_t nextBeatAtMs = 0;
  uint32_t now = 0;
};

struct Output {
  bool setBpm = false;
  uint16_t bpm = 0;
  bool setNextBeatAtMs = false;
  uint32_t nextBeatAtMs = 0;
  bool queueSyncBeatDot = false;
  bool requestClockSync = false;
};

void reset();
Output update(const Input& input);

}  // namespace decaflash::brain::audio_follow
