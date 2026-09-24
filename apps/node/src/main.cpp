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
using decaflash::RunnerMotion;
using decaflash::RunnerPresentation;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::protocol::BrainHelloMessage;
using decaflash::protocol::ClockSyncMessage;
using decaflash::protocol::NodeTextMessage;
using decaflash::protocol::SceneSelectMessage;
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
  0, 0, 0,
  0, 0, 0,
  0, 0, 0,
  1, 1,
  0, 1200, 0,
  0,
  0,
  1, 0, 255,
  RunnerMotion::Bounce,
  RunnerPresentation::Parallel,
  0,
  {},
};

NodeIdentity nodeIdentity = {
  DEVICE_TYPE,
  DEFAULT_NODE_KIND,
  NodeRole::Pulse,
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
  uint32_t startedAtMs = 0;
  uint32_t nextUnitAtMs = 0;
  size_t segmentCount = 0;
  size_t segmentIndex = 0;
  uint8_t segmentUnitsRemaining = 0;
  bool currentUnitStartsOnBeat = false;
  NodeTextSegment segments[kNodeTextSegmentCapacity] = {};
};

NodeTextOverlayState nodeTextOverlay;

enum class RunMode : uint8_t {
  Demo = 0,
  BrainWaiting = 1,
  BrainRunning = 2,
};

RunMode runMode = RunMode::Demo;
uint32_t lastBeatRenderedAtMs = 0;
uint8_t lastRenderedBeatInBar = 0;
uint32_t lastRenderedBar = 0;

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool hasPendingSceneSelect = false;
volatile bool hasPendingClockSync = false;
volatile bool hasPendingBrainHello = false;
volatile bool hasPendingNodeText = false;
SceneSelectMessage pendingSceneSelectMessage = {};
ClockSyncMessage pendingClockSyncMessage = {};
BrainHelloMessage pendingBrainHelloMessage = {};
NodeTextMessage pendingNodeTextMessage = {};

void onBeat();

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

uint32_t currentBeatIntervalMs() {
  return (beatIntervalMs == 0U) ? bpmToIntervalMs(BPM) : beatIntervalMs;
}

uint32_t nodeTextUnitDurationMs(bool startsOnBeat) {
  const uint32_t intervalMs = currentBeatIntervalMs();
  const uint32_t firstHalfMs = intervalMs / 2U;
  const uint32_t secondHalfMs = intervalMs - firstHalfMs;
  const uint32_t durationMs = startsOnBeat ? firstHalfMs : secondHalfMs;
  return (durationMs == 0U) ? 1U : durationMs;
}

void clearNodeTextOverlay(bool clearOutput = true) {
  if (clearOutput && (nodeTextOverlay.active || nodeTextOverlay.outputLit)) {
    renderer.showTemporaryLit(false);
  }

  nodeTextOverlay = {};
}

void queueNodeTextOverlay(const NodeTextMessage& message) {
  clearNodeTextOverlay();
  if (!buildNodeTextOverlay(message.text)) {
    Serial.println("TEXT: ignored empty");
    clearNodeTextOverlay(false);
    return;
  }

  nodeTextOverlay.pending = true;
  Serial.printf("TEXT: queued segments=%u\n",
                static_cast<unsigned>(nodeTextOverlay.segmentCount));
}

bool advanceNodeTextOverlayBoundary(uint32_t boundaryAtMs, bool nextUnitStartsOnBeat) {
  if (!nodeTextOverlay.active || nodeTextOverlay.segmentIndex >= nodeTextOverlay.segmentCount) {
    return false;
  }

  if (nodeTextOverlay.segmentUnitsRemaining > 0U) {
    nodeTextOverlay.segmentUnitsRemaining--;
  }

  while (nodeTextOverlay.segmentUnitsRemaining == 0U) {
    nodeTextOverlay.segmentIndex++;
    if (nodeTextOverlay.segmentIndex >= nodeTextOverlay.segmentCount) {
      clearNodeTextOverlay();
      Serial.println("TEXT: done");
      return false;
    }

    nodeTextOverlay.segmentUnitsRemaining = nodeTextOverlay.segments[nodeTextOverlay.segmentIndex].units;
  }

  const bool nextLit = nodeTextOverlay.segments[nodeTextOverlay.segmentIndex].on;
  if (nextLit != nodeTextOverlay.outputLit) {
    renderer.showTemporaryLit(nextLit);
    nodeTextOverlay.outputLit = nextLit;
  }

  nodeTextOverlay.currentUnitStartsOnBeat = nextUnitStartsOnBeat;
  nodeTextOverlay.nextUnitAtMs = boundaryAtMs + nodeTextUnitDurationMs(nextUnitStartsOnBeat);
  return true;
}

bool startPendingNodeTextOverlay(uint32_t now) {
  if (!nodeTextOverlay.pending || nodeTextOverlay.segmentCount == 0) {
    return false;
  }

  nodeTextOverlay.pending = false;
  nodeTextOverlay.active = true;
  nodeTextOverlay.startedAtMs = now;
  nodeTextOverlay.segmentIndex = 0;
  nodeTextOverlay.segmentUnitsRemaining = nodeTextOverlay.segments[0].units;
  nodeTextOverlay.outputLit = nodeTextOverlay.segments[0].on;
  nodeTextOverlay.currentUnitStartsOnBeat = true;
  nodeTextOverlay.nextUnitAtMs = now + nodeTextUnitDurationMs(true);
  resetFlashBurst();
  renderer.showTemporaryLit(nodeTextOverlay.outputLit);
  Serial.println("TEXT: start");
  return true;
}

void syncNodeTextOverlayToBeat(uint32_t now, bool startedAtThisBeat) {
  if (!nodeTextOverlay.active || startedAtThisBeat) {
    return;
  }

  if (nodeTextOverlay.currentUnitStartsOnBeat) {
    nodeTextOverlay.nextUnitAtMs = now + nodeTextUnitDurationMs(true);
    return;
  }

  advanceNodeTextOverlayBoundary(now, true);
}

bool serviceNodeTextOverlay(uint32_t now) {
  if (!nodeTextOverlay.active) {
    return false;
  }

  while (nodeTextOverlay.active && (int32_t)(now - nodeTextOverlay.nextUnitAtMs) >= 0) {
    if (!advanceNodeTextOverlayBoundary(
          nodeTextOverlay.nextUnitAtMs,
          !nodeTextOverlay.currentUnitStartsOnBeat
        )) {
      return false;
    }
  }

  return nodeTextOverlay.active;
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

void enterBrainWaitingMode() {
  runMode = RunMode::BrainWaiting;
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
  Serial.printf("CLOCK: bar=%lu beat=%u/%u\n",
                static_cast<unsigned long>(currentBar),
                static_cast<unsigned>(beatInBar),
                static_cast<unsigned>(beatsPerBar));
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
  if (persist) {
    if (!saveNodeKind(nodeKind) || !saveNodeRole(effectiveRole)) {
      Serial.println("CONFIG: save_failed");
    }
  }

  const bool brainOwned = runMode != RunMode::Demo;
  clearNodeTextOverlay();
  configureNodeProfile(nodeKind, effectiveRole);
  announceNodeProfile();
  printPrograms();

  if (brainOwned) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      applyFlashCommand(flashSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
    } else {
      applyRgbCommand(rgbSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
    }
  } else {
    selectProgram(0);
  }
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

  const bool brainOwned = runMode != RunMode::Demo;
  clearNodeTextOverlay();
  configureNodeProfile(nodeIdentity.nodeKind, nodeRole);
  renderer.showRoleConfirm(nodeIdentity.nodeEffect);
  announceNodeProfile();
  printPrograms();

  if (brainOwned) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      applyFlashCommand(flashSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
    } else {
      applyRgbCommand(rgbSceneCommandFor(nodeIdentity.nodeEffect, currentProgram));
    }
  } else {
    selectProgram(0);
  }
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

void stageIncomingSceneSelect(const SceneSelectMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingSceneSelectMessage = message;
  hasPendingSceneSelect = true;
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

  if (header.type == decaflash::protocol::MessageType::SceneSelect &&
      len == static_cast<int>(sizeof(SceneSelectMessage))) {
    SceneSelectMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::SceneSelect)) {
      return;
    }
    stageIncomingSceneSelect(message);
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
    if (message.targetNodeKind == nodeIdentity.nodeKind) {
      stageIncomingNodeText(message);
    }
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
  enterBrainWaitingMode();
  Serial.println("BRAIN: waiting_for_clock");
}

void processPendingSceneSelectMessage(const SceneSelectMessage& message) {
  if (message.sceneIndex >= kSceneCount || currentProgramCount == 0) {
    return;
  }

  currentProgram = message.sceneIndex;
  if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
    const FlashCommand command = flashSceneCommandFor(nodeIdentity.nodeEffect, currentProgram);
    if (memcmp(&command, &activeFlashCommand, sizeof(command)) != 0) {
      applyFlashCommand(command);
    }
  } else {
    const RgbCommand& command = rgbSceneCommandFor(nodeIdentity.nodeEffect, currentProgram);
    if (memcmp(&command, &activeRgbCommand, sizeof(command)) != 0) {
      applyRgbCommand(command);
    }
  }
}

void processPendingNodeTextMessage(const NodeTextMessage& message) {
  if (message.targetNodeKind != nodeIdentity.nodeKind) {
    return;
  }

  if ((message.flags & decaflash::protocol::kNodeTextFlagCancel) != 0U) {
    clearNodeTextOverlay();
    Serial.println("TEXT: cancel");
    return;
  }

  if (runMode != RunMode::BrainRunning) {
    Serial.println("TEXT: ignore waiting_for_clock");
    return;
  }

  queueNodeTextOverlay(message);
}

void applyClockSync(const ClockSyncMessage& message) {
  if (message.bpm == 0) {
    return;
  }

  beatIntervalMs = bpmToIntervalMs(message.bpm);
  beatsPerBar = (message.beatsPerBar == 0) ? DEFAULT_BEATS_PER_BAR : message.beatsPerBar;
  runMode = RunMode::BrainRunning;
  const uint32_t now = millis();

  if (wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now)) {
    nextBeatAtMs = now + beatIntervalMs;
    return;
  }

  beatInBar = (message.beatInBar == 0) ? 1U : message.beatInBar;
  currentBar = (message.currentBar == 0) ? 1U : message.currentBar;
  onBeat();
  nextBeatAtMs = now + beatIntervalMs;
}

void processPendingRadio() {
  bool hadSceneSelect = false;
  bool hadClockSync = false;
  bool hadBrainHello = false;
  bool hadNodeText = false;
  SceneSelectMessage sceneSelectMessage = {};
  ClockSyncMessage clockMessage = {};
  BrainHelloMessage brainHelloMessage = {};
  NodeTextMessage nodeTextMessage = {};

  portENTER_CRITICAL(&radioMux);
  if (hasPendingSceneSelect) {
    sceneSelectMessage = pendingSceneSelectMessage;
    hasPendingSceneSelect = false;
    hadSceneSelect = true;
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

  if (hadSceneSelect) {
    processPendingSceneSelectMessage(sceneSelectMessage);
  }

  if (hadClockSync) {
    applyClockSync(clockMessage);
  }

  if (hadNodeText) {
    processPendingNodeTextMessage(nodeTextMessage);
  }
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
  const bool startedNodeTextOverlay = startPendingNodeTextOverlay(now);
  syncNodeTextOverlayToBeat(now, startedNodeTextOverlay);

  if (!outputMuted && !nodeTextOverlay.active) {
    if (nodeIdentity.nodeKind == NodeKind::Flashlight) {
      refreshFlashRenderCommandForBar(currentBar);
      const bool trigger = isTriggerBeat(
        activeFlashRenderCommand.triggerEveryBars,
        activeFlashRenderCommand.triggerBeat
      );

      switch (activeFlashRenderCommand.pattern) {
        case FlashPattern::Pulse:
          if (trigger && activeFlashRenderCommand.flashDurationMs > 0) {
            renderer.flash100(activeFlashRenderCommand.flashDurationMs);
          }
          break;

        case FlashPattern::PulseRow:
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
      switch (activeRgbCommand.pattern) {
        case RgbPattern::PulseRow:
          if (isTriggerBeat(activeRgbCommand.triggerEveryBars, activeRgbCommand.triggerBeat)) {
            renderer.triggerRgbPulseRow();
          }
          break;

        // A runner is continuously beat-clocked by the RGB renderer. It has no
        // one-shot trigger to schedule here.
        case RgbPattern::Runner:
        case RgbPattern::Pulse:
        case RgbPattern::Heartbeat:
        case RgbPattern::RiserPulse:
        case RgbPattern::Wave:
        case RgbPattern::Off:
        default:
          break;
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
  nextFilterMonitorAtMs = millis() + FILTER_MONITOR_INTERVAL_MS;
}

void loop() {
  serviceSerial();
  processPendingRadio();
  serviceButton();
  serviceClock();
  serviceOutput();
  serviceFilterMonitor();
}
