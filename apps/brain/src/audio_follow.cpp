#include "audio_follow.h"

#include <Arduino.h>

namespace decaflash::brain::audio_follow {

namespace {

static constexpr uint8_t AUDIO_SYNC_CANDIDATE_CONFIDENCE = 68;
static constexpr uint8_t AUDIO_SYNC_LOCKED_CONFIDENCE = 62;
static constexpr uint8_t AUDIO_SYNC_REQUIRED_ONSETS = 3;
static constexpr uint8_t AUDIO_SYNC_CANDIDATE_BPM_TOLERANCE = 4;
static constexpr uint8_t AUDIO_SYNC_MAX_BPM_STEP = 1;
static constexpr uint8_t AUDIO_SYNC_INTERVAL_MIN_PERCENT = 88;
static constexpr uint8_t AUDIO_SYNC_INTERVAL_MAX_PERCENT = 112;
static constexpr uint8_t AUDIO_SYNC_DOUBLE_INTERVAL_MIN_PERCENT = 176;
static constexpr uint8_t AUDIO_SYNC_DOUBLE_INTERVAL_MAX_PERCENT = 224;
static constexpr uint32_t AUDIO_SYNC_LOST_MS = 4000;
static constexpr uint32_t AUDIO_SYNC_HARD_RESYNC_MS = 90;
static constexpr uint8_t AUDIO_SYNC_SOFT_TRIM_DIVISOR = 3;
static constexpr uint8_t AUDIO_SYNC_PRE_RESYNC_TRIM_DIVISOR = 2;
static constexpr uint8_t AUDIO_SYNC_FOLLOW_REQUIRED_UPDATES = 2;
static constexpr uint8_t AUDIO_SYNC_RESYNC_REQUIRED_HITS = 2;

enum class AudioSyncState : uint8_t {
  Searching = 0,
  Locked = 1,
  Holdover = 2,
};

AudioSyncState audioSyncState = AudioSyncState::Searching;
uint8_t audioSyncCandidateCount = 0;
uint16_t audioSyncCandidateBpm = 0;
uint16_t audioFollowCandidateBpm = 0;
uint8_t audioFollowCandidateCount = 0;
int8_t audioSyncPhaseMissSign = 0;
uint8_t audioSyncPhaseMissCount = 0;
uint32_t lastAudioOnsetSeenAtMs = 0;
uint32_t lastAudioLockAtMs = 0;

uint16_t clampBpm(uint16_t bpm, uint16_t minBpm, uint16_t maxBpm) {
  if (bpm < minBpm) {
    return minBpm;
  }

  if (bpm > maxBpm) {
    return maxBpm;
  }

  return bpm;
}

uint16_t bpmDifference(uint16_t left, uint16_t right) {
  return (left > right) ? (left - right) : (right - left);
}

const char* audioSyncStateName(AudioSyncState state) {
  switch (state) {
    case AudioSyncState::Locked:
      return "locked";

    case AudioSyncState::Holdover:
      return "holdover";

    case AudioSyncState::Searching:
    default:
      return "searching";
  }
}

bool audioSyncHasLock() {
  return audioSyncState == AudioSyncState::Locked ||
         audioSyncState == AudioSyncState::Holdover;
}

void resetAudioFollowAdjustment() {
  audioFollowCandidateBpm = 0;
  audioFollowCandidateCount = 0;
  audioSyncPhaseMissSign = 0;
  audioSyncPhaseMissCount = 0;
}

uint32_t audioSyncHoldoverEnterMs(uint32_t beatIntervalMs) {
  if (beatIntervalMs == 0) {
    return 1500;
  }

  // Wait about one full bar before dropping into holdover so ordinary
  // detection gaps do not flap the state machine on real music.
  uint32_t holdoverMs = beatIntervalMs * 4UL;
  if (holdoverMs >= AUDIO_SYNC_LOST_MS) {
    holdoverMs = AUDIO_SYNC_LOST_MS - 250UL;
  }
  return holdoverMs;
}

void setAudioSyncState(AudioSyncState nextState,
                       uint32_t now,
                       uint16_t bpm,
                       const char* reason) {
  if (audioSyncState == nextState) {
    return;
  }

  const uint32_t ageMs = (lastAudioLockAtMs == 0) ? 0 : (now - lastAudioLockAtMs);
  audioSyncState = nextState;

  switch (nextState) {
    case AudioSyncState::Locked:
      Serial.printf("AUDIO-SYNC: state=%s reason=%s bpm=%u\n",
                    audioSyncStateName(nextState),
                    reason,
                    static_cast<unsigned>(bpm));
      return;

    case AudioSyncState::Holdover:
      Serial.printf("AUDIO-SYNC: state=%s reason=%s bpm=%u age_ms=%lu\n",
                    audioSyncStateName(nextState),
                    reason,
                    static_cast<unsigned>(bpm),
                    static_cast<unsigned long>(ageMs));
      return;

    case AudioSyncState::Searching:
    default:
      Serial.printf("AUDIO-SYNC: state=%s reason=%s age_ms=%lu\n",
                    audioSyncStateName(nextState),
                    reason,
                    static_cast<unsigned long>(ageMs));
      return;
  }
}

}  // namespace

void reset() {
  audioSyncState = AudioSyncState::Searching;
  audioSyncCandidateCount = 0;
  audioSyncCandidateBpm = 0;
  resetAudioFollowAdjustment();
  lastAudioOnsetSeenAtMs = 0;
  lastAudioLockAtMs = 0;
}

Output update(const Input& input) {
  Output output = {};

  if (!input.brainLive) {
    reset();
    return output;
  }

  const bool hasFreshOnset =
    input.musicPresent &&
    input.detectedBpm != 0 &&
    input.onsetAtMs != 0 &&
    input.onsetAtMs != lastAudioOnsetSeenAtMs;

  if (audioSyncHasLock()) {
    const uint32_t elapsedSinceAcceptedOnsetMs =
      (lastAudioLockAtMs == 0) ? 0 : (input.now - lastAudioLockAtMs);

    if (audioSyncState == AudioSyncState::Locked &&
        !hasFreshOnset &&
        elapsedSinceAcceptedOnsetMs > audioSyncHoldoverEnterMs(input.beatIntervalMs)) {
      resetAudioFollowAdjustment();
      setAudioSyncState(AudioSyncState::Holdover, input.now, input.currentBpm, "signal_gap");
    }

    if (elapsedSinceAcceptedOnsetMs > AUDIO_SYNC_LOST_MS) {
      setAudioSyncState(AudioSyncState::Searching, input.now, input.currentBpm, "signal_lost");
      reset();
      return output;
    }
  }

  if (!hasFreshOnset) {
    return output;
  }

  const uint32_t previousAudioOnsetSeenAtMs = lastAudioOnsetSeenAtMs;
  lastAudioOnsetSeenAtMs = input.onsetAtMs;

  const uint16_t targetBpm = clampBpm(input.detectedBpm, input.minBpm, input.maxBpm);

  if (audioSyncState == AudioSyncState::Searching) {
    if (input.beatConfidence < AUDIO_SYNC_CANDIDATE_CONFIDENCE) {
      audioSyncCandidateCount = 0;
      audioSyncCandidateBpm = 0;
      return output;
    }

    if (audioSyncCandidateCount == 0 ||
        bpmDifference(audioSyncCandidateBpm, targetBpm) > AUDIO_SYNC_CANDIDATE_BPM_TOLERANCE) {
      audioSyncCandidateBpm = targetBpm;
      audioSyncCandidateCount = 1;
      return output;
    }

    audioSyncCandidateBpm =
      static_cast<uint16_t>((audioSyncCandidateBpm + targetBpm + 1U) / 2U);
    if (audioSyncCandidateCount < AUDIO_SYNC_REQUIRED_ONSETS) {
      audioSyncCandidateCount++;
    }

    if (audioSyncCandidateCount < AUDIO_SYNC_REQUIRED_ONSETS) {
      return output;
    }

    const uint16_t lockedBpm = audioSyncCandidateBpm;
    audioSyncCandidateCount = 0;
    audioSyncCandidateBpm = 0;
    resetAudioFollowAdjustment();
    lastAudioLockAtMs = input.now;
    output.setBpm = true;
    output.bpm = lockedBpm;
    output.setNextBeatAtMs = true;
    output.nextBeatAtMs = input.onsetAtMs;
    output.queueSyncBeatDot = true;
    output.requestClockSync = true;
    setAudioSyncState(AudioSyncState::Locked,
                      input.now,
                      lockedBpm,
                      "acquired");
    return output;
  }

  if (input.beatConfidence < AUDIO_SYNC_LOCKED_CONFIDENCE) {
    if (audioSyncState == AudioSyncState::Locked &&
        lastAudioLockAtMs != 0 &&
        (input.now - lastAudioLockAtMs) > audioSyncHoldoverEnterMs(input.beatIntervalMs)) {
      resetAudioFollowAdjustment();
      setAudioSyncState(AudioSyncState::Holdover, input.now, input.currentBpm, "low_confidence");
    }
    return output;
  }

  const bool wasHoldover = audioSyncState == AudioSyncState::Holdover;
  const uint32_t observedIntervalMs =
    (previousAudioOnsetSeenAtMs == 0) ? 0 : (input.onsetAtMs - previousAudioOnsetSeenAtMs);
  if (!wasHoldover && observedIntervalMs != 0) {
    const uint32_t minIntervalMs =
      (input.beatIntervalMs * AUDIO_SYNC_INTERVAL_MIN_PERCENT) / 100UL;
    const uint32_t maxIntervalMs =
      (input.beatIntervalMs * AUDIO_SYNC_INTERVAL_MAX_PERCENT) / 100UL;
    const uint32_t doubleMinIntervalMs =
      (input.beatIntervalMs * AUDIO_SYNC_DOUBLE_INTERVAL_MIN_PERCENT) / 100UL;
    const uint32_t doubleMaxIntervalMs =
      (input.beatIntervalMs * AUDIO_SYNC_DOUBLE_INTERVAL_MAX_PERCENT) / 100UL;

    if (observedIntervalMs < minIntervalMs) {
      return output;
    }

    if (observedIntervalMs > maxIntervalMs &&
        (observedIntervalMs < doubleMinIntervalMs || observedIntervalMs > doubleMaxIntervalMs)) {
      if (audioSyncState == AudioSyncState::Locked &&
          lastAudioLockAtMs != 0 &&
          (input.now - lastAudioLockAtMs) > audioSyncHoldoverEnterMs(input.beatIntervalMs)) {
        resetAudioFollowAdjustment();
        setAudioSyncState(AudioSyncState::Holdover,
                          input.now,
                          input.currentBpm,
                          "interval_outlier");
      }
      return output;
    }
  }

  lastAudioLockAtMs = input.now;

  if (wasHoldover) {
    resetAudioFollowAdjustment();
    setAudioSyncState(AudioSyncState::Locked,
                      input.now,
                      input.currentBpm,
                      "reacquired");
  }

  if (targetBpm == input.currentBpm) {
    audioFollowCandidateBpm = 0;
    audioFollowCandidateCount = 0;
  } else {
    if (audioFollowCandidateCount == 0 || audioFollowCandidateBpm != targetBpm) {
      audioFollowCandidateBpm = targetBpm;
      audioFollowCandidateCount = 1;
    } else if (audioFollowCandidateCount < AUDIO_SYNC_FOLLOW_REQUIRED_UPDATES) {
      audioFollowCandidateCount++;
    }

    if (audioFollowCandidateCount >= AUDIO_SYNC_FOLLOW_REQUIRED_UPDATES) {
      const int32_t bpmDelta =
        static_cast<int32_t>(targetBpm) - static_cast<int32_t>(input.currentBpm);
      uint16_t steppedBpm = input.currentBpm;
      if (bpmDelta > 0) {
        const uint16_t step = static_cast<uint16_t>(
          (bpmDelta > AUDIO_SYNC_MAX_BPM_STEP) ? AUDIO_SYNC_MAX_BPM_STEP : bpmDelta);
        steppedBpm = static_cast<uint16_t>(input.currentBpm + step);
      } else {
        const int32_t positiveDelta = -bpmDelta;
        const uint16_t step = static_cast<uint16_t>(
          (positiveDelta > AUDIO_SYNC_MAX_BPM_STEP) ? AUDIO_SYNC_MAX_BPM_STEP : positiveDelta);
        steppedBpm = static_cast<uint16_t>(input.currentBpm - step);
      }

      output.setBpm = true;
      output.bpm = steppedBpm;
      audioFollowCandidateBpm = 0;
      audioFollowCandidateCount = 0;
    }
  }

  const int32_t nextScheduledBeatAtMs = static_cast<int32_t>(input.nextBeatAtMs);
  const int32_t previousScheduledBeatAtMs =
    nextScheduledBeatAtMs - static_cast<int32_t>(input.beatIntervalMs);
  const int32_t previousBeatErrorMs =
    static_cast<int32_t>(input.onsetAtMs) - previousScheduledBeatAtMs;
  const int32_t nextBeatErrorMs =
    static_cast<int32_t>(input.onsetAtMs) - nextScheduledBeatAtMs;
  const int32_t phaseErrorMs =
    (abs(previousBeatErrorMs) <= abs(nextBeatErrorMs)) ? previousBeatErrorMs : nextBeatErrorMs;
  const uint32_t absolutePhaseErrorMs = static_cast<uint32_t>(abs(phaseErrorMs));
  uint32_t hardResyncMs = input.beatIntervalMs / 5UL;
  if (hardResyncMs > AUDIO_SYNC_HARD_RESYNC_MS) {
    hardResyncMs = AUDIO_SYNC_HARD_RESYNC_MS;
  }
  if (hardResyncMs < 20UL) {
    hardResyncMs = 20UL;
  }

  if (absolutePhaseErrorMs >= hardResyncMs) {
    const int8_t phaseErrorSign = (phaseErrorMs < 0) ? -1 : 1;
    if (audioSyncPhaseMissSign == phaseErrorSign) {
      if (audioSyncPhaseMissCount < AUDIO_SYNC_RESYNC_REQUIRED_HITS) {
        audioSyncPhaseMissCount++;
      }
    } else {
      audioSyncPhaseMissSign = phaseErrorSign;
      audioSyncPhaseMissCount = 1;
    }

    if (audioSyncPhaseMissCount >= AUDIO_SYNC_RESYNC_REQUIRED_HITS) {
      output.setNextBeatAtMs = true;
      output.nextBeatAtMs = static_cast<uint32_t>(
        static_cast<int32_t>(input.nextBeatAtMs) + phaseErrorMs);
      audioSyncPhaseMissSign = 0;
      audioSyncPhaseMissCount = 0;
      output.queueSyncBeatDot = true;
      output.requestClockSync = true;
      return output;
    }

    output.setNextBeatAtMs = true;
    output.nextBeatAtMs = static_cast<uint32_t>(
      static_cast<int32_t>(input.nextBeatAtMs) +
      (phaseErrorMs / AUDIO_SYNC_PRE_RESYNC_TRIM_DIVISOR));
    return output;
  }

  audioSyncPhaseMissSign = 0;
  audioSyncPhaseMissCount = 0;
  output.setNextBeatAtMs = true;
  output.nextBeatAtMs = static_cast<uint32_t>(
    static_cast<int32_t>(input.nextBeatAtMs) + (phaseErrorMs / AUDIO_SYNC_SOFT_TRIM_DIVISOR));
  return output;
}

}  // namespace decaflash::brain::audio_follow
