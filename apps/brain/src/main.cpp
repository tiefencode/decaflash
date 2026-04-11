#include <Arduino.h>
#include <M5Atom.h>

#include "scene_programs.h"
#include "decaflash_types.h"
#include "espnow_transport.h"
#include "matrix_meter.h"
#include "matrix_ui.h"
#include "audio_follow.h"
#include "pdm_microphone.h"
#include "protocol.h"
#include "brain_shell.h"
#include "ai_mode.h"
#include "api_client.h"
#include "sync_debug.h"
#include "text_playback.h"
#include "wifi_manager.h"

using decaflash::DeviceType;
using decaflash::NodeEffect;
using decaflash::scenes::flashSceneCommandFor;
using decaflash::scenes::kFlashReference;
using decaflash::scenes::kPulseReference;
using decaflash::scenes::kSceneCount;
using decaflash::scenes::rgbSceneCommandFor;
using decaflash::scenes::sceneName;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::protocol::makeBrainHelloMessage;
using decaflash::protocol::makeClockSyncMessage;
using decaflash::protocol::makeFlashCommandMessage;
using decaflash::protocol::makeRgbCommandMessage;
using decaflash::protocol::NodeStatusMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t COMMAND_REFRESH_MS = 60000;
// Node discovery is event-driven; heartbeats are only a fallback signal.
// Keep stale time comfortably above the 30s node heartbeat so a missed
// heartbeat does not turn into a false rediscovery.
static constexpr uint32_t NODE_STALE_MS = 75000;
static constexpr uint32_t NODE_RESTART_UPTIME_GRACE_MS = 2000;
static constexpr uint16_t DEFAULT_BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;
static constexpr uint16_t BEAT_DOT_FLASH_MS = 140;
static constexpr uint32_t METER_REFRESH_MS = 40;
static constexpr uint32_t UI_SCENE_DISPLAY_MS = 3000;
static constexpr uint32_t ESPNOW_RECOVERY_INTERVAL_MS = 1000;
static constexpr uint16_t MIN_BPM = 60;
static constexpr uint16_t MAX_BPM = 180;
static constexpr size_t kSceneSlots = kSceneCount;
static constexpr size_t kTrackedNodeCapacity = 8;
static constexpr size_t kPendingNodeStatusCapacity = 6;
static constexpr NodeEffect kFlashEffects[] = {
  NodeEffect::Pulse,
};
static constexpr NodeEffect kRgbEffects[] = {
  NodeEffect::Wash,
  NodeEffect::Pulse,
  NodeEffect::Accent,
  NodeEffect::Flicker,
};
uint32_t nextSendAtMs = 0;
bool espNowReady = false;
bool brainLive = false;
uint32_t commandRevision = 1;
uint32_t clockRevision = 1;
uint32_t beatSerial = 0;
uint16_t currentBpm = DEFAULT_BPM;
uint32_t beatIntervalMs = 0;
uint32_t nextBeatAtMs = 0;
uint8_t beatInBar = 1;
uint32_t currentBar = 1;
uint32_t matrixOffAtMs = 0;
uint32_t uiFeedbackUntilMs = 0;
uint32_t beatDotUntilMs = 0;
uint8_t beatDotBeat = 0;
uint32_t beatDotColorOverride = 0;
bool syncBeatDotPending = false;
size_t currentSceneIndex = 0;
uint32_t lastMeterDrawAtMs = 0;
bool pendingCommandRefresh = false;
bool pendingClockSync = false;
decaflash::brain::PdmMicrophone microphone;
portMUX_TYPE nodeStatusMux = portMUX_INITIALIZER_UNLOCKED;
bool buttonPressedLastLoop = false;
bool buttonLongPressHandled = false;
uint32_t buttonPressedAtMs = 0;
bool lastRadioPauseActive = false;
bool lastWifiConnectedForEspNow = false;
uint8_t lastWifiChannelForEspNow = 0;
bool espNowBlockedByChannel = false;
bool espNowRecoveryRequested = false;
const char* espNowRecoveryReason = "startup";
uint32_t lastEspNowRecoveryAtMs = 0;

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

uint32_t bpmToIntervalMs(uint16_t bpm) {
  return 60000UL / bpm;
}

uint16_t clampBpm(uint16_t bpm) {
  if (bpm < MIN_BPM) {
    return MIN_BPM;
  }

  if (bpm > MAX_BPM) {
    return MAX_BPM;
  }

  return bpm;
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

const char* nodeRoleName(decaflash::NodeEffect nodeEffect) {
  switch (nodeEffect) {
    case decaflash::NodeEffect::Wash:
      return "wash";

    case decaflash::NodeEffect::Pulse:
      return "pulse";

    case decaflash::NodeEffect::Accent:
      return "accent";

    case decaflash::NodeEffect::Flicker:
      return "flicker";

    case decaflash::NodeEffect::None:
    default:
      return "none";
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

esp_err_t registerEspNowReceiveCallback() {
  return esp_now_register_recv_cb(onEspNowReceive);
}

void printTrackedNode(const TrackedNode& node, const char* eventLabel) {
  char macBuffer[18] = {};
  formatMac(node.mac, macBuffer, sizeof(macBuffer));

  Serial.printf("NODE: %s mac=%s kind=%s role=%s\n",
                eventLabel,
                macBuffer,
                nodeKindName(node.status.identity.nodeKind),
                nodeRoleName(node.status.identity.nodeEffect));
}

size_t activeNodeCount();

const char* nodeSyncModeName(uint8_t flags) {
  if ((flags & decaflash::protocol::kNodeClockSyncFlagDuplicateBeat) != 0U) {
    return "duplicate";
  }

  if ((flags & decaflash::protocol::kNodeClockSyncFlagPredictedBeat) != 0U) {
    return "predicted";
  }

  if ((flags & decaflash::protocol::kNodeClockSyncFlagResync) != 0U) {
    return "resync";
  }

  if ((flags & decaflash::protocol::kNodeClockSyncFlagMeasured) != 0U) {
    return "measured";
  }

  return "waiting";
}

bool hasClockSyncTelemetry(const NodeStatusMessage& status) {
  return status.clockSync.beatSerial != 0;
}

bool clockSyncChanged(const NodeStatusMessage& previousStatus, const NodeStatusMessage& nextStatus) {
  return previousStatus.clockSync.clockRevision != nextStatus.clockSync.clockRevision ||
         previousStatus.clockSync.beatSerial != nextStatus.clockSync.beatSerial ||
         previousStatus.clockSync.currentBar != nextStatus.clockSync.currentBar ||
         previousStatus.clockSync.beatInBar != nextStatus.clockSync.beatInBar ||
         previousStatus.clockSync.phaseErrorMs != nextStatus.clockSync.phaseErrorMs ||
         previousStatus.clockSync.flags != nextStatus.clockSync.flags;
}

void printTrackedNodeSync(const TrackedNode& node, const char* eventLabel) {
  char macBuffer[18] = {};
  formatMac(node.mac, macBuffer, sizeof(macBuffer));

  if (!hasClockSyncTelemetry(node.status)) {
    Serial.printf("SYNC: %s mac=%s kind=%s role=%s state=waiting\n",
                  eventLabel,
                  macBuffer,
                  nodeKindName(node.status.identity.nodeKind),
                  nodeRoleName(node.status.identity.nodeEffect));
    return;
  }

  Serial.printf("SYNC: %s mac=%s kind=%s role=%s beat=%lu bar=%lu beat_in_bar=%u phase_ms=%d mode=%s\n",
                eventLabel,
                macBuffer,
                nodeKindName(node.status.identity.nodeKind),
                nodeRoleName(node.status.identity.nodeEffect),
                static_cast<unsigned long>(node.status.clockSync.beatSerial),
                static_cast<unsigned long>(node.status.clockSync.currentBar),
                static_cast<unsigned>(node.status.clockSync.beatInBar),
                static_cast<int>(node.status.clockSync.phaseErrorMs),
                nodeSyncModeName(node.status.clockSync.flags));
}

void printSyncSummary() {
  const size_t nodeCount = activeNodeCount();
  Serial.printf("SYNC: summary active_nodes=%u log=%s\n",
                static_cast<unsigned>(nodeCount),
                decaflash::brain::sync_debug::autoLogEnabled() ? "on" : "off");

  if (nodeCount == 0) {
    Serial.println("SYNC: no_active_nodes");
    return;
  }

  for (const auto& trackedNode : trackedNodes) {
    if (!trackedNode.active) {
      continue;
    }

    printTrackedNodeSync(trackedNode, "node");
  }
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
    const NodeStatusMessage previousStatus = slot->status;
    const bool identityChanged =
      !wasActive ||
      memcmp(slot->mac, event.mac, sizeof(slot->mac)) != 0 ||
      slot->status.identity.nodeKind != event.status.identity.nodeKind ||
      slot->status.identity.nodeEffect != event.status.identity.nodeEffect ||
      slot->status.identity.profileRevision != event.status.identity.profileRevision;
    const bool restarted =
      wasActive &&
      !identityChanged &&
      event.status.uptimeMs + NODE_RESTART_UPTIME_GRACE_MS < slot->status.uptimeMs;
    const bool syncChanged =
      !wasActive || clockSyncChanged(previousStatus, event.status);

    slot->active = true;
    memcpy(slot->mac, event.mac, sizeof(slot->mac));
    slot->status = event.status;
    slot->lastSeenAtMs = now;

    if (!wasActive) {
      printTrackedNode(*slot, "seen");
      pendingCommandRefresh = true;
    } else if (identityChanged) {
      printTrackedNode(*slot, "role");
      pendingCommandRefresh = true;
    } else if (restarted) {
      printTrackedNode(*slot, "restart");
      pendingCommandRefresh = true;
    } else if (decaflash::brain::sync_debug::autoLogEnabled() &&
               hasClockSyncTelemetry(event.status) &&
               syncChanged) {
      printTrackedNodeSync(*slot, "update");
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

    trackedNode.active = false;
  }
}

size_t activeNodeCount() {
  size_t count = 0;
  for (const auto& trackedNode : trackedNodes) {
    if (trackedNode.active) {
      count++;
    }
  }

  return count;
}

void requestEspNowRecovery(const char* reason) {
  espNowReady = false;
  espNowRecoveryRequested = true;
  espNowRecoveryReason = reason;
}

void recoverEspNowIfNeeded(uint32_t now) {
  if (espNowBlockedByChannel || !espNowRecoveryRequested) {
    return;
  }

  if ((now - lastEspNowRecoveryAtMs) < ESPNOW_RECOVERY_INTERVAL_MS) {
    return;
  }

  lastEspNowRecoveryAtMs = now;
  const auto recovery = decaflash::espnow_transport::recoverEspNow();
  espNowReady = recovery.ok();

  if (!espNowReady) {
    Serial.printf("ESP-NOW: recover_failed reason=%s wifi_init=%d wifi_mode=%d wifi_start=%d wifi_ch=%d deinit=%d init=%d peer=%d\n",
                  espNowRecoveryReason,
                  static_cast<int>(recovery.wifiInit),
                  static_cast<int>(recovery.wifiSetMode),
                  static_cast<int>(recovery.wifiStart),
                  static_cast<int>(recovery.wifiSetChannel),
                  static_cast<int>(recovery.espNowDeinit),
                  static_cast<int>(recovery.espNowInit),
                  static_cast<int>(recovery.peer.addPeer));
    return;
  }

  const esp_err_t receiveCallbackResult = registerEspNowReceiveCallback();
  if (receiveCallbackResult != ESP_OK) {
    espNowReady = false;
    Serial.printf("ESP-NOW: recover_failed reason=%s wifi_init=%d wifi_mode=%d wifi_start=%d wifi_ch=%d deinit=%d init=%d peer=%d recv_cb=%d\n",
                  espNowRecoveryReason,
                  static_cast<int>(recovery.wifiInit),
                  static_cast<int>(recovery.wifiSetMode),
                  static_cast<int>(recovery.wifiStart),
                  static_cast<int>(recovery.wifiSetChannel),
                  static_cast<int>(recovery.espNowDeinit),
                  static_cast<int>(recovery.espNowInit),
                  static_cast<int>(recovery.peer.addPeer),
                  static_cast<int>(receiveCallbackResult));
    return;
  }

  espNowRecoveryRequested = false;
  pendingCommandRefresh = brainLive;
  pendingClockSync = brainLive;
  const uint8_t activeChannel = decaflash::brain::wifi_manager::isConnected()
                                  ? decaflash::brain::wifi_manager::currentChannel()
                                  : decaflash::espnow_transport::kWifiChannel;
  Serial.printf("ESP-NOW: recovered reason=%s channel=%u\n",
                espNowRecoveryReason,
                static_cast<unsigned>(activeChannel));
}

void serviceEspNowState(uint32_t now) {
  const bool radioPauseActive = decaflash::brain::api_client::radioPauseActive();
  const bool wifiConnected = decaflash::brain::wifi_manager::isConnected();
  const uint8_t wifiChannel = decaflash::brain::wifi_manager::currentChannel();
  const bool radioStateChanged =
    wifiConnected != lastWifiConnectedForEspNow ||
    wifiChannel != lastWifiChannelForEspNow;

  if (radioStateChanged) {
    lastWifiConnectedForEspNow = wifiConnected;
    lastWifiChannelForEspNow = wifiChannel;

    if (wifiConnected &&
        wifiChannel != 0 &&
        wifiChannel != decaflash::espnow_transport::kWifiChannel) {
      if (!espNowBlockedByChannel) {
        Serial.printf("ESP-NOW: blocked reason=wifi_channel_mismatch wifi_ch=%u espnow_ch=%u\n",
                      static_cast<unsigned>(wifiChannel),
                      static_cast<unsigned>(decaflash::espnow_transport::kWifiChannel));
      }
      espNowBlockedByChannel = true;
      espNowReady = false;
      espNowRecoveryRequested = false;
      return;
    }

    if (radioPauseActive && espNowBlockedByChannel) {
      return;
    }

    if (espNowBlockedByChannel) {
      Serial.printf("ESP-NOW: channel_ok channel=%u\n",
                    static_cast<unsigned>(
                      wifiConnected ? wifiChannel : decaflash::espnow_transport::kWifiChannel));
    }

    espNowBlockedByChannel = false;
    requestEspNowRecovery(wifiConnected ? "wifi_state_changed" : "wifi_disconnected");
  }

  if (!radioPauseActive &&
      !espNowBlockedByChannel &&
      !espNowReady &&
      !espNowRecoveryRequested) {
    requestEspNowRecovery("not_ready");
  }

  recoverEspNowIfNeeded(now);
}

void serviceManagedRadioPauseTransition() {
  const bool radioPauseActive = decaflash::brain::api_client::radioPauseActive();
  if (lastRadioPauseActive && !radioPauseActive) {
    if (espNowBlockedByChannel && !decaflash::brain::wifi_manager::isConnected()) {
      Serial.printf("ESP-NOW: channel_ok channel=%u\n",
                    static_cast<unsigned>(decaflash::espnow_transport::kWifiChannel));
      espNowBlockedByChannel = false;
    }
    requestEspNowRecovery("wifi_session_ended");
  }

  lastRadioPauseActive = radioPauseActive;
}

void resetAudioClockFollow() {
  decaflash::brain::audio_follow::reset();
  syncBeatDotPending = false;
}

void queueSyncBeatDot() {
  syncBeatDotPending = true;
}

void requestClockSync() {
  pendingClockSync = brainLive;
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
  (void)source;
  requestClockSync();
}

void updateClockFromAudio(uint32_t now) {
  decaflash::brain::audio_follow::Input input = {};
  input.brainLive = brainLive;
  input.musicPresent = microphone.musicPresent();
  input.detectedBpm = microphone.clockBpm();
  input.beatConfidence = microphone.beatConfidence();
  input.onsetAtMs = microphone.lastOnsetAtMs();
  input.currentBpm = currentBpm;
  input.minBpm = MIN_BPM;
  input.maxBpm = MAX_BPM;
  input.beatIntervalMs = beatIntervalMs;
  input.nextBeatAtMs = nextBeatAtMs;
  input.now = now;

  const decaflash::brain::audio_follow::Output output =
    decaflash::brain::audio_follow::update(input);

  if (output.setBpm) {
    setClockBpm(output.bpm, "audio_sync");
  }

  if (output.setNextBeatAtMs) {
    nextBeatAtMs = output.nextBeatAtMs;
  }

  if (output.queueSyncBeatDot) {
    queueSyncBeatDot();
  }

  if (output.requestClockSync) {
    requestClockSync();
  }
}

bool isSceneUiActive(uint32_t now) {
  return uiFeedbackUntilMs != 0 && (int32_t)(now - uiFeedbackUntilMs) < 0;
}

void selectNextScene();
void activateBrain();

void handleButtonInput(uint32_t now) {
  const bool buttonPressed = M5.Btn.isPressed();
  const bool radioPauseActive = decaflash::brain::api_client::radioPauseActive();

  if (buttonPressed && !buttonPressedLastLoop) {
    buttonPressedAtMs = now;
    buttonLongPressHandled = false;
  }

  if (buttonPressed && !buttonLongPressHandled &&
      (now - buttonPressedAtMs) >= decaflash::brain::ai_mode::togglePressMs()) {
    if (!radioPauseActive || decaflash::brain::ai_mode::enabled()) {
      decaflash::brain::ai_mode::toggle(now, microphone);
      buttonLongPressHandled = true;
    }
  }

  if (radioPauseActive) {
    if (!buttonPressed && buttonPressedLastLoop) {
      buttonLongPressHandled = false;
    }
    buttonPressedLastLoop = buttonPressed;
    return;
  }

  if (!buttonPressed && buttonPressedLastLoop) {
    if (!buttonLongPressHandled) {
      if (brainLive) {
        selectNextScene();
      } else {
        activateBrain();
      }
    }

    buttonLongPressHandled = false;
  }

  buttonPressedLastLoop = buttonPressed;
}

void updateBeatDotOverlay(uint32_t now) {
  if (decaflash::brain::text_playback::isActive() ||
      decaflash::brain::ai_mode::blocksBeatDotOverlay(now, microphone)) {
    return;
  }

  if (brainLive && beatDotUntilMs != 0 && (int32_t)(now - beatDotUntilMs) < 0) {
    decaflash::brain::matrix::drawBeatDotOverlay(beatDotBeat, beatDotColorOverride);
    return;
  }

  (void)now;
  decaflash::brain::matrix::clearBeatDotPixel();
}

bool currentMatrixUiOwnsStatusPixel(uint32_t now) {
  return decaflash::brain::text_playback::isActive() ||
         isSceneUiActive(now) ||
         decaflash::brain::ai_mode::blocksBeatDotOverlay(now, microphone);
}

void updateStatusPixelOverlay(uint32_t now) {
  if (currentMatrixUiOwnsStatusPixel(now)) {
    return;
  }

  uint32_t colorValue = 0;
  if (decaflash::brain::wifi_manager::statusPixelColor(now, colorValue)) {
    decaflash::brain::matrix::drawStatusPixelOverlay(colorValue);
    return;
  }

  decaflash::brain::matrix::clearStatusPixel();
}

void updateIdleMatrixUi(uint32_t now) {
  if (decaflash::brain::text_playback::serviceMatrix(now)) {
    return;
  }

  if (isSceneUiActive(now) || matrixOffAtMs != 0) {
    return;
  }

  if (decaflash::brain::ai_mode::renderOverlay(now, microphone)) {
    return;
  }

  if ((now - lastMeterDrawAtMs) < METER_REFRESH_MS) {
    return;
  }

  const auto meterTheme = decaflash::brain::ai_mode::useAiMeterTheme(microphone)
    ? decaflash::brain::matrix::MeterTheme::AiActive
    : decaflash::brain::matrix::MeterTheme::Default;
  decaflash::brain::matrix::drawMicrophoneMeter(microphone.meterLevel(), meterTheme);
  lastMeterDrawAtMs = now;
}

void onBeat() {
  const uint8_t currentBeat = beatInBar;
  const bool periodicBarSync = currentBeat == 1;

  beatDotBeat = currentBeat;
  beatDotColorOverride = syncBeatDotPending ? 0xFF0000 : 0;
  syncBeatDotPending = false;
  beatDotUntilMs = millis() + BEAT_DOT_FLASH_MS;

  if (!decaflash::brain::api_client::radioPauseActive() &&
      espNowReady &&
      (pendingClockSync || periodicBarSync)) {
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
      if (result == ESP_ERR_ESPNOW_NOT_INIT) {
        requestEspNowRecovery("clock_sync_not_init");
      }
      Serial.printf("SEND: clock_sync result=%d beat=%u bar=%lu\n",
                    result,
                    currentBeat,
                    static_cast<unsigned long>(currentBar));
    } else {
      pendingClockSync = false;
    }
  }

  beatInBar++;
  if (beatInBar > BEATS_PER_BAR) {
    beatInBar = 1;
    currentBar++;
  }
}

void sendFlashCommand(NodeEffect targetNodeEffect, const decaflash::FlashCommand& command) {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady || !brainLive) {
    return;
  }

  const auto message = makeFlashCommandMessage(
    decaflash::NodeKind::Flashlight,
    targetNodeEffect,
    command,
    commandRevision
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result != ESP_OK) {
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      requestEspNowRecovery("flash_command_not_init");
    }
    Serial.printf("SEND: flash_command result=%d role=%s scene=%u command=%s\n",
                  result,
                  nodeRoleName(targetNodeEffect),
                  static_cast<unsigned>(currentSceneIndex + 1),
                  message.command.name);
  }
}

void sendRgbCommand(NodeEffect targetNodeEffect, const decaflash::RgbCommand& command) {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady || !brainLive) {
    return;
  }

  const auto message = makeRgbCommandMessage(
    decaflash::NodeKind::RgbStrip,
    targetNodeEffect,
    command,
    commandRevision
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result != ESP_OK) {
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      requestEspNowRecovery("rgb_command_not_init");
    }
    Serial.printf("SEND: rgb_command result=%d role=%s scene=%u command=%s\n",
                  result,
                  nodeRoleName(targetNodeEffect),
                  static_cast<unsigned>(currentSceneIndex + 1),
                  message.command.name);
  }
}

void sendCurrentCommands() {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady || !brainLive) {
    return;
  }

  for (const auto effect : kFlashEffects) {
    sendFlashCommand(effect, flashSceneCommandFor(effect, currentSceneIndex));
  }

  for (const auto effect : kRgbEffects) {
    sendRgbCommand(effect, rgbSceneCommandFor(effect, currentSceneIndex));
  }
}

void sendBrainHello() {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady || brainLive) {
    return;
  }

  const auto message = makeBrainHelloMessage();
  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result != ESP_OK) {
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      requestEspNowRecovery("brain_hello_not_init");
    }
    Serial.printf("SEND: brain_hello result=%d\n", result);
  }
}

void showSceneUi() {
  decaflash::brain::matrix::drawSceneNumber(currentSceneIndex);
  uiFeedbackUntilMs = millis() + UI_SCENE_DISPLAY_MS;
  matrixOffAtMs = 0;
  Serial.printf("SCENE: index=%u name=%s bpm=%u\n",
                static_cast<unsigned>(currentSceneIndex + 1),
                sceneName(currentSceneIndex),
                static_cast<unsigned>(currentBpm));
}

void selectNextScene() {
  currentSceneIndex = (currentSceneIndex + 1) % kSceneSlots;
  commandRevision++;
  showSceneUi();
  sendCurrentCommands();
  requestClockSync();
}

void activateBrain() {
  brainLive = true;
  pendingCommandRefresh = false;
  resetAudioClockFollow();
  beatSerial = 0;
  beatInBar = 1;
  currentBar = 1;
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  commandRevision++;
  showSceneUi();
  sendCurrentCommands();
  requestClockSync();
  nextSendAtMs = millis() + COMMAND_REFRESH_MS;
  Serial.printf("BRAIN: live scene=%u bpm=%u\n",
                static_cast<unsigned>(currentSceneIndex + 1U),
                static_cast<unsigned>(currentBpm));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  M5.begin(true, false, true);
  decaflash::brain::matrix::clearMatrix();

  Serial.println();
  Serial.println("Decaflash Brain V1");
  Serial.printf("DEVICE: type=%u\n", static_cast<unsigned>(DEVICE_TYPE));
  const auto message = makeFlashCommandMessage(
    decaflash::NodeKind::Flashlight,
    NodeEffect::Pulse,
    kFlashReference,
    1
  );
  Serial.printf("PROTOCOL: dcfl/v%u\n", message.header.version);
  Serial.printf("EXAMPLE: command=%s\n", message.command.name);
  Serial.printf("EXAMPLE: rgb=%s\n", kPulseReference.name);
  microphone.begin();
  decaflash::brain::api_client::begin();

  const auto initResult = initEspNow();
  const auto peerResult = initResult.ok() ? ensureBroadcastPeer() : decltype(ensureBroadcastPeer()){};
  espNowReady = initResult.ok() && peerResult.ok();
  esp_err_t receiveCallbackResult = ESP_ERR_ESPNOW_NOT_INIT;
  if (espNowReady) {
    receiveCallbackResult = registerEspNowReceiveCallback();
    if (receiveCallbackResult != ESP_OK) {
      espNowReady = false;
    }
  }

  Serial.printf("WIFI: set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("WIFI: start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("WIFI: set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("ESP-NOW: init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("ESP-NOW: state=%s\n", espNowReady ? "ok" : "failed");
  Serial.printf("ESP-NOW: peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("ESP-NOW: add_peer=%d\n", static_cast<int>(peerResult.addPeer));
  Serial.printf("ESP-NOW: recv_cb=%d\n", static_cast<int>(receiveCallbackResult));
  Serial.printf("STARTUP: mode=%s\n", espNowReady ? "silent start" : "startup only");
  Serial.println("BUTTON: press start/next scene");
  decaflash::brain::shell::printHelp();

  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  decaflash::brain::matrix::clearMatrix();

  if (espNowReady) {
    sendBrainHello();
  }
}

void loop() {
  M5.update();
  const uint32_t now = millis();
  decaflash::brain::shell::serviceSerialInput();
  microphone.update();
  decaflash::brain::api_client::service(now);
  serviceManagedRadioPauseTransition();
  handleButtonInput(now);
  if (microphone.recordingReady()) {
    decaflash::brain::RecordedAudioClip recording = {};
    const bool tookRecording = microphone.takeRecording(recording);
    const bool queued = tookRecording &&
      decaflash::brain::api_client::queueRecordedAudioToTextDisplay(
        recording,
        decaflash::brain::ai_mode::ownsRecording());
    if (!queued) {
      Serial.println("RECORD: process_queue_failed");
      decaflash::brain::ai_mode::handleRecordingProcessed(now, false);
    }
  }
  bool audioProcessingSucceeded = false;
  bool audioProcessingWifiFailed = false;
  if (decaflash::brain::api_client::takeRecordedAudioCompletion(
        audioProcessingSucceeded,
        audioProcessingWifiFailed)) {
    if (audioProcessingWifiFailed && decaflash::brain::ai_mode::ownsRecording()) {
      decaflash::brain::ai_mode::handleWifiFailure(now);
    } else {
      decaflash::brain::ai_mode::handleRecordingProcessed(now, audioProcessingSucceeded);
    }
  }
  decaflash::brain::ai_mode::service(now, microphone);
  serviceEspNowState(now);
  processPendingNodeStatuses(now);
  expireTrackedNodes(now);
  if (decaflash::brain::sync_debug::consumeStatusPrintRequested()) {
    printSyncSummary();
  }

  if (brainLive && pendingCommandRefresh) {
    sendCurrentCommands();
    pendingCommandRefresh = false;
    requestClockSync();
    nextSendAtMs = now + COMMAND_REFRESH_MS;
  }

  if (uiFeedbackUntilMs != 0 && (int32_t)(now - uiFeedbackUntilMs) >= 0) {
    uiFeedbackUntilMs = 0;
    decaflash::brain::matrix::clearMatrix();
  }

  if (matrixOffAtMs != 0 && (int32_t)(now - matrixOffAtMs) >= 0) {
    decaflash::brain::matrix::clearMatrix();
    matrixOffAtMs = 0;
  }

  if (beatDotUntilMs != 0 && (int32_t)(now - beatDotUntilMs) >= 0) {
    beatDotUntilMs = 0;
    beatDotColorOverride = 0;
  }

  updateClockFromAudio(now);
  while (brainLive && (int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }

  if (brainLive && (int32_t)(now - nextSendAtMs) >= 0) {
    sendCurrentCommands();
    requestClockSync();
    nextSendAtMs += COMMAND_REFRESH_MS;
  }

  updateIdleMatrixUi(now);
  updateBeatDotOverlay(now);
  updateStatusPixelOverlay(now);
}
