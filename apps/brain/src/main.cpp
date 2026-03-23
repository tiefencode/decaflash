#include <Arduino.h>
#include <M5Atom.h>

#include "command_examples.h"
#include "decaflash_types.h"
#include "espnow_transport.h"
#include "pdm_microphone.h"
#include "protocol.h"

using decaflash::DeviceType;
using decaflash::examples::kFlashCommandCount;
using decaflash::examples::kFlashCommands;
using decaflash::examples::kFlashQuadSkip;
using decaflash::examples::kRgbCommandCount;
using decaflash::examples::kRgbCommands;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::protocol::makeBrainHelloMessage;
using decaflash::protocol::makeClockSyncMessage;
using decaflash::protocol::makeNodeCommandMessage;
using decaflash::protocol::NodeStatusMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t COMMAND_REFRESH_MS = 10000;
static constexpr uint32_t NODE_STALE_MS = 15000;
static constexpr uint16_t DEFAULT_BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;
static constexpr uint16_t BEAT_DOT_FLASH_MS = 140;
static constexpr uint16_t TAP_FLASH_MS = 50;
static constexpr uint32_t METER_REFRESH_MS = 40;
static constexpr uint32_t DEBUG_STATUS_INTERVAL_MS = 1000;
static constexpr uint32_t UI_MODE_DISPLAY_MS = 3000;
static constexpr uint32_t BUTTON_TAP_WINDOW_MS = 450;
static constexpr uint32_t TAP_TEMPO_TIMEOUT_MS = 1600;
static constexpr uint16_t TAP_TEMPO_MIN_BPM = 60;
static constexpr uint16_t TAP_TEMPO_MAX_BPM = 180;
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
static constexpr size_t kModeCount = kFlashCommandCount;
static constexpr size_t kTrackedNodeCapacity = 8;
static constexpr size_t kPendingNodeStatusCapacity = 6;
static_assert(kFlashCommandCount == kRgbCommandCount, "brain mode tables must stay aligned");
uint32_t nextSendAtMs = 0;
bool espNowReady = false;
bool brainLive = false;
uint32_t commandRevision = 1;
uint32_t clockRevision = 1;
uint32_t helloRevision = 1;
uint32_t beatSerial = 0;
uint16_t currentBpm = DEFAULT_BPM;
uint32_t beatIntervalMs = 0;
uint32_t nextBeatAtMs = 0;
uint8_t beatInBar = 1;
uint32_t currentBar = 1;
uint32_t matrixOffAtMs = 0;
uint32_t uiFeedbackUntilMs = 0;
uint32_t tapTempoUiUntilMs = 0;
uint32_t beatDotUntilMs = 0;
uint8_t beatDotBeat = 0;
uint32_t beatDotColorOverride = 0;
bool syncBeatDotPending = false;
size_t currentModeIndex = 0;
bool pendingSingleTap = false;
uint32_t pendingSingleTapAtMs = 0;
uint32_t lastTapAtMs = 0;
uint32_t tapIntervalsMs[3] = {0, 0, 0};
uint8_t tapIntervalCount = 0;
uint32_t lastMeterDrawAtMs = 0;
bool audioClockLocked = false;
uint8_t audioSyncCandidateCount = 0;
uint16_t audioSyncCandidateBpm = 0;
uint16_t audioFollowCandidateBpm = 0;
uint8_t audioFollowCandidateCount = 0;
int8_t audioSyncPhaseMissSign = 0;
uint8_t audioSyncPhaseMissCount = 0;
uint32_t lastAudioOnsetSeenAtMs = 0;
uint32_t lastAudioOnsetHandledAtMs = 0;
uint32_t lastAudioLockAtMs = 0;
uint32_t lastDebugStatusAtMs = 0;
decaflash::brain::PdmMicrophone microphone;
portMUX_TYPE nodeStatusMux = portMUX_INITIALIZER_UNLOCKED;

struct TrackedNode {
  bool active = false;
  uint8_t mac[6] = {};
  NodeStatusMessage status = {};
  uint32_t lastSeenAtMs = 0;
};

struct PendingNodeStatusEvent {
  bool ready = false;
  uint8_t mac[6] = {};
  NodeStatusMessage status = {};
};

TrackedNode trackedNodes[kTrackedNodeCapacity];
PendingNodeStatusEvent pendingNodeStatuses[kPendingNodeStatusCapacity];

static constexpr uint8_t kDigitMasks[5][5] = {
  {0b00100, 0b01100, 0b00100, 0b00100, 0b01110},  // 1
  {0b01110, 0b00010, 0b01110, 0b01000, 0b01110},  // 2
  {0b01110, 0b00010, 0b00110, 0b00010, 0b01110},  // 3
  {0b01010, 0b01010, 0b01110, 0b00010, 0b00010},  // 4
  {0b01110, 0b01000, 0b01110, 0b00010, 0b01110},  // 5
};

uint32_t color(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) |
         static_cast<uint32_t>(b);
}

uint32_t bpmToIntervalMs(uint16_t bpm) {
  return 60000UL / bpm;
}

uint16_t clampBpm(uint16_t bpm) {
  if (bpm < TAP_TEMPO_MIN_BPM) {
    return TAP_TEMPO_MIN_BPM;
  }

  if (bpm > TAP_TEMPO_MAX_BPM) {
    return TAP_TEMPO_MAX_BPM;
  }

  return bpm;
}

uint16_t bpmDifference(uint16_t left, uint16_t right) {
  return (left > right) ? (left - right) : (right - left);
}

const char* nodeKindName(decaflash::NodeKind nodeKind) {
  switch (nodeKind) {
    case decaflash::NodeKind::RgbStrip:
      return "rgb";

    case decaflash::NodeKind::Flashlight:
    default:
      return "flash";
  }
}

void formatMac(const uint8_t* mac, char* buffer, size_t bufferLength) {
  snprintf(buffer,
           bufferLength,
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],
           mac[1],
           mac[2],
           mac[3],
           mac[4],
           mac[5]);
}

void queueNodeStatus(const uint8_t* mac, const NodeStatusMessage& status) {
  portENTER_CRITICAL(&nodeStatusMux);
  size_t slotIndex = 0;
  bool foundFreeSlot = false;

  for (size_t i = 0; i < kPendingNodeStatusCapacity; ++i) {
    if (!pendingNodeStatuses[i].ready) {
      slotIndex = i;
      foundFreeSlot = true;
      break;
    }
  }

  if (!foundFreeSlot) {
    slotIndex = 0;
  }

  pendingNodeStatuses[slotIndex].ready = true;
  memcpy(pendingNodeStatuses[slotIndex].mac, mac, sizeof(pendingNodeStatuses[slotIndex].mac));
  pendingNodeStatuses[slotIndex].status = status;
  portEXIT_CRITICAL(&nodeStatusMux);
}

void onEspNowReceive(const uint8_t* mac, const uint8_t* data, int len) {
  if (len != static_cast<int>(sizeof(NodeStatusMessage))) {
    return;
  }

  NodeStatusMessage status = {};
  memcpy(&status, data, sizeof(status));
  if (!isValidHeader(status.header, decaflash::protocol::MessageType::NodeStatus)) {
    return;
  }

  queueNodeStatus(mac, status);
}

void printTrackedNode(const TrackedNode& node, const char* eventLabel) {
  char macBuffer[18] = {};
  formatMac(node.mac, macBuffer, sizeof(macBuffer));

  Serial.printf("node=%s mac=%s kind=%s bpm=%u program=",
                eventLabel,
                macBuffer,
                nodeKindName(node.status.identity.nodeKind),
                static_cast<unsigned>(node.status.currentBpm));
  if (node.status.currentProgramIndex == 255) {
    Serial.print("remote");
  } else {
    Serial.print(static_cast<unsigned>(node.status.currentProgramIndex + 1U));
  }
  Serial.printf(" uptime=%lu\n", static_cast<unsigned long>(node.status.uptimeMs));
}

void processPendingNodeStatuses(uint32_t now) {
  PendingNodeStatusEvent localEvents[kPendingNodeStatusCapacity];
  size_t localEventCount = 0;

  portENTER_CRITICAL(&nodeStatusMux);
  for (size_t i = 0; i < kPendingNodeStatusCapacity; ++i) {
    if (!pendingNodeStatuses[i].ready) {
      continue;
    }

    localEvents[localEventCount++] = pendingNodeStatuses[i];
    pendingNodeStatuses[i].ready = false;
  }
  portEXIT_CRITICAL(&nodeStatusMux);

  for (size_t eventIndex = 0; eventIndex < localEventCount; ++eventIndex) {
    const auto& event = localEvents[eventIndex];
    TrackedNode* slot = nullptr;
    TrackedNode* reusableSlot = nullptr;

    for (auto& trackedNode : trackedNodes) {
      if (trackedNode.active &&
          memcmp(trackedNode.mac, event.mac, sizeof(trackedNode.mac)) == 0) {
        slot = &trackedNode;
        break;
      }

      if (!trackedNode.active && reusableSlot == nullptr) {
        reusableSlot = &trackedNode;
      }
    }

    if (slot == nullptr) {
      slot = reusableSlot;
    }

    if (slot == nullptr) {
      slot = &trackedNodes[0];
    }

    const bool wasActive = slot->active;
    const bool changed =
      !wasActive ||
      memcmp(slot->mac, event.mac, sizeof(slot->mac)) != 0 ||
      slot->status.identity.nodeKind != event.status.identity.nodeKind ||
      slot->status.currentBpm != event.status.currentBpm ||
      slot->status.beatsPerBar != event.status.beatsPerBar ||
      slot->status.currentProgramIndex != event.status.currentProgramIndex;

    slot->active = true;
    memcpy(slot->mac, event.mac, sizeof(slot->mac));
    slot->status = event.status;
    slot->lastSeenAtMs = now;

    if (!wasActive) {
      printTrackedNode(*slot, "seen");
    } else if (changed) {
      printTrackedNode(*slot, "update");
    }
  }
}

void expireTrackedNodes(uint32_t now) {
  for (auto& trackedNode : trackedNodes) {
    if (!trackedNode.active) {
      continue;
    }

    if ((now - trackedNode.lastSeenAtMs) < NODE_STALE_MS) {
      continue;
    }

    printTrackedNode(trackedNode, "lost");
    trackedNode.active = false;
  }
}

void resetAudioClockFollow() {
  audioClockLocked = false;
  audioSyncCandidateCount = 0;
  audioSyncCandidateBpm = 0;
  audioFollowCandidateBpm = 0;
  audioFollowCandidateCount = 0;
  audioSyncPhaseMissSign = 0;
  audioSyncPhaseMissCount = 0;
  lastAudioOnsetSeenAtMs = 0;
  lastAudioOnsetHandledAtMs = 0;
  lastAudioLockAtMs = 0;
  syncBeatDotPending = false;
}

void queueSyncBeatDot() {
  syncBeatDotPending = true;
}

void setClockBpm(uint16_t bpm, const char* source) {
  const uint16_t clampedBpm = clampBpm(bpm);
  if (clampedBpm == currentBpm) {
    beatIntervalMs = bpmToIntervalMs(currentBpm);
    return;
  }

  currentBpm = clampedBpm;
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  clockRevision++;
  Serial.printf("clock=bpm source=%s bpm=%u\n", source, static_cast<unsigned>(currentBpm));
}

void updateClockFromAudio(uint32_t now) {
  if (!brainLive) {
    resetAudioClockFollow();
    return;
  }

  const bool musicPresent = microphone.musicPresent();
  const uint16_t detectedBpm = microphone.clockBpm();
  const uint8_t beatConfidence = microphone.beatConfidence();
  const uint32_t onsetAtMs = microphone.lastOnsetAtMs();

  if (audioClockLocked && (!musicPresent || detectedBpm == 0) &&
      (now - lastAudioLockAtMs) > AUDIO_SYNC_LOST_MS) {
    resetAudioClockFollow();
    Serial.println("audio_sync=unlock reason=signal_lost");
    return;
  }

  if (!musicPresent || detectedBpm == 0 || onsetAtMs == 0 || onsetAtMs == lastAudioOnsetSeenAtMs) {
    return;
  }

  const uint32_t previousAudioOnsetSeenAtMs = lastAudioOnsetSeenAtMs;
  lastAudioOnsetSeenAtMs = onsetAtMs;

  const uint16_t targetBpm = clampBpm(detectedBpm);

  if (!audioClockLocked) {
    if (beatConfidence < AUDIO_SYNC_CANDIDATE_CONFIDENCE) {
      audioSyncCandidateCount = 0;
      audioSyncCandidateBpm = 0;
      return;
    }

    lastAudioOnsetHandledAtMs = onsetAtMs;

    if (audioSyncCandidateCount == 0 ||
        bpmDifference(audioSyncCandidateBpm, targetBpm) > AUDIO_SYNC_CANDIDATE_BPM_TOLERANCE) {
      audioSyncCandidateBpm = targetBpm;
      audioSyncCandidateCount = 1;
      return;
    }

    audioSyncCandidateBpm =
      static_cast<uint16_t>((audioSyncCandidateBpm + targetBpm + 1U) / 2U);
    if (audioSyncCandidateCount < AUDIO_SYNC_REQUIRED_ONSETS) {
      audioSyncCandidateCount++;
    }

    if (audioSyncCandidateCount < AUDIO_SYNC_REQUIRED_ONSETS) {
      return;
    }

    audioClockLocked = true;
    audioFollowCandidateBpm = 0;
    audioFollowCandidateCount = 0;
    audioSyncPhaseMissSign = 0;
    audioSyncPhaseMissCount = 0;
    lastAudioLockAtMs = now;
    setClockBpm(audioSyncCandidateBpm, "audio_lock");
    nextBeatAtMs = onsetAtMs;
    queueSyncBeatDot();
    Serial.printf("audio_sync=lock bpm=%u conf=%u at=%lu\n",
                  static_cast<unsigned>(currentBpm),
                  static_cast<unsigned>(beatConfidence),
                  static_cast<unsigned long>(onsetAtMs));
    return;
  }

  if (beatConfidence < AUDIO_SYNC_LOCKED_CONFIDENCE) {
    return;
  }

  const uint32_t observedIntervalMs =
    (previousAudioOnsetSeenAtMs == 0) ? 0 : (onsetAtMs - previousAudioOnsetSeenAtMs);
  if (observedIntervalMs != 0) {
    const uint32_t minIntervalMs =
      (beatIntervalMs * AUDIO_SYNC_INTERVAL_MIN_PERCENT) / 100UL;
    const uint32_t maxIntervalMs =
      (beatIntervalMs * AUDIO_SYNC_INTERVAL_MAX_PERCENT) / 100UL;
    const uint32_t doubleMinIntervalMs =
      (beatIntervalMs * AUDIO_SYNC_DOUBLE_INTERVAL_MIN_PERCENT) / 100UL;
    const uint32_t doubleMaxIntervalMs =
      (beatIntervalMs * AUDIO_SYNC_DOUBLE_INTERVAL_MAX_PERCENT) / 100UL;

    if (observedIntervalMs < minIntervalMs) {
      return;
    }

    if (observedIntervalMs > maxIntervalMs &&
        (observedIntervalMs < doubleMinIntervalMs || observedIntervalMs > doubleMaxIntervalMs)) {
      return;
    }
  }

  lastAudioOnsetHandledAtMs = onsetAtMs;
  lastAudioLockAtMs = now;

  if (targetBpm == currentBpm) {
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
      const int32_t bpmDelta = static_cast<int32_t>(targetBpm) - static_cast<int32_t>(currentBpm);
      uint16_t steppedBpm = currentBpm;
      if (bpmDelta > 0) {
        const uint16_t step = static_cast<uint16_t>(
          (bpmDelta > AUDIO_SYNC_MAX_BPM_STEP) ? AUDIO_SYNC_MAX_BPM_STEP : bpmDelta);
        steppedBpm = static_cast<uint16_t>(currentBpm + step);
      } else {
        const int32_t positiveDelta = -bpmDelta;
        const uint16_t step = static_cast<uint16_t>(
          (positiveDelta > AUDIO_SYNC_MAX_BPM_STEP) ? AUDIO_SYNC_MAX_BPM_STEP : positiveDelta);
        steppedBpm = static_cast<uint16_t>(currentBpm - step);
      }

      setClockBpm(steppedBpm, "audio_follow");
      audioFollowCandidateBpm = 0;
      audioFollowCandidateCount = 0;
    }
  }

  const int32_t nextScheduledBeatAtMs = static_cast<int32_t>(nextBeatAtMs);
  const int32_t previousScheduledBeatAtMs =
    nextScheduledBeatAtMs - static_cast<int32_t>(beatIntervalMs);
  const int32_t previousBeatErrorMs =
    static_cast<int32_t>(onsetAtMs) - previousScheduledBeatAtMs;
  const int32_t nextBeatErrorMs =
    static_cast<int32_t>(onsetAtMs) - nextScheduledBeatAtMs;
  const int32_t phaseErrorMs =
    (abs(previousBeatErrorMs) <= abs(nextBeatErrorMs)) ? previousBeatErrorMs : nextBeatErrorMs;
  const uint32_t absolutePhaseErrorMs = static_cast<uint32_t>(abs(phaseErrorMs));
  uint32_t hardResyncMs = beatIntervalMs / 5UL;
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
      nextBeatAtMs = static_cast<uint32_t>(
        static_cast<int32_t>(nextBeatAtMs) + phaseErrorMs);
      audioSyncPhaseMissSign = 0;
      audioSyncPhaseMissCount = 0;
      queueSyncBeatDot();
      Serial.printf("audio_sync=resync error=%ld at=%lu\n",
                    static_cast<long>(phaseErrorMs),
                    static_cast<unsigned long>(onsetAtMs));
      return;
    }

    nextBeatAtMs = static_cast<uint32_t>(
      static_cast<int32_t>(nextBeatAtMs) + (phaseErrorMs / AUDIO_SYNC_PRE_RESYNC_TRIM_DIVISOR));
    return;
  }

  audioSyncPhaseMissSign = 0;
  audioSyncPhaseMissCount = 0;
  nextBeatAtMs = static_cast<uint32_t>(
    static_cast<int32_t>(nextBeatAtMs) + (phaseErrorMs / AUDIO_SYNC_SOFT_TRIM_DIVISOR));
}

void resetTapTempoSequence() {
  pendingSingleTap = false;
  pendingSingleTapAtMs = 0;
  lastTapAtMs = 0;
  tapIntervalCount = 0;
  tapTempoUiUntilMs = 0;

  for (auto& interval : tapIntervalsMs) {
    interval = 0;
  }
}

void clearMatrix() {
  for (uint8_t i = 0; i < 25; ++i) {
    M5.dis.drawpix(i, 0x000000);
  }
}

void drawModeNumber(size_t modeIndex) {
  clearMatrix();

  const uint8_t* rows = kDigitMasks[modeIndex];
  for (uint8_t y = 0; y < 5; ++y) {
    for (uint8_t x = 0; x < 5; ++x) {
      const bool on = (rows[y] >> (4 - x)) & 0x01;
      M5.dis.drawpix(y * 5 + x, on ? color(255, 255, 255) : 0x000000);
    }
  }
}

uint32_t beatDotColor() {
  if (beatDotColorOverride != 0) {
    return beatDotColorOverride;
  }

  return (beatDotBeat == 1) ? color(255, 0, 0) : color(255, 255, 255);
}

void drawBeatDotOverlay() {
  M5.dis.drawpix(4, beatDotColor());
}

void updateBeatDotOverlay(uint32_t now) {
  if (!brainLive || beatDotUntilMs == 0 || (int32_t)(now - beatDotUntilMs) >= 0) {
    return;
  }

  drawBeatDotOverlay();
}

void drawTapTempoFlash() {
  for (uint8_t i = 0; i < 25; ++i) {
    M5.dis.drawpix(i, color(255, 90, 0));
  }

  matrixOffAtMs = millis() + TAP_FLASH_MS;
}

uint32_t meterColorForRow(uint8_t rowFromBottom) {
  switch (rowFromBottom) {
    case 0:
    case 1:
      return color(0, 110, 18);
    case 2:
      return color(130, 120, 0);
    case 3:
      return color(160, 60, 0);
    default:
      return color(180, 0, 0);
  }
}

void drawMicrophoneMeter(uint8_t filledPixels) {
  clearMatrix();

  for (uint8_t slot = 0; slot < 25; ++slot) {
    if (slot >= filledPixels) {
      break;
    }

    const uint8_t rowFromBottom = slot / 5;
    const uint8_t x = slot % 5;
    const uint8_t y = 4 - rowFromBottom;
    M5.dis.drawpix(y * 5 + x, meterColorForRow(rowFromBottom));
  }
}

void updateIdleMatrixUi(uint32_t now) {
  if (uiFeedbackUntilMs != 0 || tapTempoUiUntilMs != 0 || matrixOffAtMs != 0) {
    return;
  }

  if ((now - lastMeterDrawAtMs) < METER_REFRESH_MS) {
    return;
  }

  drawMicrophoneMeter(microphone.meterLevel());
  lastMeterDrawAtMs = now;
}

void printCompactAudioDebug(uint32_t now) {
  if ((now - lastDebugStatusAtMs) < DEBUG_STATUS_INTERVAL_MS) {
    return;
  }

  lastDebugStatusAtMs = now;
  Serial.printf("debug music=%u raw=%u audio=%u clock=%u conf=%u lock=%u meter=%u live=%u\n",
                static_cast<unsigned>(microphone.musicPresent()),
                static_cast<unsigned>(microphone.detectedBpm()),
                static_cast<unsigned>(microphone.clockBpm()),
                static_cast<unsigned>(currentBpm),
                static_cast<unsigned>(microphone.beatConfidence()),
                static_cast<unsigned>(audioClockLocked),
                static_cast<unsigned>(microphone.meterLevel()),
                static_cast<unsigned>(brainLive));
}

void onBeat() {
  const uint8_t currentBeat = beatInBar;

  beatDotBeat = currentBeat;
  beatDotColorOverride = syncBeatDotPending ? color(0, 90, 255) : 0;
  syncBeatDotPending = false;
  beatDotUntilMs = millis() + BEAT_DOT_FLASH_MS;

  if (espNowReady && currentBeat == 1) {
    const auto sync = makeClockSyncMessage(
      clockRevision,
      ++beatSerial,
      currentBpm,
      BEATS_PER_BAR,
      currentBeat,
      currentBar
    );

    const auto result = esp_now_send(
      decaflash::espnow_transport::kBroadcastMac,
      reinterpret_cast<const uint8_t*>(&sync),
      sizeof(sync)
    );

    if (result != ESP_OK) {
      Serial.printf("send=clock_sync result=%d beat=%u bar=%lu\n",
                    result,
                    currentBeat,
                    static_cast<unsigned long>(currentBar));
    }
  }

  beatInBar++;
  if (beatInBar > BEATS_PER_BAR) {
    beatInBar = 1;
    currentBar++;
  }
}

void sendCommand(decaflash::NodeKind targetNodeKind, const decaflash::NodeCommand& command) {
  if (!espNowReady || !brainLive) {
    return;
  }

  const auto message = makeNodeCommandMessage(
    targetNodeKind,
    command,
    commandRevision
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  Serial.printf("send=node_command result=%d target=%u mode=%u command=%s\n",
                result,
                static_cast<unsigned>(targetNodeKind),
                static_cast<unsigned>(currentModeIndex + 1),
                message.command.name);
}

void sendCurrentCommands() {
  sendCommand(decaflash::NodeKind::Flashlight, kFlashCommands[currentModeIndex]);
  sendCommand(decaflash::NodeKind::RgbStrip, kRgbCommands[currentModeIndex]);
}

void sendBrainHello() {
  if (!espNowReady || brainLive) {
    return;
  }

  const auto message = makeBrainHelloMessage(helloRevision);
  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  Serial.printf("send=brain_hello result=%d\n", result);
}

void showModeUi() {
  drawModeNumber(currentModeIndex);
  uiFeedbackUntilMs = millis() + UI_MODE_DISPLAY_MS;
  matrixOffAtMs = 0;
  Serial.printf("mode=%u\n", static_cast<unsigned>(currentModeIndex + 1));
}

void updateBpmFromTapTempo() {
  if (tapIntervalCount == 0) {
    return;
  }

  uint32_t totalIntervalMs = 0;
  for (uint8_t i = 0; i < tapIntervalCount; ++i) {
    totalIntervalMs += tapIntervalsMs[i];
  }

  const uint32_t averageIntervalMs = totalIntervalMs / tapIntervalCount;
  if (averageIntervalMs == 0) {
    return;
  }

  resetAudioClockFollow();
  setClockBpm(static_cast<uint16_t>(60000UL / averageIntervalMs), "tap");

  Serial.printf("tap_tempo=bpm:%u\n", static_cast<unsigned>(currentBpm));
}

void selectNextMode() {
  currentModeIndex = (currentModeIndex + 1) % kModeCount;
  commandRevision++;
  showModeUi();
  sendCurrentCommands();
}

void activateBrain() {
  brainLive = true;
  resetAudioClockFollow();
  beatSerial = 0;
  beatInBar = 1;
  currentBar = 1;
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  commandRevision++;
  showModeUi();
  sendCurrentCommands();
  nextSendAtMs = millis() + COMMAND_REFRESH_MS;
  Serial.println("brain=live");
}

void registerTap() {
  const uint32_t now = millis();
  tapTempoUiUntilMs = now + TAP_TEMPO_TIMEOUT_MS;

  if (lastTapAtMs != 0 && (now - lastTapAtMs) <= TAP_TEMPO_TIMEOUT_MS) {
    drawTapTempoFlash();
    const uint32_t intervalMs = now - lastTapAtMs;

    if (tapIntervalCount < 3) {
      tapIntervalsMs[tapIntervalCount++] = intervalMs;
    } else {
      tapIntervalsMs[0] = tapIntervalsMs[1];
      tapIntervalsMs[1] = tapIntervalsMs[2];
      tapIntervalsMs[2] = intervalMs;
    }

    pendingSingleTap = false;
    pendingSingleTapAtMs = 0;
    updateBpmFromTapTempo();
  } else {
    pendingSingleTap = true;
    pendingSingleTapAtMs = now;
    tapIntervalCount = 0;
  }

  lastTapAtMs = now;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  M5.begin(true, false, true);
  clearMatrix();

  Serial.println();
  Serial.println("Decaflash Brain V1");
  Serial.printf("device_type=%u\n", static_cast<unsigned>(DEVICE_TYPE));
  const auto message = makeNodeCommandMessage(decaflash::NodeKind::Flashlight, kFlashQuadSkip, 1);
  Serial.printf("protocol=dcfl/v%u\n", message.header.version);
  Serial.printf("example_command=%s\n", message.command.name);
  microphone.begin();

  const auto initResult = initEspNow();
  const auto peerResult = initResult.ok() ? ensureBroadcastPeer() : decltype(ensureBroadcastPeer()){};
  espNowReady = initResult.ok() && peerResult.ok();

  Serial.printf("wifi_set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("wifi_start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("wifi_set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("esp_now_init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("esp_now=%s\n", initResult.ok() ? "ok" : "failed");
  Serial.printf("peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("add_peer=%d\n", static_cast<int>(peerResult.addPeer));
  Serial.printf("mode=%s\n", espNowReady ? "silent start" : "startup only");
  Serial.println("button=single tap start/next mode | multi tap bpm");
  Serial.println("node_discovery=esp-now node status receive");

  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  clearMatrix();

  if (espNowReady) {
    esp_now_register_recv_cb(onEspNowReceive);
    sendBrainHello();
  }
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  microphone.update();
  processPendingNodeStatuses(now);
  expireTrackedNodes(now);

  if (M5.Btn.wasPressed()) {
    registerTap();
  }

  if (lastTapAtMs != 0 && (now - lastTapAtMs) > TAP_TEMPO_TIMEOUT_MS) {
    resetTapTempoSequence();
  }

  if (pendingSingleTap && (now - pendingSingleTapAtMs) > BUTTON_TAP_WINDOW_MS) {
    pendingSingleTap = false;
    pendingSingleTapAtMs = 0;
    if (brainLive) {
      selectNextMode();
    } else {
      activateBrain();
    }
  }

  if (uiFeedbackUntilMs != 0 && (int32_t)(now - uiFeedbackUntilMs) >= 0) {
    uiFeedbackUntilMs = 0;
    clearMatrix();
  }

  if (matrixOffAtMs != 0 && (int32_t)(now - matrixOffAtMs) >= 0) {
    clearMatrix();
    matrixOffAtMs = 0;
  }

  if (beatDotUntilMs != 0 && (int32_t)(now - beatDotUntilMs) >= 0) {
    beatDotUntilMs = 0;
    beatDotColorOverride = 0;
  }

  updateClockFromAudio(now);
  printCompactAudioDebug(now);

  while (brainLive && (int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }

  if (brainLive && (int32_t)(now - nextSendAtMs) >= 0) {
    sendCurrentCommands();
    nextSendAtMs += COMMAND_REFRESH_MS;
  }

  updateIdleMatrixUi(now);
  updateBeatDotOverlay(now);
}
