#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstring>

#include "decaflash_types.h"
#include "espnow_transport.h"
#include "node_output.h"
#include "node_programs.h"
#include "protocol.h"

using decaflash::DeviceType;
using decaflash::FlashCommand;
using decaflash::FlashLength;
using decaflash::FlashPattern;
using decaflash::NodeEffect;
using decaflash::NodeIdentity;
using decaflash::NodeKind;
using decaflash::RgbCommand;
using decaflash::RgbPattern;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::protocol::BrainHelloMessage;
using decaflash::protocol::ClockSyncMessage;
using decaflash::protocol::FlashCommandMessage;
using decaflash::protocol::RgbCommandMessage;
using decaflash::protocol::makeNodeStatusMessage;
using decaflash::node::flashSceneCommandFor;
using decaflash::node::kSceneCount;
using decaflash::node::rgbSceneCommandFor;
using decaflash::node::sceneName;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Node;
static constexpr NodeKind DEFAULT_NODE_KIND = NodeKind::RgbStrip;
static constexpr char kConfigNamespace[] = "decaflash";
static constexpr char kConfigNodeKindKey[] = "node_kind";
static constexpr char kConfigNodeEffectKey[] = "node_effect";
static constexpr size_t kSerialLineCapacity = 48;
static constexpr uint32_t NODE_STATUS_INTERVAL_MS = 5000;
static constexpr int BUTTON_PIN = 39;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr uint32_t CLOCK_SYNC_TIMEOUT_MS = 4000;
static constexpr uint32_t BRAIN_HELLO_DEDUP_MS = 1500;
static constexpr uint8_t BRAIN_CONNECT_FLASH_COUNT = 3;
static constexpr uint32_t BRAIN_CONNECT_FLASH_INTERVAL_MS = 1000;
static constexpr uint16_t BRAIN_CONNECT_FLASH_DURATION_MS = 260;
static constexpr uint32_t SOFT_SYNC_MATCH_WINDOW_MS = 120;
static constexpr uint32_t DUPLICATE_BEAT_GUARD_MS = 250;
static constexpr int32_t SOFT_SYNC_MAX_CORRECTION_MS = 30;
static constexpr int32_t HARD_SYNC_ERROR_MS = 180;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t DEFAULT_BEATS_PER_BAR = 4;

static constexpr FlashCommand REMOTE_IDLE_FLASH_COMMAND = {
  "Remote Idle",
  FlashPattern::Off,
  FlashLength::Short,
  1,
  1,
};

static constexpr RgbCommand REMOTE_IDLE_RGB_COMMAND = {
  "Remote Idle",
  RgbPattern::Off,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
  1,
  1,
  1200,
  0,
};

NodeIdentity nodeIdentity = {
  DEVICE_TYPE,
  DEFAULT_NODE_KIND,
  NodeEffect::Pulse,
  0,
};

NodeOutput renderer;
size_t currentProgramCount = 0;
size_t currentProgram = 0;
FlashCommand activeFlashCommand = REMOTE_IDLE_FLASH_COMMAND;
RgbCommand activeRgbCommand = REMOTE_IDLE_RGB_COMMAND;
Preferences preferences;
char serialLine[kSerialLineCapacity] = {};
size_t serialLineLength = 0;
bool espNowReady = false;
uint32_t nextStatusAtMs = 0;
bool outputMuted = false;

bool buttonPressed = false;
bool longPressHandled = false;
uint32_t buttonChangedAtMs = 0;
uint32_t buttonPressedAtMs = 0;
bool lastButtonLevel = HIGH;

uint32_t nextBeatAtMs = 0;
uint32_t beatIntervalMs = 0;
uint32_t currentBar = 1;
uint8_t beatInBar = 1;
uint8_t beatsPerBar = DEFAULT_BEATS_PER_BAR;
bool brainConnected = false;
bool remoteControlActive = false;
bool awaitingRemoteClock = false;
uint32_t lastRemoteCommandRevision = 0;
uint32_t lastClockRevision = 0;
uint32_t lastClockBeatSerial = 0;
uint32_t lastClockSyncAtMs = 0;
uint32_t lastBrainActivityAtMs = 0;
uint32_t lastBrainHelloHandledAtMs = 0;
bool clockLocked = false;
bool brainConnectSequenceShown = false;
uint32_t lastBeatRenderedAtMs = 0;
uint8_t lastRenderedBeatInBar = 0;
uint32_t lastRenderedBar = 0;

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool hasPendingFlashCommand = false;
volatile bool hasPendingRgbCommand = false;
volatile bool hasPendingClockSync = false;
volatile bool hasPendingBrainHello = false;
FlashCommandMessage pendingFlashCommandMessage = {};
RgbCommandMessage pendingRgbCommandMessage = {};
ClockSyncMessage pendingClockSyncMessage = {};
BrainHelloMessage pendingBrainHelloMessage = {};

void onBeat();

NodeEffect defaultEffectFor(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return NodeEffect::Pulse;

    case NodeKind::Flashlight:
    default:
      return NodeEffect::Pulse;
  }
}

bool effectCompatible(NodeKind nodeKind, NodeEffect nodeEffect) {
  if (nodeEffect == NodeEffect::None) {
    return false;
  }

  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return nodeEffect == NodeEffect::Wash ||
             nodeEffect == NodeEffect::Pulse ||
             nodeEffect == NodeEffect::Accent ||
             nodeEffect == NodeEffect::Flicker;

    case NodeKind::Flashlight:
      return nodeEffect == NodeEffect::Pulse ||
             nodeEffect == NodeEffect::Accent;

    default:
      return false;
  }
}

NodeEffect nextNodeEffect(NodeKind nodeKind, NodeEffect currentEffect) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      switch (currentEffect) {
        case NodeEffect::Wash:
          return NodeEffect::Pulse;

        case NodeEffect::Pulse:
          return NodeEffect::Accent;

        case NodeEffect::Accent:
          return NodeEffect::Flicker;

        case NodeEffect::Flicker:
        case NodeEffect::None:
        default:
          return NodeEffect::Wash;
      }

    case NodeKind::Flashlight:
    default:
      return (currentEffect == NodeEffect::Accent) ? NodeEffect::Pulse : NodeEffect::Accent;
  }
}

const char* nodeKindName(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return "rgb";

    case NodeKind::Flashlight:
      return "flash";

    case NodeKind::UvLed:
      return "uv";

    default:
      return "unknown";
  }
}

const char* nodeEffectName(NodeEffect nodeEffect) {
  switch (nodeEffect) {
    case NodeEffect::Wash:
      return "wash";

    case NodeEffect::Pulse:
      return "pulse";

    case NodeEffect::Accent:
      return "accent";

    case NodeEffect::Flicker:
      return "flicker";

    case NodeEffect::None:
    default:
      return "none";
  }
}

const char* runtimeLabelFor(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return "standalone rgb demo";

    case NodeKind::Flashlight:
    default:
      return "standalone flash demo";
  }
}

bool isSupportedNodeKind(uint8_t rawNodeKind) {
  return rawNodeKind == static_cast<uint8_t>(NodeKind::Flashlight) ||
         rawNodeKind == static_cast<uint8_t>(NodeKind::RgbStrip);
}

bool isSupportedNodeEffect(uint8_t rawNodeEffect) {
  switch (static_cast<NodeEffect>(rawNodeEffect)) {
    case NodeEffect::Wash:
    case NodeEffect::Pulse:
    case NodeEffect::Accent:
    case NodeEffect::Flicker:
      return true;

    case NodeEffect::None:
    default:
      return false;
  }
}

NodeKind loadStoredNodeKind() {
  if (!preferences.begin(kConfigNamespace, true)) {
    return DEFAULT_NODE_KIND;
  }

  const uint8_t storedNodeKind = preferences.getUChar(
    kConfigNodeKindKey,
    static_cast<uint8_t>(DEFAULT_NODE_KIND)
  );
  preferences.end();

  if (!isSupportedNodeKind(storedNodeKind)) {
    return DEFAULT_NODE_KIND;
  }

  return static_cast<NodeKind>(storedNodeKind);
}

NodeEffect loadStoredNodeEffect(NodeKind nodeKind) {
  if (!preferences.begin(kConfigNamespace, true)) {
    return defaultEffectFor(nodeKind);
  }

  const uint8_t storedNodeEffect = preferences.getUChar(
    kConfigNodeEffectKey,
    static_cast<uint8_t>(defaultEffectFor(nodeKind))
  );
  preferences.end();

  if (!isSupportedNodeEffect(storedNodeEffect)) {
    return defaultEffectFor(nodeKind);
  }

  const NodeEffect nodeEffect = static_cast<NodeEffect>(storedNodeEffect);
  return effectCompatible(nodeKind, nodeEffect) ? nodeEffect : defaultEffectFor(nodeKind);
}

bool saveNodeKind(NodeKind nodeKind) {
  if (!preferences.begin(kConfigNamespace, false)) {
    return false;
  }

  const size_t bytesWritten = preferences.putUChar(
    kConfigNodeKindKey,
    static_cast<uint8_t>(nodeKind)
  );
  preferences.end();
  return bytesWritten == sizeof(uint8_t);
}

bool saveNodeEffect(NodeEffect nodeEffect) {
  if (!preferences.begin(kConfigNamespace, false)) {
    return false;
  }

  const size_t bytesWritten = preferences.putUChar(
    kConfigNodeEffectKey,
    static_cast<uint8_t>(nodeEffect)
  );
  preferences.end();
  return bytesWritten == sizeof(uint8_t);
}

uint32_t bpmToIntervalMs(uint16_t bpm) {
  return 60000UL / bpm;
}

uint16_t currentBpmValue() {
  return beatIntervalMs == 0 ? BPM : static_cast<uint16_t>(60000UL / beatIntervalMs);
}

uint8_t currentProgramIndexForStatus() {
  if (remoteControlActive) {
    return 255;
  }

  return static_cast<uint8_t>(currentProgram > 254 ? 254 : currentProgram);
}

void sendNodeStatus(const char* reason) {
  if (!espNowReady) {
    return;
  }

  const auto message = makeNodeStatusMessage(
    nodeIdentity,
    currentBpmValue(),
    beatsPerBar,
    currentProgramIndexForStatus(),
    millis()
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result != ESP_OK || strcmp(reason, "heartbeat") != 0) {
    Serial.printf("send=node_status result=%d reason=%s kind=%s role=%s scene=",
                  result,
                  reason,
                  nodeKindName(nodeIdentity.nodeKind),
                  nodeEffectName(nodeIdentity.nodeEffect));
    if (message.currentProgramIndex == 255) {
      Serial.println("remote");
    } else {
      Serial.println(static_cast<unsigned>(message.currentProgramIndex + 1U));
    }
  }
}

void refreshProgramSet() {
  currentProgramCount = 0;

  if (nodeIdentity.nodeKind == NodeKind::Flashlight ||
      nodeIdentity.nodeKind == NodeKind::RgbStrip) {
    currentProgramCount = kSceneCount;
  }

  if (currentProgramCount == 0) {
    currentProgram = 0;
    activeFlashCommand = REMOTE_IDLE_FLASH_COMMAND;
    activeRgbCommand = REMOTE_IDLE_RGB_COMMAND;
    renderer.setFlashCommand(activeFlashCommand);
    renderer.setRgbCommand(activeRgbCommand);
    return;
  }

  if (currentProgram >= currentProgramCount) {
    currentProgram = 0;
  }
}

void advanceBeatPosition(uint8_t beat, uint32_t bar, uint8_t& nextBeat, uint32_t& nextBar) {
  nextBeat = beat + 1;
  nextBar = bar;

  if (nextBeat > beatsPerBar) {
    nextBeat = 1;
    nextBar++;
  }
}

bool wasSameBeatRenderedRecently(uint8_t beat, uint32_t bar, uint32_t now) {
  return lastBeatRenderedAtMs != 0 &&
         (now - lastBeatRenderedAtMs) <= DUPLICATE_BEAT_GUARD_MS &&
         lastRenderedBeatInBar == beat &&
         lastRenderedBar == bar;
}

void applyFlashCommand(const FlashCommand& command) {
  activeFlashCommand = command;
  renderer.setFlashCommand(activeFlashCommand);
  outputMuted = false;
  renderer.allOff();
}

void applyRgbCommand(const RgbCommand& command) {
  activeRgbCommand = command;
  renderer.setRgbCommand(activeRgbCommand);
  outputMuted = false;
}

void resetLocalClockState() {
  brainConnected = false;
  remoteControlActive = false;
  awaitingRemoteClock = false;
  lastRemoteCommandRevision = 0;
  lastClockRevision = 0;
  lastClockBeatSerial = 0;
  lastClockSyncAtMs = 0;
  clockLocked = false;
  lastBeatRenderedAtMs = 0;
  lastRenderedBeatInBar = 0;
  lastRenderedBar = 0;
  beatIntervalMs = bpmToIntervalMs(BPM);
  beatsPerBar = DEFAULT_BEATS_PER_BAR;
  beatInBar = 1;
  currentBar = 1;
  nextBeatAtMs = millis() + beatIntervalMs;
  renderer.syncBeatClock(millis(), beatIntervalMs, beatsPerBar, beatInBar, currentBar);
}

void selectProgram(size_t programIndex, bool announce = true) {
  if (currentProgramCount == 0) {
    return;
  }

  currentProgram = programIndex % currentProgramCount;
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    applyFlashCommand(flashSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
  } else {
    applyRgbCommand(rgbSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
  }
  resetLocalClockState();

  if (announce) {
    Serial.println();
    Serial.println("-----");
    Serial.printf("SCENE %u (%s): %s | %s | %s | %u BPM\n",
                  static_cast<unsigned>(currentProgram + 1),
                  sceneName(currentProgram),
                  nodeIdentity.nodeKind == NodeKind::Flashlight
                    ? activeFlashCommand.name
                    : activeRgbCommand.name,
                  nodeKindName(nodeIdentity.nodeKind),
                  nodeEffectName(nodeIdentity.nodeEffect),
                  static_cast<unsigned>(currentBpmValue()));
    Serial.println("-----");
  }

  sendNodeStatus("scene");
}

void selectNextProgram() {
  if (currentProgramCount == 0) {
    return;
  }

  selectProgram((currentProgram + 1) % currentProgramCount);
}

void printPrograms() {
  Serial.printf("scenes=%s role=%s\n",
                nodeKindName(nodeIdentity.nodeKind),
                nodeEffectName(nodeIdentity.nodeEffect));

  for (size_t i = 0; i < currentProgramCount; ++i) {
    const char* name = nodeIdentity.nodeKind == NodeKind::Flashlight
      ? flashSceneCommandFor(nodeIdentity.nodeEffect, i).name
      : rgbSceneCommandFor(nodeIdentity.nodeEffect, i).name;
    Serial.printf("  %u -> %s | %s\n",
                  static_cast<unsigned>(i + 1),
                  sceneName(i),
                  name);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Button:");
  Serial.println("  short press -> next scene (local only)");
  Serial.println("  long press  -> next role");
  Serial.println();
  Serial.println("Serial:");
  Serial.println("  mode rgb");
  Serial.println("  mode flash");
  Serial.println("  role wash | pulse | accent | flicker");
  Serial.println("  effect ...   (alias for role ...)");
  Serial.println("  status");
  Serial.println("  help");
  Serial.println();
}

void printStatus() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("node_role=%s\n", nodeEffectName(nodeIdentity.nodeEffect));
  Serial.printf("renderer=%s\n", renderer.rendererName());
  Serial.printf("scene=%u/%u name=%s command=%s\n",
                static_cast<unsigned>(currentProgram + 1),
                static_cast<unsigned>(currentProgramCount),
                sceneName(currentProgram),
                nodeIdentity.nodeKind == NodeKind::Flashlight
                  ? activeFlashCommand.name
                  : activeRgbCommand.name);
  Serial.printf("control=%s clock=%s bpm=%u muted=%u\n",
                remoteControlActive ? "remote" : "local",
                clockLocked ? "locked" : "local",
                static_cast<unsigned>(currentBpmValue()),
                static_cast<unsigned>(outputMuted));
  Serial.println("-----");
}

void announceNodeProfile() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("node_role=%s\n", nodeEffectName(nodeIdentity.nodeEffect));
  Serial.printf("renderer=%s\n", renderer.rendererName());
  Serial.printf("runtime=%s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("-----");
}

void configureNodeProfile(NodeKind nodeKind, NodeEffect nodeEffect) {
  const NodeEffect effectiveEffect =
    effectCompatible(nodeKind, nodeEffect) ? nodeEffect : defaultEffectFor(nodeKind);
  nodeIdentity.nodeKind = nodeKind;
  nodeIdentity.nodeEffect = effectiveEffect;
  renderer.setNodeProfile(nodeKind, effectiveEffect);
  refreshProgramSet();
  selectProgram(0, false);
}

void switchNodeKind(NodeKind nodeKind, bool persist) {
  const NodeEffect effectiveEffect =
    effectCompatible(nodeKind, nodeIdentity.nodeEffect)
      ? nodeIdentity.nodeEffect
      : defaultEffectFor(nodeKind);

  if (persist) {
    if (!saveNodeKind(nodeKind) || !saveNodeEffect(effectiveEffect)) {
      Serial.println("config=save_failed");
    }
  }

  configureNodeProfile(nodeKind, effectiveEffect);
  announceNodeProfile();
  printPrograms();
  selectProgram(0);
  sendNodeStatus("node_mode");
}

void switchNodeEffect(NodeEffect nodeEffect, bool persist) {
  if (!effectCompatible(nodeIdentity.nodeKind, nodeEffect)) {
    Serial.printf("serial=role_mismatch role=%s mode=%s\n",
                  nodeEffectName(nodeEffect),
                  nodeKindName(nodeIdentity.nodeKind));
    return;
  }

  if (persist && !saveNodeEffect(nodeEffect)) {
    Serial.println("config=save_failed");
  }

  configureNodeProfile(nodeIdentity.nodeKind, nodeEffect);
  renderer.showRoleConfirm(nodeIdentity.nodeEffect);
  announceNodeProfile();
  printPrograms();
  selectProgram(0);
  sendNodeStatus("node_role");
}

void cycleNodeEffect(bool persist) {
  switchNodeEffect(nextNodeEffect(nodeIdentity.nodeKind, nodeIdentity.nodeEffect), persist);
}

void runBrainConnectSequence() {
  lastBrainActivityAtMs = millis();
  brainConnected = true;
  brainConnectSequenceShown = true;
  renderer.allOff();

  for (uint8_t i = 0; i < BRAIN_CONNECT_FLASH_COUNT; ++i) {
    renderer.flash100(BRAIN_CONNECT_FLASH_DURATION_MS);

    if (i + 1 < BRAIN_CONNECT_FLASH_COUNT) {
      delay(BRAIN_CONNECT_FLASH_INTERVAL_MS - BRAIN_CONNECT_FLASH_DURATION_MS);
    }
  }
}

void stageIncomingFlashCommand(const FlashCommandMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingFlashCommandMessage = message;
  hasPendingFlashCommand = true;
  portEXIT_CRITICAL(&radioMux);
}

void stageIncomingRgbCommand(const RgbCommandMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingRgbCommandMessage = message;
  hasPendingRgbCommand = true;
  portEXIT_CRITICAL(&radioMux);
}

void stageIncomingClockSync(const ClockSyncMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingClockSyncMessage = message;
  hasPendingClockSync = true;
  portEXIT_CRITICAL(&radioMux);
}

void stageIncomingBrainHello(const BrainHelloMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingBrainHelloMessage = message;
  hasPendingBrainHello = true;
  portEXIT_CRITICAL(&radioMux);
}

void onEspNowReceive(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;

  if (len < static_cast<int>(sizeof(decaflash::protocol::MessageHeader))) {
    return;
  }

  decaflash::protocol::MessageHeader header = {};
  memcpy(&header, data, sizeof(header));

  if (header.type == decaflash::protocol::MessageType::FlashCommand &&
      len == static_cast<int>(sizeof(FlashCommandMessage))) {
    FlashCommandMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::FlashCommand)) {
      return;
    }
    stageIncomingFlashCommand(message);
    return;
  }

  if (header.type == decaflash::protocol::MessageType::RgbCommand &&
      len == static_cast<int>(sizeof(RgbCommandMessage))) {
    RgbCommandMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::RgbCommand)) {
      return;
    }
    stageIncomingRgbCommand(message);
    return;
  }

  if (header.type == decaflash::protocol::MessageType::ClockSync &&
      len == static_cast<int>(sizeof(ClockSyncMessage))) {
    ClockSyncMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::ClockSync)) {
      return;
    }
    stageIncomingClockSync(message);
    return;
  }

  if (header.type == decaflash::protocol::MessageType::BrainHello &&
      len == static_cast<int>(sizeof(BrainHelloMessage))) {
    BrainHelloMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::BrainHello)) {
      return;
    }
    stageIncomingBrainHello(message);
  }
}

void processPendingBrainHelloMessage(const BrainHelloMessage& message) {
  (void)message;
  const uint32_t now = millis();
  if (lastBrainHelloHandledAtMs != 0 &&
      (now - lastBrainHelloHandledAtMs) < BRAIN_HELLO_DEDUP_MS) {
    return;
  }

  lastBrainHelloHandledAtMs = now;
  lastBrainActivityAtMs = now;
  brainConnectSequenceShown = false;
  runBrainConnectSequence();

  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    applyFlashCommand(REMOTE_IDLE_FLASH_COMMAND);
  } else {
    applyRgbCommand(REMOTE_IDLE_RGB_COMMAND);
  }
  remoteControlActive = true;
  awaitingRemoteClock = true;
  clockLocked = false;
  Serial.println("remote=waiting_for_brain");
  sendNodeStatus("brain_hello");
}

void processPendingFlashCommandMessage(const FlashCommandMessage& message) {
  lastBrainActivityAtMs = millis();

  if (nodeIdentity.nodeKind != NodeKind::Flashlight ||
      message.targetNodeKind != nodeIdentity.nodeKind ||
      message.targetNodeEffect != nodeIdentity.nodeEffect) {
    return;
  }

  if (!brainConnected) {
    brainConnected = true;
  }

  if (remoteControlActive && message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  applyFlashCommand(message.command);
  remoteControlActive = true;
  awaitingRemoteClock = true;
  lastRemoteCommandRevision = message.commandRevision;

  Serial.printf("remote=flash command=%s role=%s bpm=%u\n",
                activeFlashCommand.name,
                nodeEffectName(nodeIdentity.nodeEffect),
                static_cast<unsigned>(currentBpmValue()));
  sendNodeStatus("remote_flash");
}

void processPendingRgbCommandMessage(const RgbCommandMessage& message) {
  lastBrainActivityAtMs = millis();

  if (nodeIdentity.nodeKind != NodeKind::RgbStrip ||
      message.targetNodeKind != nodeIdentity.nodeKind ||
      message.targetNodeEffect != nodeIdentity.nodeEffect) {
    return;
  }

  if (!brainConnected) {
    brainConnected = true;
  }

  if (remoteControlActive && message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  applyRgbCommand(message.command);
  remoteControlActive = true;
  awaitingRemoteClock = true;
  lastRemoteCommandRevision = message.commandRevision;

  Serial.printf("remote=rgb command=%s role=%s bpm=%u\n",
                activeRgbCommand.name,
                nodeEffectName(nodeIdentity.nodeEffect),
                static_cast<unsigned>(currentBpmValue()));
  sendNodeStatus("remote_rgb");
}

void applyClockSync(const ClockSyncMessage& message) {
  lastBrainActivityAtMs = millis();

  if (message.clockRevision == lastClockRevision &&
      message.beatSerial == lastClockBeatSerial) {
    return;
  }

  if (!brainConnected) {
    brainConnected = true;
  }

  const uint32_t now = millis();
  lastClockRevision = message.clockRevision;
  lastClockBeatSerial = message.beatSerial;
  lastClockSyncAtMs = now;
  beatIntervalMs = bpmToIntervalMs(message.bpm);
  beatsPerBar = message.beatsPerBar;
  awaitingRemoteClock = false;

  if (!remoteControlActive) {
    advanceBeatPosition(message.beatInBar, message.currentBar, beatInBar, currentBar);
    nextBeatAtMs = now + beatIntervalMs;
    clockLocked = true;
    return;
  }

  if (wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now)) {
    advanceBeatPosition(message.beatInBar, message.currentBar, beatInBar, currentBar);
    nextBeatAtMs = lastBeatRenderedAtMs + beatIntervalMs;
    clockLocked = true;
    return;
  }

  const bool recentMatchingBeat =
    wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now) &&
    (now - lastBeatRenderedAtMs) <= SOFT_SYNC_MATCH_WINDOW_MS;

  if (recentMatchingBeat) {
    const uint32_t desiredNextBeatAtMs = now + beatIntervalMs;
    const int32_t phaseErrorMs =
      static_cast<int32_t>(desiredNextBeatAtMs) - static_cast<int32_t>(nextBeatAtMs);

    if (abs(phaseErrorMs) <= HARD_SYNC_ERROR_MS) {
      int32_t correctionMs = phaseErrorMs / 2;
      if (correctionMs > SOFT_SYNC_MAX_CORRECTION_MS) {
        correctionMs = SOFT_SYNC_MAX_CORRECTION_MS;
      } else if (correctionMs < -SOFT_SYNC_MAX_CORRECTION_MS) {
        correctionMs = -SOFT_SYNC_MAX_CORRECTION_MS;
      }

      nextBeatAtMs = static_cast<uint32_t>(static_cast<int32_t>(nextBeatAtMs) + correctionMs);
      advanceBeatPosition(message.beatInBar, message.currentBar, beatInBar, currentBar);
      clockLocked = true;
      return;
    }
  }

  beatInBar = message.beatInBar;
  currentBar = message.currentBar;
  clockLocked = true;
  onBeat();
  nextBeatAtMs = now + beatIntervalMs;
}

void processPendingRadio() {
  bool hadFlashCommand = false;
  bool hadRgbCommand = false;
  bool hadClockSync = false;
  bool hadBrainHello = false;
  FlashCommandMessage flashCommandMessage = {};
  RgbCommandMessage rgbCommandMessage = {};
  ClockSyncMessage clockMessage = {};
  BrainHelloMessage brainHelloMessage = {};

  portENTER_CRITICAL(&radioMux);
  if (hasPendingFlashCommand) {
    flashCommandMessage = pendingFlashCommandMessage;
    hasPendingFlashCommand = false;
    hadFlashCommand = true;
  }
  if (hasPendingRgbCommand) {
    rgbCommandMessage = pendingRgbCommandMessage;
    hasPendingRgbCommand = false;
    hadRgbCommand = true;
  }
  if (hasPendingClockSync) {
    clockMessage = pendingClockSyncMessage;
    hasPendingClockSync = false;
    hadClockSync = true;
  }
  if (hasPendingBrainHello) {
    brainHelloMessage = pendingBrainHelloMessage;
    hasPendingBrainHello = false;
    hadBrainHello = true;
  }
  portEXIT_CRITICAL(&radioMux);

  if (hadBrainHello) {
    processPendingBrainHelloMessage(brainHelloMessage);
  }

  if (hadFlashCommand) {
    processPendingFlashCommandMessage(flashCommandMessage);
  }

  if (hadRgbCommand) {
    processPendingRgbCommandMessage(rgbCommandMessage);
  }

  if (hadClockSync) {
    applyClockSync(clockMessage);
  }
}

void serviceNodeStatus() {
  if (!espNowReady) {
    return;
  }

  const uint32_t now = millis();
  if ((int32_t)(now - nextStatusAtMs) < 0) {
    return;
  }

  sendNodeStatus("heartbeat");
  nextStatusAtMs = now + NODE_STATUS_INTERVAL_MS;
}

void serviceButton() {
  const uint32_t now = millis();
  const bool level = digitalRead(BUTTON_PIN);

  if (level != lastButtonLevel) {
    lastButtonLevel = level;
    buttonChangedAtMs = now;
  }

  if ((now - buttonChangedAtMs) < BUTTON_DEBOUNCE_MS) {
    return;
  }

  const bool isPressed = (level == LOW);

  if (isPressed && !buttonPressed) {
    buttonPressed = true;
    longPressHandled = false;
    buttonPressedAtMs = now;
    return;
  }

  if (isPressed && buttonPressed && !longPressHandled) {
    if ((now - buttonPressedAtMs) >= BUTTON_LONG_PRESS_MS) {
      longPressHandled = true;
      Serial.println("button=long_press");
      cycleNodeEffect(true);
    }
    return;
  }

  if (!isPressed && buttonPressed) {
    buttonPressed = false;

    if (!longPressHandled && !brainConnected && !remoteControlActive) {
      Serial.println("button=short_press");
      selectNextProgram();
    }
  }
}

uint32_t flashDurationMsFor(FlashLength length) {
  return (length == FlashLength::Long) ? 120U : 65U;
}

bool isTriggerBeat(uint8_t triggerEveryBars, uint8_t triggerBeat) {
  const bool isTriggerBar =
    (triggerEveryBars <= 1) || ((currentBar % triggerEveryBars) == 0);
  return ((triggerBeat == 0) || (beatInBar == triggerBeat)) && isTriggerBar;
}

void onBeat() {
  const uint32_t now = millis();
  lastBeatRenderedAtMs = now;
  lastRenderedBeatInBar = beatInBar;
  lastRenderedBar = currentBar;
  renderer.syncBeatClock(now, beatIntervalMs, beatsPerBar, beatInBar, currentBar);

  if (!outputMuted) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      const bool trigger = isTriggerBeat(
        activeFlashCommand.triggerEveryBars,
        activeFlashCommand.triggerBeat
      );

      switch (activeFlashCommand.pattern) {
        case FlashPattern::PerBeat:
          if (trigger) {
            renderer.flash100(flashDurationMsFor(activeFlashCommand.length));
          }
          break;

        case FlashPattern::Off:
        default:
          renderer.allOff();
          break;
      }
    } else {
      bool trigger = false;
      switch (activeRgbCommand.pattern) {
        case RgbPattern::BeatPulse:
        case RgbPattern::Accent:
        case RgbPattern::RunnerFlicker:
          trigger = isTriggerBeat(activeRgbCommand.triggerEveryBars, activeRgbCommand.triggerBeat);
          break;

        case RgbPattern::BarWave:
        case RgbPattern::Off:
        default:
          trigger = false;
          break;
      }

      if (trigger) {
        renderer.triggerRgbAccent();
      }
    }
  }

  beatInBar++;

  if (beatInBar > beatsPerBar) {
    beatInBar = 1;
    currentBar++;
  }
}

void serviceClock() {
  const uint32_t now = millis();

  if (brainConnectSequenceShown &&
      lastBrainActivityAtMs != 0 &&
      (now - lastBrainActivityAtMs) >= CLOCK_SYNC_TIMEOUT_MS) {
    brainConnected = false;
    awaitingRemoteClock = false;
    brainConnectSequenceShown = false;
  }

  if (clockLocked && lastClockSyncAtMs != 0 &&
      (now - lastClockSyncAtMs) >= CLOCK_SYNC_TIMEOUT_MS) {
    clockLocked = false;
    if (brainConnected) {
      brainConnected = false;
      awaitingRemoteClock = false;
    }
  }

  if (brainConnected && !remoteControlActive) {
    return;
  }

  if (remoteControlActive && awaitingRemoteClock) {
    return;
  }

  while ((int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }
}

void serviceOutput() {
  if (outputMuted) {
    return;
  }

  renderer.service(millis());
}

void normalizeSerialLine(char* line) {
  size_t length = strlen(line);
  while (length > 0 && isspace(static_cast<unsigned char>(line[length - 1]))) {
    line[--length] = '\0';
  }

  size_t start = 0;
  while (line[start] != '\0' && isspace(static_cast<unsigned char>(line[start]))) {
    start++;
  }

  if (start > 0) {
    memmove(line, line + start, strlen(line + start) + 1);
  }

  for (size_t i = 0; line[i] != '\0'; ++i) {
    line[i] = static_cast<char>(tolower(static_cast<unsigned char>(line[i])));
  }
}

bool parseNodeEffect(const char* line, NodeEffect& parsedEffect) {
  if (strcmp(line, "role wash") == 0 ||
      strcmp(line, "effect wash") == 0 ||
      strcmp(line, "effect ambient") == 0 ||
      strcmp(line, "effect bluewash") == 0) {
    parsedEffect = NodeEffect::Wash;
    return true;
  }

  if (strcmp(line, "role pulse") == 0 ||
      strcmp(line, "effect pulse") == 0 ||
      strcmp(line, "effect bluepulse") == 0 ||
      strcmp(line, "effect flashpulse") == 0) {
    parsedEffect = NodeEffect::Pulse;
    return true;
  }

  if (strcmp(line, "role accent") == 0 ||
      strcmp(line, "effect accent") == 0 ||
      strcmp(line, "effect redaccent") == 0 ||
      strcmp(line, "effect flashaccent") == 0) {
    parsedEffect = NodeEffect::Accent;
    return true;
  }

  if (strcmp(line, "role flicker") == 0 ||
      strcmp(line, "effect flicker") == 0 ||
      strcmp(line, "effect motion") == 0 ||
      strcmp(line, "effect blueredflicker") == 0) {
    parsedEffect = NodeEffect::Flicker;
    return true;
  }

  return false;
}

void processSerialCommand(const char* line) {
  if (strcmp(line, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(line, "status") == 0) {
    printStatus();
    return;
  }

  if (strcmp(line, "mode rgb") == 0 || strcmp(line, "mode rgbstrip") == 0) {
    switchNodeKind(NodeKind::RgbStrip, true);
    return;
  }

  if (strcmp(line, "mode flash") == 0 || strcmp(line, "mode flashlight") == 0) {
    switchNodeKind(NodeKind::Flashlight, true);
    return;
  }

  NodeEffect parsedEffect = NodeEffect::None;
  if (parseNodeEffect(line, parsedEffect)) {
    switchNodeEffect(parsedEffect, true);
    return;
  }

  Serial.printf("serial=unknown_command '%s'\n", line);
  Serial.println("serial=use 'help'");
}

void serviceSerial() {
  while (Serial.available() > 0) {
    const char incoming = static_cast<char>(Serial.read());

    if (incoming == '\r') {
      continue;
    }

    if (incoming == '\n') {
      serialLine[serialLineLength] = '\0';
      normalizeSerialLine(serialLine);
      if (serialLine[0] != '\0') {
        processSerialCommand(serialLine);
      }
      serialLineLength = 0;
      serialLine[0] = '\0';
      continue;
    }

    if (serialLineLength + 1 < kSerialLineCapacity) {
      serialLine[serialLineLength++] = incoming;
      serialLine[serialLineLength] = '\0';
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(STARTUP_DELAY_MS);

  pinMode(BUTTON_PIN, INPUT);

  const NodeKind storedKind = loadStoredNodeKind();
  const NodeEffect storedEffect = loadStoredNodeEffect(storedKind);
  configureNodeProfile(storedKind, storedEffect);

  Serial.println();
  Serial.println("Decaflash Node V2");
  Serial.printf(
    "device_type=%u node_kind=%u node_role=%u\n",
    static_cast<unsigned>(nodeIdentity.deviceType),
    static_cast<unsigned>(nodeIdentity.nodeKind),
    static_cast<unsigned>(nodeIdentity.nodeEffect)
  );
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("node_role=%s\n", nodeEffectName(nodeIdentity.nodeEffect));
  Serial.printf("runtime=%s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("controls=atom button + serial");
  Serial.printf("renderer=%s\n", renderer.rendererName());
  if (nodeIdentity.nodeKind == NodeKind::RgbStrip) {
    Serial.println("rgb_driver=sk6812 pins=26+32");
  }
  Serial.printf("clock=%u bpm %u/4\n", BPM, beatsPerBar);

  const auto initResult = initEspNow();
  decaflash::espnow_transport::PeerResult peerResult = {};
  if (initResult.ok()) {
    peerResult = ensureBroadcastPeer();
  }
  espNowReady = initResult.ok() && peerResult.ok();

  Serial.printf("wifi_set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("wifi_start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("wifi_set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("esp_now_init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("esp_now=%s\n", espNowReady ? "ok" : "failed");
  Serial.printf("peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("add_peer=%d\n", static_cast<int>(peerResult.addPeer));
  if (espNowReady) {
    esp_now_register_recv_cb(onEspNowReceive);
  }
  Serial.println("runtime=local scene demo + remote scene receive");
  Serial.println("node_stack=kind+role aware");
  printPrograms();
  printHelp();
  selectProgram(0);
  if (espNowReady) {
    sendNodeStatus("boot");
    nextStatusAtMs = millis() + NODE_STATUS_INTERVAL_MS;
  }
}

void loop() {
  serviceSerial();
  processPendingRadio();
  serviceButton();
  serviceClock();
  serviceOutput();
  serviceNodeStatus();
}
