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
using decaflash::FlashCommandMode;
using decaflash::FlashPattern;
using decaflash::FlashRenderCommand;
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
using decaflash::protocol::NodeClockSyncTelemetry;
using decaflash::protocol::NodeTextMessage;
using decaflash::protocol::RgbCommandMessage;
using decaflash::protocol::makeNodeStatusMessage;
using decaflash::node::flashRenderCommandFor;
using decaflash::node::flashSceneCommandFor;
using decaflash::node::flashVariationEpochFor;
using decaflash::node::kSceneCount;
using decaflash::node::rgbSceneCommandFor;
using decaflash::node::sceneName;

using NodeRole = decaflash::NodeEffect;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Node;
static constexpr NodeKind DEFAULT_NODE_KIND = NodeKind::Flashlight;
static constexpr char kConfigNamespace[] = "decaflash";
static constexpr char kConfigNodeKindKey[] = "node_kind";
static constexpr char kConfigNodeEffectKey[] = "node_effect";
static constexpr size_t kSerialLineCapacity = 48;
static constexpr uint32_t NODE_STATUS_INTERVAL_MS = 30000;
static constexpr uint32_t FILTER_MONITOR_INTERVAL_MS = 5000;
static constexpr int BUTTON_PIN = 39;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr uint8_t BRAIN_CONNECT_FLASH_COUNT = 3;
static constexpr uint32_t BRAIN_CONNECT_FLASH_INTERVAL_MS = 1000;
static constexpr uint16_t BRAIN_CONNECT_FLASH_DURATION_MS = 260;
static constexpr uint32_t DUPLICATE_BEAT_GUARD_MS = 250;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t DEFAULT_BEATS_PER_BAR = 4;
static constexpr size_t kNodeTextSegmentCapacity = decaflash::protocol::kNodeTextLength * 10U;

static constexpr FlashCommand REMOTE_IDLE_FLASH_COMMAND = {
  "Remote Idle",
  FlashCommandMode::Off,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
};

static constexpr FlashRenderCommand REMOTE_IDLE_FLASH_RENDER_COMMAND = {
  "Off",
  FlashPattern::Off,
  1,
  1,
  0,
  0,
  0,
  0,
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
  NodeRole::Pulse,
  1,
};

NodeOutput renderer;
size_t currentProgramCount = 0;
size_t currentProgram = 0;
FlashCommand activeFlashCommand = REMOTE_IDLE_FLASH_COMMAND;
FlashRenderCommand activeFlashRenderCommand = REMOTE_IDLE_FLASH_RENDER_COMMAND;
uint32_t activeFlashVariationEpoch = UINT32_MAX;
RgbCommand activeRgbCommand = REMOTE_IDLE_RGB_COMMAND;
Preferences preferences;
char serialLine[kSerialLineCapacity] = {};
size_t serialLineLength = 0;
bool espNowReady = false;
uint32_t nextStatusAtMs = 0;
uint32_t nextFilterMonitorAtMs = 0;
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

struct FlashBurstState {
  bool active = false;
  uint8_t remaining = 0;
  uint32_t nextFlashAtMs = 0;
  uint32_t intervalMs = 0;
  int16_t intervalStepMs = 0;
  uint16_t flashDurationMs = 0;
};

FlashBurstState flashBurst;

struct NodeTextSegment {
  uint8_t units = 0;
  bool on = false;
};

struct NodeTextOverlayState {
  bool pending = false;
  bool active = false;
  bool outputLit = false;
  uint32_t revision = 0;
  uint32_t startedAtMs = 0;
  uint32_t beatIntervalMs = 0;
  uint32_t elapsedUnits = 0;
  uint32_t nextSegmentAtMs = 0;
  size_t segmentCount = 0;
  size_t segmentIndex = 0;
  NodeTextSegment segments[kNodeTextSegmentCapacity] = {};
};

NodeTextOverlayState nodeTextOverlay;

enum class RunMode : uint8_t {
  Demo = 0,
  BrainWaiting = 1,
  BrainRunning = 2,
};

RunMode runMode = RunMode::Demo;
uint32_t lastRemoteCommandRevision = 0;
uint32_t lastClockRevision = 0;
uint32_t lastClockBeatSerial = 0;
NodeClockSyncTelemetry lastClockSyncTelemetry = {};
uint32_t lastBeatRenderedAtMs = 0;
uint8_t lastRenderedBeatInBar = 0;
uint32_t lastRenderedBar = 0;

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool hasPendingFlashCommand = false;
volatile bool hasPendingRgbCommand = false;
volatile bool hasPendingClockSync = false;
volatile bool hasPendingBrainHello = false;
volatile bool hasPendingNodeText = false;
FlashCommandMessage pendingFlashCommandMessage = {};
RgbCommandMessage pendingRgbCommandMessage = {};
ClockSyncMessage pendingClockSyncMessage = {};
BrainHelloMessage pendingBrainHelloMessage = {};
NodeTextMessage pendingNodeTextMessage = {};
uint32_t lastNodeTextRevision = 0;

void onBeat();

struct ClockSyncDiagnostics {
  int16_t phaseErrorMs = 0;
  uint8_t flags = 0;
};

NodeRole defaultRoleFor(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return NodeRole::Pulse;

    case NodeKind::Flashlight:
    default:
      return NodeRole::Pulse;
  }
}

bool roleCompatible(NodeKind nodeKind, NodeRole nodeRole) {
  if (nodeRole == NodeRole::None) {
    return false;
  }

  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return nodeRole == NodeRole::Wash ||
             nodeRole == NodeRole::Pulse ||
             nodeRole == NodeRole::Accent ||
             nodeRole == NodeRole::Flicker;

    case NodeKind::Flashlight:
      return nodeRole == NodeRole::Pulse;

    default:
      return false;
  }
}

NodeRole nextNodeRole(NodeKind nodeKind, NodeRole currentRole) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      switch (currentRole) {
        case NodeRole::Wash:
          return NodeRole::Pulse;

        case NodeRole::Pulse:
          return NodeRole::Accent;

        case NodeRole::Accent:
          return NodeRole::Flicker;

        case NodeRole::Flicker:
        case NodeRole::None:
        default:
          return NodeRole::Wash;
      }

    case NodeKind::Flashlight:
    default:
      (void)currentRole;
      return NodeRole::Pulse;
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

const char* nodeRoleName(NodeRole nodeRole) {
  switch (nodeRole) {
    case NodeRole::Wash:
      return "wash";

    case NodeRole::Pulse:
      return "pulse";

    case NodeRole::Accent:
      return "accent";

    case NodeRole::Flicker:
      return "flicker";

    case NodeRole::None:
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

bool isSupportedNodeRole(uint8_t rawNodeRole) {
  switch (static_cast<NodeRole>(rawNodeRole)) {
    case NodeRole::Wash:
    case NodeRole::Pulse:
    case NodeRole::Accent:
    case NodeRole::Flicker:
      return true;

    case NodeRole::None:
    default:
      return false;
  }
}

const char* colorDriftSideName(uint8_t colorDrift) {
  if (colorDrift < 96U) {
    return "blue";
  }

  if (colorDrift > 160U) {
    return "magenta";
  }

  return "neutral";
}

uint8_t colorDriftStrength(uint8_t colorDrift) {
  return (colorDrift > 127U)
    ? static_cast<uint8_t>((colorDrift - 127U) * 2U)
    : static_cast<uint8_t>((127U - colorDrift) * 2U);
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

NodeRole loadStoredNodeRole(NodeKind nodeKind) {
  if (!preferences.begin(kConfigNamespace, true)) {
    return defaultRoleFor(nodeKind);
  }

  const uint8_t storedNodeRole = preferences.getUChar(
    kConfigNodeEffectKey,
    static_cast<uint8_t>(defaultRoleFor(nodeKind))
  );
  preferences.end();

  if (!isSupportedNodeRole(storedNodeRole)) {
    return defaultRoleFor(nodeKind);
  }

  const NodeRole nodeRole = static_cast<NodeRole>(storedNodeRole);
  return roleCompatible(nodeKind, nodeRole) ? nodeRole : defaultRoleFor(nodeKind);
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

bool saveNodeRole(NodeRole nodeRole) {
  if (!preferences.begin(kConfigNamespace, false)) {
    return false;
  }

  const size_t bytesWritten = preferences.putUChar(
    kConfigNodeEffectKey,
    static_cast<uint8_t>(nodeRole)
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

const char* runModeName(RunMode mode) {
  switch (mode) {
    case RunMode::BrainWaiting:
      return "brain_wait";

    case RunMode::BrainRunning:
      return "brain";

    case RunMode::Demo:
    default:
      return "demo";
  }
}

bool isBrainOwned(RunMode mode) {
  return mode != RunMode::Demo;
}

struct MorseEntry {
  char character;
  const char* pattern;
};

const MorseEntry kMorseTable[] = {
  {'A', ".-"},
  {'B', "-..."},
  {'C', "-.-."},
  {'D', "-.."},
  {'E', "."},
  {'F', "..-."},
  {'G', "--."},
  {'H', "...."},
  {'I', ".."},
  {'J', ".---"},
  {'K', "-.-"},
  {'L', ".-.."},
  {'M', "--"},
  {'N', "-."},
  {'O', "---"},
  {'P', ".--."},
  {'Q', "--.-"},
  {'R', ".-."},
  {'S', "..."},
  {'T', "-"},
  {'U', "..-"},
  {'V', "...-"},
  {'W', ".--"},
  {'X', "-..-"},
  {'Y', "-.--"},
  {'Z', "--.."},
  {'0', "-----"},
  {'1', ".----"},
  {'2', "..---"},
  {'3', "...--"},
  {'4', "....-"},
  {'5', "....."},
  {'6', "-...."},
  {'7', "--..."},
  {'8', "---.."},
  {'9', "----."},
};

char uppercaseAscii(char character) {
  if (character >= 'a' && character <= 'z') {
    return static_cast<char>(character - 'a' + 'A');
  }

  return character;
}

const char* morsePatternFor(char character) {
  const char normalized = uppercaseAscii(character);
  for (const auto& entry : kMorseTable) {
    if (entry.character == normalized) {
      return entry.pattern;
    }
  }

  return nullptr;
}

int16_t clampPhaseErrorMs(int32_t phaseErrorMs) {
  if (phaseErrorMs < -32768) {
    return -32768;
  }

  if (phaseErrorMs > 32767) {
    return 32767;
  }

  return static_cast<int16_t>(phaseErrorMs);
}

const char* syncModeName(uint8_t flags) {
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

void logFlashVariationChange(uint32_t bar) {
  (void)bar;
  Serial.printf("FLASH: %s\n", activeFlashRenderCommand.name);
}

void resetFlashBurst() {
  flashBurst = {};
}

bool appendNodeTextSegment(bool on, uint16_t units) {
  while (units > 0U) {
    const uint8_t chunkUnits = static_cast<uint8_t>((units > 255U) ? 255U : units);
    if (nodeTextOverlay.segmentCount > 0 &&
        nodeTextOverlay.segments[nodeTextOverlay.segmentCount - 1U].on == on &&
        nodeTextOverlay.segments[nodeTextOverlay.segmentCount - 1U].units <=
          static_cast<uint8_t>(255U - chunkUnits)) {
      nodeTextOverlay.segments[nodeTextOverlay.segmentCount - 1U].units =
        static_cast<uint8_t>(nodeTextOverlay.segments[nodeTextOverlay.segmentCount - 1U].units +
                             chunkUnits);
    } else {
      if (nodeTextOverlay.segmentCount >= kNodeTextSegmentCapacity) {
        return false;
      }

      nodeTextOverlay.segments[nodeTextOverlay.segmentCount].units = chunkUnits;
      nodeTextOverlay.segments[nodeTextOverlay.segmentCount].on = on;
      nodeTextOverlay.segmentCount++;
    }

    units = static_cast<uint16_t>(units - chunkUnits);
  }

  return true;
}

bool appendMorsePattern(const char* pattern) {
  if (pattern == nullptr || pattern[0] == '\0') {
    return false;
  }

  for (size_t index = 0; pattern[index] != '\0'; ++index) {
    const uint16_t onUnits = (pattern[index] == '-') ? 2U : 1U;
    if (!appendNodeTextSegment(true, onUnits)) {
      return false;
    }

    if (pattern[index + 1U] != '\0' && !appendNodeTextSegment(false, 1U)) {
      return false;
    }
  }

  return true;
}

bool buildNodeTextOverlay(const char* text) {
  nodeTextOverlay.segmentCount = 0;

  bool haveLetter = false;
  bool wordGapPending = false;
  for (size_t index = 0; text != nullptr && text[index] != '\0'; ++index) {
    const char character = text[index];
    if (character == ' ') {
      if (haveLetter) {
        wordGapPending = true;
      }
      continue;
    }

    const char* pattern = morsePatternFor(character);
    if (pattern == nullptr) {
      continue;
    }

    if (haveLetter) {
      if (!appendNodeTextSegment(false, wordGapPending ? 4U : 2U)) {
        return false;
      }
    }

    if (!appendMorsePattern(pattern)) {
      return false;
    }

    haveLetter = true;
    wordGapPending = false;
  }

  return nodeTextOverlay.segmentCount > 0;
}

uint32_t nodeTextUnitsToMs(uint32_t units, uint32_t intervalMs) {
  return (units * intervalMs) / 2U;
}

void clearNodeTextOverlay(bool clearOutput = true) {
  if (clearOutput && (nodeTextOverlay.active || nodeTextOverlay.outputLit)) {
    renderer.showTemporaryLit(false);
  }

  nodeTextOverlay = {};
}

void queueNodeTextOverlay(const NodeTextMessage& message) {
  clearNodeTextOverlay();
  nodeTextOverlay.revision = message.textRevision;
  if (!buildNodeTextOverlay(message.text)) {
    Serial.printf("TEXT: ignored empty revision=%lu\n",
                  static_cast<unsigned long>(message.textRevision));
    clearNodeTextOverlay(false);
    return;
  }

  nodeTextOverlay.pending = true;
  Serial.printf("TEXT: queued revision=%lu segments=%u\n",
                static_cast<unsigned long>(nodeTextOverlay.revision),
                static_cast<unsigned>(nodeTextOverlay.segmentCount));
}

void startPendingNodeTextOverlay(uint32_t now) {
  if (!nodeTextOverlay.pending || nodeTextOverlay.segmentCount == 0) {
    return;
  }

  nodeTextOverlay.pending = false;
  nodeTextOverlay.active = true;
  nodeTextOverlay.startedAtMs = now;
  nodeTextOverlay.beatIntervalMs = (beatIntervalMs == 0U) ? bpmToIntervalMs(BPM) : beatIntervalMs;
  nodeTextOverlay.elapsedUnits = 0;
  nodeTextOverlay.segmentIndex = 0;
  nodeTextOverlay.nextSegmentAtMs =
    now + nodeTextUnitsToMs(nodeTextOverlay.segments[0].units, nodeTextOverlay.beatIntervalMs);
  nodeTextOverlay.outputLit = nodeTextOverlay.segments[0].on;
  resetFlashBurst();
  renderer.showTemporaryLit(nodeTextOverlay.outputLit);
  Serial.printf("TEXT: start revision=%lu\n",
                static_cast<unsigned long>(nodeTextOverlay.revision));
}

bool serviceNodeTextOverlay(uint32_t now) {
  if (!nodeTextOverlay.active) {
    return false;
  }

  while (nodeTextOverlay.segmentIndex < nodeTextOverlay.segmentCount &&
         (int32_t)(now - nodeTextOverlay.nextSegmentAtMs) >= 0) {
    nodeTextOverlay.elapsedUnits += nodeTextOverlay.segments[nodeTextOverlay.segmentIndex].units;
    nodeTextOverlay.segmentIndex++;
    if (nodeTextOverlay.segmentIndex < nodeTextOverlay.segmentCount) {
      nodeTextOverlay.nextSegmentAtMs =
        nodeTextOverlay.startedAtMs +
        nodeTextUnitsToMs(
          nodeTextOverlay.elapsedUnits +
            nodeTextOverlay.segments[nodeTextOverlay.segmentIndex].units,
          nodeTextOverlay.beatIntervalMs
        );
    }
  }

  if (nodeTextOverlay.segmentIndex >= nodeTextOverlay.segmentCount) {
    clearNodeTextOverlay();
    Serial.printf("TEXT: done revision=%lu\n",
                  static_cast<unsigned long>(lastNodeTextRevision));
    return false;
  }

  const bool nextLit = nodeTextOverlay.segments[nodeTextOverlay.segmentIndex].on;
  if (nextLit != nodeTextOverlay.outputLit) {
    renderer.showTemporaryLit(nextLit);
    nodeTextOverlay.outputLit = nextLit;
  }

  return true;
}

void refreshFlashRenderCommandForBar(uint32_t bar) {
  if (activeFlashCommand.mode == FlashCommandMode::Off) {
    activeFlashVariationEpoch = 0;
    activeFlashRenderCommand = REMOTE_IDLE_FLASH_RENDER_COMMAND;
    return;
  }

  const uint32_t variationEpoch = flashVariationEpochFor(activeFlashCommand, bar);
  if (variationEpoch == activeFlashVariationEpoch) {
    return;
  }

  activeFlashVariationEpoch = variationEpoch;
  activeFlashRenderCommand = flashRenderCommandFor(activeFlashCommand, bar);
  logFlashVariationChange(bar);
}

void bumpNodeProfileRevision() {
  nodeIdentity.profileRevision++;
  if (nodeIdentity.profileRevision == 0) {
    nodeIdentity.profileRevision = 1;
  }
}

uint8_t currentProgramIndexForStatus() {
  if (isBrainOwned(runMode)) {
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
    millis(),
    lastClockSyncTelemetry
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  const bool quietStatusReason =
    strcmp(reason, "heartbeat") == 0 || strcmp(reason, "clock_sync") == 0;
  if (result != ESP_OK || !quietStatusReason) {
    Serial.printf("SEND: node_status result=%d reason=%s kind=%s role=%s profile_rev=%u scene=",
                  result,
                  reason,
                  nodeKindName(nodeIdentity.nodeKind),
                  nodeRoleName(nodeIdentity.nodeEffect),
                  static_cast<unsigned>(nodeIdentity.profileRevision));
    if (message.currentProgramIndex == 255) {
      Serial.println("brain");
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
    activeFlashRenderCommand = REMOTE_IDLE_FLASH_RENDER_COMMAND;
    activeFlashVariationEpoch = UINT32_MAX;
    activeRgbCommand = REMOTE_IDLE_RGB_COMMAND;
    resetFlashBurst();
    renderer.setFlashCommand(activeFlashCommand);
    renderer.setRgbCommand(activeRgbCommand);
    return;
  }

  if (currentProgram >= currentProgramCount) {
    currentProgram = 0;
  }
}

bool wasSameBeatRenderedRecently(uint8_t beat, uint32_t bar, uint32_t now) {
  return lastBeatRenderedAtMs != 0 &&
         (now - lastBeatRenderedAtMs) <= DUPLICATE_BEAT_GUARD_MS &&
         lastRenderedBeatInBar == beat &&
         lastRenderedBar == bar;
}

void resetBeatRenderHistory() {
  lastBeatRenderedAtMs = 0;
  lastRenderedBeatInBar = 0;
  lastRenderedBar = 0;
}

void clearButtonGesture() {
  buttonPressed = false;
  longPressHandled = false;
  buttonPressedAtMs = 0;
}

void resetClockSyncTelemetry() {
  lastClockSyncTelemetry = {};
}

void enterBrainWaitingMode() {
  runMode = RunMode::BrainWaiting;
  lastClockRevision = 0;
  lastClockBeatSerial = 0;
  resetClockSyncTelemetry();
  resetBeatRenderHistory();
  clearButtonGesture();
}

void applyFlashCommand(const FlashCommand& command) {
  activeFlashCommand = command;
  activeFlashVariationEpoch = UINT32_MAX;
  refreshFlashRenderCommandForBar(currentBar);
  resetFlashBurst();
  renderer.setFlashCommand(activeFlashCommand);
  outputMuted = false;
  renderer.allOff();
}

void applyRgbCommand(const RgbCommand& command) {
  activeRgbCommand = command;
  renderer.setRgbCommand(activeRgbCommand);
  outputMuted = false;
}

void resetDemoClockState() {
  runMode = RunMode::Demo;
  lastRemoteCommandRevision = 0;
  lastClockRevision = 0;
  lastClockBeatSerial = 0;
  resetClockSyncTelemetry();
  clearButtonGesture();
  resetBeatRenderHistory();
  resetFlashBurst();
  beatIntervalMs = bpmToIntervalMs(BPM);
  beatsPerBar = DEFAULT_BEATS_PER_BAR;
  beatInBar = 1;
  currentBar = 1;
  const uint32_t now = millis();
  nextBeatAtMs = now + beatIntervalMs;
  renderer.syncBeatClock(now, beatIntervalMs, beatsPerBar, beatInBar, currentBar);
}

void selectProgram(size_t programIndex, bool announce = true) {
  if (currentProgramCount == 0) {
    return;
  }

  currentProgram = programIndex % currentProgramCount;
  resetDemoClockState();
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    applyFlashCommand(flashSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
  } else {
    applyRgbCommand(rgbSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
  }

  if (announce) {
    Serial.println();
    Serial.println("-----");
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      Serial.printf("SCENE %u (%s): profile=%s motif=%s | %s | %u BPM\n",
                    static_cast<unsigned>(currentProgram + 1),
                    sceneName(currentProgram),
                    activeFlashCommand.name,
                    activeFlashRenderCommand.name,
                    nodeKindName(nodeIdentity.nodeKind),
                    static_cast<unsigned>(currentBpmValue()));
    } else {
      Serial.printf("SCENE %u (%s): %s | %s | %s | %u BPM\n",
                    static_cast<unsigned>(currentProgram + 1),
                    sceneName(currentProgram),
                    activeRgbCommand.name,
                    nodeKindName(nodeIdentity.nodeKind),
                    nodeRoleName(nodeIdentity.nodeEffect),
                    static_cast<unsigned>(currentBpmValue()));
    }
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
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    Serial.printf("scenes=%s role=%s\n",
                  nodeKindName(nodeIdentity.nodeKind),
                  nodeRoleName(nodeIdentity.nodeEffect));

    for (size_t i = 0; i < currentProgramCount; ++i) {
      const FlashCommand command = flashSceneCommandFor(nodeIdentity.nodeEffect, i);
      Serial.printf("  %u -> %s | %s\n",
                    static_cast<unsigned>(i + 1),
                    sceneName(i),
                    command.name);
    }
    return;
  }

  Serial.printf("scenes=%s role=%s\n",
                nodeKindName(nodeIdentity.nodeKind),
                nodeRoleName(nodeIdentity.nodeEffect));

  for (size_t i = 0; i < currentProgramCount; ++i) {
    Serial.printf("  %u -> %s | %s\n",
                  static_cast<unsigned>(i + 1),
                  sceneName(i),
                  rgbSceneCommandFor(nodeIdentity.nodeEffect, i).name);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Button:");
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    Serial.println("  short press -> next scene");
    Serial.println("  long press  -> turn output off");
  } else {
    Serial.println("  short press -> next scene (demo only)");
    Serial.println("  long press  -> next role");
  }
  Serial.println();
  Serial.println("Serial:");
  Serial.println("  mode rgb");
  Serial.println("  mode flash");
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    Serial.println("  role pulse");
  } else {
    Serial.println("  role wash | pulse | accent | flicker");
  }
  Serial.println("  effect ...   (alias for role ...)");
  Serial.println("  status");
  Serial.println("  help");
  Serial.println();
}

void printStatus() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("NODE: mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("NODE: role=%s\n", nodeRoleName(nodeIdentity.nodeEffect));
  Serial.printf("RENDERER: %s\n", renderer.rendererName());
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    Serial.printf("SCENE: %u/%u name=%s profile=%s motif=%s\n",
                  static_cast<unsigned>(currentProgram + 1),
                  static_cast<unsigned>(currentProgramCount),
                  sceneName(currentProgram),
                  activeFlashCommand.name,
                  activeFlashRenderCommand.name);
  } else {
    Serial.printf("SCENE: %u/%u name=%s command=%s\n",
                  static_cast<unsigned>(currentProgram + 1),
                  static_cast<unsigned>(currentProgramCount),
                  sceneName(currentProgram),
                  activeRgbCommand.name);
  }
  Serial.printf("CONTROL: mode=%s bpm=%u muted=%u\n",
                runModeName(runMode),
                static_cast<unsigned>(currentBpmValue()),
                static_cast<unsigned>(outputMuted));
  if (lastClockSyncTelemetry.beatSerial == 0) {
    Serial.println("SYNC: waiting");
  } else {
    Serial.printf("SYNC: beat=%lu bar=%lu beat_in_bar=%u phase_ms=%d mode=%s\n",
                  static_cast<unsigned long>(lastClockSyncTelemetry.beatSerial),
                  static_cast<unsigned long>(lastClockSyncTelemetry.currentBar),
                  static_cast<unsigned>(lastClockSyncTelemetry.beatInBar),
                  static_cast<int>(lastClockSyncTelemetry.phaseErrorMs),
                  syncModeName(lastClockSyncTelemetry.flags));
  }
  if (nodeIdentity.nodeKind == NodeKind::RgbStrip) {
    SurfaceModulationState modulation = {};
    if (renderer.surfaceModulationState(millis(), modulation)) {
      Serial.printf("ACTIVITY: %u\n",
                    static_cast<unsigned>(modulation.activity));
      Serial.printf("NOISE: shadow=%u pocket=%u cool=%u\n",
                    static_cast<unsigned>(modulation.shadowDepth),
                    static_cast<unsigned>(modulation.pocketChance),
                    static_cast<unsigned>(modulation.coolShift));
      Serial.printf("COLOR: %s %u\n",
                    colorDriftSideName(modulation.colorDrift),
                    static_cast<unsigned>(colorDriftStrength(modulation.colorDrift)));
    }
  }
  Serial.println("-----");
}

void announceNodeProfile() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("NODE: mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("NODE: role=%s\n", nodeRoleName(nodeIdentity.nodeEffect));
  Serial.printf("RENDERER: %s\n", renderer.rendererName());
  Serial.printf("RUNTIME: %s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("-----");
}

void configureNodeProfile(NodeKind nodeKind, NodeRole nodeRole) {
  const NodeRole effectiveRole =
    roleCompatible(nodeKind, nodeRole) ? nodeRole : defaultRoleFor(nodeKind);
  nodeIdentity.nodeKind = nodeKind;
  nodeIdentity.nodeEffect = effectiveRole;
  renderer.setNodeProfile(nodeKind, effectiveRole);
  refreshProgramSet();
}

void switchNodeKind(NodeKind nodeKind, bool persist) {
  const NodeRole effectiveRole =
    roleCompatible(nodeKind, nodeIdentity.nodeEffect)
      ? nodeIdentity.nodeEffect
      : defaultRoleFor(nodeKind);
  const NodeKind previousKind = nodeIdentity.nodeKind;
  const NodeRole previousRole = nodeIdentity.nodeEffect;

  if (persist) {
    if (!saveNodeKind(nodeKind) || !saveNodeRole(effectiveRole)) {
      Serial.println("CONFIG: save_failed");
    }
  }

  const bool brainOwned = isBrainOwned(runMode);
  clearNodeTextOverlay();
  configureNodeProfile(nodeKind, effectiveRole);
  if (previousKind != nodeIdentity.nodeKind ||
      previousRole != nodeIdentity.nodeEffect) {
    bumpNodeProfileRevision();
  }
  announceNodeProfile();
  printPrograms();

  if (brainOwned) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      applyFlashCommand(REMOTE_IDLE_FLASH_COMMAND);
    } else {
      applyRgbCommand(REMOTE_IDLE_RGB_COMMAND);
    }
    lastRemoteCommandRevision = 0;
    enterBrainWaitingMode();
  } else {
    selectProgram(0);
  }

  sendNodeStatus("node_mode");
}

void switchNodeRole(NodeRole nodeRole, bool persist) {
  if (!roleCompatible(nodeIdentity.nodeKind, nodeRole)) {
    Serial.printf("SERIAL: role_mismatch role=%s mode=%s\n",
                  nodeRoleName(nodeRole),
                  nodeKindName(nodeIdentity.nodeKind));
    return;
  }

  if (persist && !saveNodeRole(nodeRole)) {
    Serial.println("CONFIG: save_failed");
  }

  const bool brainOwned = isBrainOwned(runMode);
  const NodeRole previousRole = nodeIdentity.nodeEffect;
  clearNodeTextOverlay();
  configureNodeProfile(nodeIdentity.nodeKind, nodeRole);
  if (previousRole != nodeIdentity.nodeEffect) {
    bumpNodeProfileRevision();
  }
  renderer.showRoleConfirm(nodeIdentity.nodeEffect);
  announceNodeProfile();
  printPrograms();

  if (brainOwned) {
    lastRemoteCommandRevision = 0;
  } else {
    selectProgram(0);
  }

  sendNodeStatus("node_role");
}

void cycleNodeRole(bool persist) {
  switchNodeRole(nextNodeRole(nodeIdentity.nodeKind, nodeIdentity.nodeEffect), persist);
}

void runBrainConnectSequence() {
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

void stageIncomingNodeText(const NodeTextMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingNodeTextMessage = message;
  hasPendingNodeText = true;
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
    return;
  }

  if (header.type == decaflash::protocol::MessageType::NodeText &&
      len == static_cast<int>(sizeof(NodeTextMessage))) {
    NodeTextMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::NodeText)) {
      return;
    }
    stageIncomingNodeText(message);
  }
}

void processPendingBrainHelloMessage(const BrainHelloMessage& message) {
  (void)message;
  clearNodeTextOverlay();
  runBrainConnectSequence();

  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    applyFlashCommand(REMOTE_IDLE_FLASH_COMMAND);
  } else {
    applyRgbCommand(REMOTE_IDLE_RGB_COMMAND);
  }
  lastRemoteCommandRevision = 0;
  enterBrainWaitingMode();
  Serial.println("BRAIN: waiting_for_clock");
  sendNodeStatus("brain_hello");
}

void processPendingFlashCommandMessage(const FlashCommandMessage& message) {
  if (nodeIdentity.nodeKind != NodeKind::Flashlight ||
      message.targetNodeKind != nodeIdentity.nodeKind ||
      message.targetNodeEffect != nodeIdentity.nodeEffect) {
    return;
  }

  if (isBrainOwned(runMode) &&
      message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  applyFlashCommand(message.command);
  lastRemoteCommandRevision = message.commandRevision;
  enterBrainWaitingMode();

  Serial.printf("BRAIN: flash command=%s role=%s bpm=%u\n",
                activeFlashCommand.name,
                nodeRoleName(nodeIdentity.nodeEffect),
                static_cast<unsigned>(currentBpmValue()));
  sendNodeStatus("remote_flash");
}

void processPendingRgbCommandMessage(const RgbCommandMessage& message) {
  if (nodeIdentity.nodeKind != NodeKind::RgbStrip ||
      message.targetNodeKind != nodeIdentity.nodeKind ||
      message.targetNodeEffect != nodeIdentity.nodeEffect) {
    return;
  }

  if (isBrainOwned(runMode) &&
      message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  applyRgbCommand(message.command);
  lastRemoteCommandRevision = message.commandRevision;
  enterBrainWaitingMode();

  Serial.printf("BRAIN: rgb command=%s role=%s bpm=%u\n",
                activeRgbCommand.name,
                nodeRoleName(nodeIdentity.nodeEffect),
                static_cast<unsigned>(currentBpmValue()));
  sendNodeStatus("remote_rgb");
}

void processPendingNodeTextMessage(const NodeTextMessage& message) {
  if (message.targetNodeKind != nodeIdentity.nodeKind) {
    return;
  }

  if (message.textRevision == lastNodeTextRevision) {
    return;
  }

  lastNodeTextRevision = message.textRevision;

  if ((message.flags & decaflash::protocol::kNodeTextFlagCancel) != 0U) {
    clearNodeTextOverlay();
    Serial.printf("TEXT: cancel revision=%lu\n",
                  static_cast<unsigned long>(message.textRevision));
    return;
  }

  if (runMode != RunMode::BrainRunning) {
    Serial.printf("TEXT: ignore waiting_for_clock revision=%lu\n",
                  static_cast<unsigned long>(message.textRevision));
    return;
  }

  queueNodeTextOverlay(message);
}

ClockSyncDiagnostics analyzeClockSync(
  const ClockSyncMessage& message,
  uint32_t now,
  bool wasBrainRunning
) {
  ClockSyncDiagnostics diagnostics = {};

  if (wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now)) {
    diagnostics.phaseErrorMs = clampPhaseErrorMs(
      static_cast<int32_t>(now) - static_cast<int32_t>(lastBeatRenderedAtMs)
    );
    diagnostics.flags =
      decaflash::protocol::kNodeClockSyncFlagMeasured |
      decaflash::protocol::kNodeClockSyncFlagDuplicateBeat;
    return diagnostics;
  }

  if (wasBrainRunning &&
      beatIntervalMs != 0 &&
      message.beatInBar == beatInBar &&
      message.currentBar == currentBar) {
    diagnostics.phaseErrorMs = clampPhaseErrorMs(
      static_cast<int32_t>(now) - static_cast<int32_t>(nextBeatAtMs)
    );
    diagnostics.flags =
      decaflash::protocol::kNodeClockSyncFlagMeasured |
      decaflash::protocol::kNodeClockSyncFlagPredictedBeat;
    return diagnostics;
  }

  diagnostics.flags = decaflash::protocol::kNodeClockSyncFlagResync;
  return diagnostics;
}

void applyClockSync(const ClockSyncMessage& message) {
  if (!isBrainOwned(runMode)) {
    return;
  }

  if (message.clockRevision == lastClockRevision &&
      message.beatSerial == lastClockBeatSerial) {
    return;
  }

  lastClockRevision = message.clockRevision;
  lastClockBeatSerial = message.beatSerial;
  beatIntervalMs = bpmToIntervalMs(message.bpm);
  beatsPerBar = (message.beatsPerBar == 0) ? DEFAULT_BEATS_PER_BAR : message.beatsPerBar;
  const bool wasBrainRunning = (runMode == RunMode::BrainRunning);
  runMode = RunMode::BrainRunning;
  const uint32_t now = millis();
  lastClockSyncTelemetry.clockRevision = message.clockRevision;
  lastClockSyncTelemetry.beatSerial = message.beatSerial;
  lastClockSyncTelemetry.currentBar = (message.currentBar == 0) ? 1U : message.currentBar;
  lastClockSyncTelemetry.beatInBar = (message.beatInBar == 0) ? 1U : message.beatInBar;
  const ClockSyncDiagnostics diagnostics = analyzeClockSync(message, now, wasBrainRunning);
  lastClockSyncTelemetry.phaseErrorMs = diagnostics.phaseErrorMs;
  lastClockSyncTelemetry.flags = diagnostics.flags;

  if (wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now)) {
    nextBeatAtMs = now + beatIntervalMs;
    sendNodeStatus("clock_sync");
    return;
  }

  beatInBar = (message.beatInBar == 0) ? 1U : message.beatInBar;
  currentBar = (message.currentBar == 0) ? 1U : message.currentBar;
  onBeat();
  nextBeatAtMs = now + beatIntervalMs;
  sendNodeStatus("clock_sync");
}

void processPendingRadio() {
  bool hadFlashCommand = false;
  bool hadRgbCommand = false;
  bool hadClockSync = false;
  bool hadBrainHello = false;
  bool hadNodeText = false;
  FlashCommandMessage flashCommandMessage = {};
  RgbCommandMessage rgbCommandMessage = {};
  ClockSyncMessage clockMessage = {};
  BrainHelloMessage brainHelloMessage = {};
  NodeTextMessage nodeTextMessage = {};

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
  if (hasPendingNodeText) {
    nodeTextMessage = pendingNodeTextMessage;
    hasPendingNodeText = false;
    hadNodeText = true;
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

  if (hadNodeText) {
    processPendingNodeTextMessage(nodeTextMessage);
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

void serviceFilterMonitor() {
  if (nodeIdentity.nodeKind != NodeKind::RgbStrip || outputMuted) {
    return;
  }

  const uint32_t now = millis();
  if ((int32_t)(now - nextFilterMonitorAtMs) < 0) {
    return;
  }

  SurfaceModulationState modulation = {};
  if (renderer.surfaceModulationState(now, modulation)) {
    Serial.printf("ACTIVITY: %u\n",
                  static_cast<unsigned>(modulation.activity));
    Serial.printf("NOISE: shadow=%u pocket=%u cool=%u\n",
                  static_cast<unsigned>(modulation.shadowDepth),
                  static_cast<unsigned>(modulation.pocketChance),
                  static_cast<unsigned>(modulation.coolShift));
    Serial.printf("COLOR: %s %u\n",
                  colorDriftSideName(modulation.colorDrift),
                  static_cast<unsigned>(colorDriftStrength(modulation.colorDrift)));
  }

  nextFilterMonitorAtMs = now + FILTER_MONITOR_INTERVAL_MS;
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
      Serial.println("BUTTON: long_press");
      if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
        outputMuted = true;
        clearNodeTextOverlay();
        resetFlashBurst();
        renderer.allOff();
      } else {
        cycleNodeRole(true);
      }
    }
    return;
  }

  if (!isPressed && buttonPressed) {
    buttonPressed = false;

    if (!longPressHandled && runMode == RunMode::Demo) {
      Serial.println("BUTTON: short_press");
      selectNextProgram();
    }
  }
}

uint32_t clampBurstInterval(int32_t intervalMs) {
  if (intervalMs < 80) {
    return 80;
  }

  return static_cast<uint32_t>(intervalMs);
}

void startFlashBurst(const FlashRenderCommand& command) {
  if (command.burstCount == 0 || command.flashDurationMs == 0) {
    return;
  }

  flashBurst.active = true;
  flashBurst.remaining = command.burstCount;
  flashBurst.intervalMs = command.burstIntervalMs;
  flashBurst.nextFlashAtMs = millis();
  flashBurst.intervalStepMs = command.burstIntervalStepMs;
  flashBurst.flashDurationMs = command.flashDurationMs;
}

void serviceFlashBurst() {
  if (nodeIdentity.nodeKind != NodeKind::Flashlight || outputMuted || !flashBurst.active) {
    return;
  }

  const uint32_t now = millis();
  if ((int32_t)(now - flashBurst.nextFlashAtMs) < 0) {
    return;
  }

  renderer.flash100(flashBurst.flashDurationMs);

  if (flashBurst.remaining > 0) {
    flashBurst.remaining--;
  }

  if (flashBurst.remaining == 0) {
    flashBurst.active = false;
    return;
  }

  flashBurst.intervalMs = clampBurstInterval(
    static_cast<int32_t>(flashBurst.intervalMs) + flashBurst.intervalStepMs
  );
  flashBurst.nextFlashAtMs = millis() + flashBurst.intervalMs;
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
  startPendingNodeTextOverlay(now);

  if (!outputMuted && !nodeTextOverlay.active) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      refreshFlashRenderCommandForBar(currentBar);
      const bool trigger = isTriggerBeat(
        activeFlashRenderCommand.triggerEveryBars,
        activeFlashRenderCommand.triggerBeat
      );

      switch (activeFlashRenderCommand.pattern) {
        case FlashPattern::BeatPulse:
          if (trigger && activeFlashRenderCommand.flashDurationMs > 0) {
            renderer.flash100(activeFlashRenderCommand.flashDurationMs);
          }
          break;

        case FlashPattern::BarBurst:
          if (trigger) {
            startFlashBurst(activeFlashRenderCommand);
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
        case RgbPattern::RunnerFlicker:
          trigger = isTriggerBeat(activeRgbCommand.triggerEveryBars, activeRgbCommand.triggerBeat);
          break;

        case RgbPattern::BeatPulse:
        case RgbPattern::Accent:
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
  if (runMode == RunMode::BrainWaiting) {
    return;
  }

  const uint32_t now = millis();
  while ((int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }
}

void serviceOutput() {
  if (outputMuted) {
    return;
  }

  if (serviceNodeTextOverlay(millis())) {
    return;
  }

  serviceFlashBurst();
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

bool parseNodeRole(const char* line, NodeRole& parsedRole) {
  if (strcmp(line, "role wash") == 0 ||
      strcmp(line, "effect wash") == 0 ||
      strcmp(line, "effect ambient") == 0 ||
      strcmp(line, "effect bluewash") == 0) {
    parsedRole = NodeRole::Wash;
    return true;
  }

  if (strcmp(line, "role pulse") == 0 ||
      strcmp(line, "effect pulse") == 0 ||
      strcmp(line, "effect bluepulse") == 0 ||
      strcmp(line, "effect flashpulse") == 0) {
    parsedRole = NodeRole::Pulse;
    return true;
  }

  if (strcmp(line, "role accent") == 0 ||
      strcmp(line, "effect accent") == 0 ||
      strcmp(line, "effect redaccent") == 0 ||
      strcmp(line, "effect flashaccent") == 0) {
    parsedRole = NodeRole::Accent;
    return true;
  }

  if (strcmp(line, "role flicker") == 0 ||
      strcmp(line, "effect flicker") == 0 ||
      strcmp(line, "effect motion") == 0 ||
      strcmp(line, "effect blueredflicker") == 0) {
    parsedRole = NodeRole::Flicker;
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

  NodeRole parsedRole = NodeRole::None;
  if (parseNodeRole(line, parsedRole)) {
    switchNodeRole(parsedRole, true);
    return;
  }

  Serial.printf("SERIAL: unknown_command '%s'\n", line);
  Serial.println("SERIAL: use 'help'");
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
  const NodeRole storedRole = loadStoredNodeRole(storedKind);
  configureNodeProfile(storedKind, storedRole);

  Serial.println();
  Serial.println("Decaflash Node V2");
  Serial.printf(
    "DEVICE: type=%u node_kind=%u node_role=%u\n",
    static_cast<unsigned>(nodeIdentity.deviceType),
    static_cast<unsigned>(nodeIdentity.nodeKind),
    static_cast<unsigned>(nodeIdentity.nodeEffect)
  );
  Serial.printf("NODE: mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("NODE: role=%s\n", nodeRoleName(nodeIdentity.nodeEffect));
  Serial.printf("RUNTIME: %s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("CONTROLS: atom button + serial");
  Serial.printf("RENDERER: %s\n", renderer.rendererName());
  if (nodeIdentity.nodeKind == NodeKind::RgbStrip) {
    Serial.println("RGB-DRIVER: sk6812 pins=26+32");
  }
  Serial.printf("CLOCK: %u bpm %u/4\n", BPM, beatsPerBar);

  const auto initResult = initEspNow();
  decaflash::espnow_transport::PeerResult peerResult = {};
  if (initResult.ok()) {
    peerResult = ensureBroadcastPeer();
  }
  espNowReady = initResult.ok() && peerResult.ok();

  Serial.printf("WIFI: set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("WIFI: start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("WIFI: set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("ESP-NOW: init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("ESP-NOW: state=%s\n", espNowReady ? "ok" : "failed");
  Serial.printf("ESP-NOW: peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("ESP-NOW: add_peer=%d\n", static_cast<int>(peerResult.addPeer));
  if (espNowReady) {
    esp_now_register_recv_cb(onEspNowReceive);
  }
  Serial.println("RUNTIME: demo scene playback + brain assignment receive");
  Serial.println("NODE-STACK: kind+role aware");
  printPrograms();
  printHelp();
  selectProgram(0);
  if (espNowReady) {
    sendNodeStatus("boot");
    nextStatusAtMs = millis() + NODE_STATUS_INTERVAL_MS;
  }
  nextFilterMonitorAtMs = millis() + FILTER_MONITOR_INTERVAL_MS;
}

void loop() {
  serviceSerial();
  processPendingRadio();
  serviceButton();
  serviceClock();
  serviceOutput();
  serviceNodeStatus();
  serviceFilterMonitor();
}
