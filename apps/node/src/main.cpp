#include <Arduino.h>
#include <Preferences.h>

#include <cctype>
#include <cstring>

#include "decaflash_types.h"
#include "espnow_transport.h"
#include "node_output.h"
#include "node_programs.h"
#include "protocol.h"

using decaflash::EffectType;
using decaflash::DeviceType;
using decaflash::NodeCommand;
using decaflash::NodeIdentity;
using decaflash::NodeKind;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::node::ProgramSet;
using decaflash::node::programSetFor;
using decaflash::protocol::BrainHelloMessage;
using decaflash::protocol::ClockSyncMessage;
using decaflash::protocol::NodeCommandMessage;
using decaflash::protocol::makeNodeStatusMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Node;
static constexpr NodeKind DEFAULT_NODE_KIND = NodeKind::RgbStrip;
static constexpr char kConfigNamespace[] = "decaflash";
static constexpr char kConfigNodeKindKey[] = "node_kind";
static constexpr size_t kSerialLineCapacity = 48;
static constexpr uint32_t NODE_STATUS_INTERVAL_MS = 5000;
static constexpr int BUTTON_PIN = 39;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr uint32_t CLOCK_SYNC_TIMEOUT_MS = 4000;
static constexpr uint8_t BRAIN_CONNECT_FLASH_COUNT = 3;
static constexpr uint32_t BRAIN_CONNECT_FLASH_INTERVAL_MS = 1000;
static constexpr uint16_t BRAIN_CONNECT_FLASH_DURATION_MS = 260;
static constexpr uint32_t SOFT_SYNC_MATCH_WINDOW_MS = 120;
static constexpr uint32_t DUPLICATE_BEAT_GUARD_MS = 250;
static constexpr int32_t SOFT_SYNC_MAX_CORRECTION_MS = 30;
static constexpr int32_t HARD_SYNC_ERROR_MS = 180;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t DEFAULT_BEATS_PER_BAR = 4;
static constexpr NodeCommand REMOTE_IDLE_COMMAND = {
  "Remote Idle",
  EffectType::Off,
  255,
  1,
  1,
  0,
  0,
  0,
  0,
};

NodeIdentity nodeIdentity = {
  DEVICE_TYPE,
  DEFAULT_NODE_KIND,
};

NodeOutput renderer;
const NodeCommand* currentPrograms = nullptr;
size_t currentProgramCount = 0;
size_t currentProgram = 0;
NodeCommand activeCommand = REMOTE_IDLE_COMMAND;
Preferences preferences;
char serialLine[kSerialLineCapacity] = {};
size_t serialLineLength = 0;
bool espNowReady = false;
uint32_t nextStatusAtMs = 0;

bool buttonPressed = false;
bool longPressHandled = false;
uint32_t buttonChangedAtMs = 0;
uint32_t buttonPressedAtMs = 0;
bool lastButtonLevel = HIGH;

uint32_t effectStartedAtMs = 0;
uint32_t nextBeatAtMs = 0;
uint32_t beatIntervalMs = 0;
uint32_t globalBeat = 0;
uint32_t currentBar = 1;
uint8_t beatInBar = 1;
uint8_t beatsPerBar = DEFAULT_BEATS_PER_BAR;

struct BurstState {
  bool active = false;
  uint8_t remaining = 0;
  uint32_t intervalMs = 0;
  uint32_t nextFlashAtMs = 0;
  int16_t intervalStepMs = 0;
  uint16_t flashDurationMs = 120;
};

BurstState burst;
bool brainConnected = false;
bool remoteControlActive = false;
bool awaitingRemoteClock = false;
uint32_t lastRemoteCommandRevision = 0;
uint32_t lastClockRevision = 0;
uint32_t lastClockBeatSerial = 0;
uint32_t lastClockSyncAtMs = 0;
bool clockLocked = false;
bool clockHoldoverAnnounced = false;
uint32_t lastBeatRenderedAtMs = 0;
uint8_t lastRenderedBeatInBar = 0;
uint32_t lastRenderedBar = 0;
uint8_t lastPrintedBeatInBar = 0;
uint32_t lastPrintedBar = 0;

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool hasPendingCommand = false;
volatile bool hasPendingClockSync = false;
volatile bool hasPendingBrainHello = false;
NodeCommandMessage pendingCommandMessage = {};
ClockSyncMessage pendingClockSyncMessage = {};
BrainHelloMessage pendingBrainHelloMessage = {};

void onBeat();

const char* nodeKindName(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return "rgb";

    case NodeKind::Flashlight:
    default:
      return "flash";
  }
}

const char* runtimeLabelFor(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return "standalone rgb strip demo";

    case NodeKind::Flashlight:
    default:
      return "standalone flash demo";
  }
}

bool isSupportedNodeKind(uint8_t rawNodeKind) {
  return rawNodeKind == static_cast<uint8_t>(NodeKind::Flashlight) ||
         rawNodeKind == static_cast<uint8_t>(NodeKind::RgbStrip);
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

  Serial.printf("send=node_status result=%d reason=%s kind=%s program=%u\n",
                result,
                reason,
                nodeKindName(nodeIdentity.nodeKind),
                static_cast<unsigned>(message.currentProgramIndex));
}

void refreshProgramSet() {
  const ProgramSet programSet = programSetFor(nodeIdentity.nodeKind);
  currentPrograms = programSet.programs;
  currentProgramCount = programSet.count;

  if (currentProgramCount == 0) {
    currentProgram = 0;
    activeCommand = REMOTE_IDLE_COMMAND;
    renderer.setCommand(activeCommand);
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

void applyCommand(const NodeCommand& command) {
  activeCommand = command;
  renderer.setCommand(activeCommand);
  effectStartedAtMs = millis();
  burst.active = false;
  burst.remaining = 0;
  renderer.allOff();
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
  clockHoldoverAnnounced = false;
  lastBeatRenderedAtMs = 0;
  lastRenderedBeatInBar = 0;
  lastRenderedBar = 0;
  lastPrintedBeatInBar = 0;
  lastPrintedBar = 0;
  burst.active = false;
  burst.remaining = 0;
  globalBeat = 0;
  beatIntervalMs = bpmToIntervalMs(BPM);
  beatsPerBar = DEFAULT_BEATS_PER_BAR;
  beatInBar = 1;
  currentBar = 1;
  nextBeatAtMs = millis() + beatIntervalMs;
}

void selectProgram(size_t programIndex, bool announce = true) {
  if (currentProgramCount == 0) {
    return;
  }

  currentProgram = programIndex % currentProgramCount;
  applyCommand(currentPrograms[currentProgram]);
  resetLocalClockState();

  if (announce) {
    Serial.println();
    Serial.println("-----");
    Serial.printf("PROGRAM %u: %s | %s | %u BPM\n",
                  static_cast<unsigned>(currentProgram + 1),
                  activeCommand.name,
                  nodeKindName(nodeIdentity.nodeKind),
                  static_cast<unsigned>(currentBpmValue()));
    Serial.println("-----");
  }

  sendNodeStatus("program");
}

void selectNextProgram() {
  if (currentProgramCount == 0) {
    return;
  }

  selectProgram((currentProgram + 1) % currentProgramCount);
}

void printPrograms() {
  Serial.printf("programs=%s\n", nodeKindName(nodeIdentity.nodeKind));

  for (size_t i = 0; i < currentProgramCount; ++i) {
    Serial.printf("  %u -> %s\n", static_cast<unsigned>(i + 1), currentPrograms[i].name);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Button:");
  Serial.println("  short press -> next program");
  Serial.println("  long press  -> turn output off");
  Serial.println();
  Serial.println("Serial:");
  Serial.println("  mode rgb");
  Serial.println("  mode flash");
  Serial.println("  status");
  Serial.println("  help");
  Serial.println();
}

void printStatus() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("node_kind=%s renderer=%s\n",
                nodeKindName(nodeIdentity.nodeKind),
                renderer.rendererName());
  Serial.printf("program=%u/%u command=%s\n",
                static_cast<unsigned>(currentProgram + 1),
                static_cast<unsigned>(currentProgramCount),
                activeCommand.name);
  Serial.printf("control=%s clock=%s bpm=%u\n",
                remoteControlActive ? "remote" : "local",
                clockLocked ? "locked" : "local",
                static_cast<unsigned>(currentBpmValue()));
  Serial.println("-----");
}

void announceNodeKind() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("node_kind=%s renderer=%s\n",
                nodeKindName(nodeIdentity.nodeKind),
                renderer.rendererName());
  Serial.printf("runtime=%s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("-----");
}

void configureNodeKind(NodeKind nodeKind) {
  nodeIdentity.nodeKind = nodeKind;
  renderer.setNodeKind(nodeKind);
  refreshProgramSet();
  selectProgram(0, false);
}

void switchNodeKind(NodeKind nodeKind, bool persist) {
  if (persist && !saveNodeKind(nodeKind)) {
    Serial.println("config=save_failed");
  }

  configureNodeKind(nodeKind);
  announceNodeKind();
  printPrograms();
  selectProgram(0);
  sendNodeStatus("node_kind");
}

void runBrainConnectSequence() {
  brainConnected = true;
  burst.active = false;
  burst.remaining = 0;
  renderer.allOff();

  for (uint8_t i = 0; i < BRAIN_CONNECT_FLASH_COUNT; ++i) {
    renderer.flash100(BRAIN_CONNECT_FLASH_DURATION_MS);

    if (i + 1 < BRAIN_CONNECT_FLASH_COUNT) {
      delay(BRAIN_CONNECT_FLASH_INTERVAL_MS - BRAIN_CONNECT_FLASH_DURATION_MS);
    }
  }
}

void stageIncomingCommand(const NodeCommandMessage& message) {
  portENTER_CRITICAL(&radioMux);
  pendingCommandMessage = message;
  hasPendingCommand = true;
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

  if (header.type == decaflash::protocol::MessageType::NodeCommand &&
      len == static_cast<int>(sizeof(NodeCommandMessage))) {
    NodeCommandMessage message = {};
    memcpy(&message, data, sizeof(message));
    if (!isValidHeader(message.header, decaflash::protocol::MessageType::NodeCommand)) {
      return;
    }
    stageIncomingCommand(message);
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

  if (!brainConnected) {
    runBrainConnectSequence();
    applyCommand(REMOTE_IDLE_COMMAND);
    remoteControlActive = true;
    awaitingRemoteClock = true;
    Serial.println();
    Serial.println("-----");
    Serial.println("REMOTE: waiting for brain");
    Serial.println("-----");
  }
}

void processPendingCommandMessage(const NodeCommandMessage& message) {
  if (message.targetNodeKind != nodeIdentity.nodeKind) {
    return;
  }

  if (!brainConnected) {
    runBrainConnectSequence();
  }

  if (remoteControlActive && message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  applyCommand(message.command);
  remoteControlActive = true;
  awaitingRemoteClock = true;
  lastRemoteCommandRevision = message.commandRevision;

  Serial.println();
  Serial.println("-----");
  Serial.printf("REMOTE: %s | %s | %u BPM\n",
                activeCommand.name,
                nodeKindName(nodeIdentity.nodeKind),
                static_cast<unsigned>(currentBpmValue()));
  Serial.println("-----");
  sendNodeStatus("remote_command");
}

void applyClockSync(const ClockSyncMessage& message) {
  if (message.clockRevision == lastClockRevision &&
      message.beatSerial == lastClockBeatSerial) {
    return;
  }

  if (!brainConnected) {
    runBrainConnectSequence();
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
    clockHoldoverAnnounced = false;
    return;
  }

  if (wasSameBeatRenderedRecently(message.beatInBar, message.currentBar, now)) {
    advanceBeatPosition(message.beatInBar, message.currentBar, beatInBar, currentBar);
    nextBeatAtMs = lastBeatRenderedAtMs + beatIntervalMs;
    clockLocked = true;
    clockHoldoverAnnounced = false;
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
      clockHoldoverAnnounced = false;
      return;
    }
  }

  beatInBar = message.beatInBar;
  currentBar = message.currentBar;
  clockLocked = true;
  clockHoldoverAnnounced = false;
  onBeat();
  nextBeatAtMs = now + beatIntervalMs;
}

void processPendingRadio() {
  bool hadCommand = false;
  bool hadClockSync = false;
  bool hadBrainHello = false;
  NodeCommandMessage commandMessage = {};
  ClockSyncMessage clockMessage = {};
  BrainHelloMessage brainHelloMessage = {};

  portENTER_CRITICAL(&radioMux);
  if (hasPendingCommand) {
    commandMessage = pendingCommandMessage;
    hasPendingCommand = false;
    hadCommand = true;
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

  if (hadCommand) {
    processPendingCommandMessage(commandMessage);
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

void outputOffFromButton() {
  Serial.println("button=long_press");
  renderer.allOff();
  effectStartedAtMs = millis();
  nextBeatAtMs = effectStartedAtMs + beatIntervalMs;
  burst.active = false;
  burst.remaining = 0;
  Serial.println("output=off");
}

void serviceButton() {
  if (brainConnected || remoteControlActive) {
    return;
  }

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
      outputOffFromButton();
    }
    return;
  }

  if (!isPressed && buttonPressed) {
    buttonPressed = false;

    if (!longPressHandled) {
      Serial.println("button=short_press");
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

void startBurst(const NodeCommand& command) {
  const uint8_t count = command.burstCount;
  const uint16_t intervalMs = command.burstIntervalMs;

  if (count == 0) {
    return;
  }

  burst.active = true;
  burst.remaining = count;
  burst.intervalMs = intervalMs;
  burst.nextFlashAtMs = millis();
  burst.intervalStepMs = command.burstIntervalStepMs;
  burst.flashDurationMs = command.flashDurationMs;
}

void printBurstLine(uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (i > 0) {
      Serial.print(" ");
    }

    Serial.print("PULSE");
  }

  Serial.println();
}

void printBeatSummary(const NodeCommand& command, bool isTriggerBeat) {
  if (lastPrintedBeatInBar == beatInBar && lastPrintedBar == currentBar) {
    return;
  }

  lastPrintedBeatInBar = beatInBar;
  lastPrintedBar = currentBar;

  if (!isTriggerBeat) {
    Serial.println("-");
  } else if (command.effect == EffectType::BarBurst) {
    printBurstLine(command.burstCount);
  } else {
    Serial.println("PULSE");
  }

  uint8_t nextBeat = beatInBar + 1;
  if (nextBeat > beatsPerBar) {
    Serial.println();
  }
}

void serviceBurst() {
  const uint32_t now = millis();

  if (!burst.active) {
    return;
  }

  if ((int32_t)(now - burst.nextFlashAtMs) < 0) {
    return;
  }

  renderer.flash100(burst.flashDurationMs);

  if (burst.remaining > 0) {
    burst.remaining--;
  }

  if (burst.remaining == 0) {
    burst.active = false;
    return;
  }

  burst.intervalMs = clampBurstInterval(
    static_cast<int32_t>(burst.intervalMs) + burst.intervalStepMs
  );
  burst.nextFlashAtMs = millis() + burst.intervalMs;
}

void onBeat() {
  const auto& command = activeCommand;
  lastBeatRenderedAtMs = millis();
  lastRenderedBeatInBar = beatInBar;
  lastRenderedBar = currentBar;
  const bool isTriggerBar =
    (command.triggerEveryBars <= 1) || ((currentBar % command.triggerEveryBars) == 0);
  const bool isTriggerBeat =
    ((command.triggerBeat == 0) || (beatInBar == command.triggerBeat)) && isTriggerBar;
  printBeatSummary(command, isTriggerBeat);

  switch (command.effect) {
    case EffectType::BeatPulse:
      if (isTriggerBeat) {
        renderer.flash100(command.flashDurationMs);
      }
      break;

    case EffectType::BarBurst:
      if (isTriggerBeat) {
        startBurst(command);
      }
      break;

    case EffectType::Off:
    default:
      renderer.allOff();
      break;
  }

  globalBeat++;
  beatInBar++;

  if (beatInBar > beatsPerBar) {
    beatInBar = 1;
    currentBar++;
  }
}

void serviceClock() {
  const uint32_t now = millis();

  if (clockLocked && lastClockSyncAtMs != 0 &&
      (now - lastClockSyncAtMs) >= CLOCK_SYNC_TIMEOUT_MS) {
    clockLocked = false;
    clockHoldoverAnnounced = true;
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

void normalizeSerialLine(char* line) {
  size_t length = strlen(line);
  while (length > 0 &&
         isspace(static_cast<unsigned char>(line[length - 1]))) {
    line[--length] = '\0';
  }

  size_t start = 0;
  while (line[start] != '\0' &&
         isspace(static_cast<unsigned char>(line[start]))) {
    start++;
  }

  if (start > 0) {
    memmove(line, line + start, strlen(line + start) + 1);
  }

  for (size_t i = 0; line[i] != '\0'; ++i) {
    line[i] = static_cast<char>(tolower(static_cast<unsigned char>(line[i])));
  }
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

  nodeIdentity.nodeKind = loadStoredNodeKind();
  configureNodeKind(nodeIdentity.nodeKind);

  Serial.println();
  Serial.println("Decaflash Node V1");
  Serial.printf(
    "device_type=%u node_kind=%u\n",
    static_cast<unsigned>(nodeIdentity.deviceType),
    static_cast<unsigned>(nodeIdentity.nodeKind)
  );
  Serial.printf("node_mode=%s\n", nodeKindName(nodeIdentity.nodeKind));
  Serial.printf("runtime=%s\n", runtimeLabelFor(nodeIdentity.nodeKind));
  Serial.println("controls=atom button + serial");
  Serial.printf("renderer=%s\n", renderer.rendererName());
  if (nodeIdentity.nodeKind == NodeKind::RgbStrip) {
    Serial.println("rgb_driver=sk6812 pins=26+32 startup_probe=rgb");
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
  Serial.println("mode=local demo + remote command receive");
  Serial.println("node_stack=shared runtime-selected renderer");
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
  serviceBurst();
  serviceNodeStatus();
}
