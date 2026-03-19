#include <Arduino.h>

#include "decaflash_types.h"
#include "espnow_transport.h"
#include "flashlight_renderer.h"
#include "node_programs.h"
#include "protocol.h"

using decaflash::EffectType;
using decaflash::NodeIdentity;
using decaflash::NodeCommand;
using decaflash::espnow_transport::initEspNow;
using decaflash::espnow_transport::isValidHeader;
using decaflash::node::kProgramCount;
using decaflash::node::kPrograms;
using decaflash::protocol::ClockSyncMessage;
using decaflash::protocol::NodeCommandMessage;

static constexpr NodeIdentity NODE_IDENTITY = {
  decaflash::DeviceType::Node,
  decaflash::NodeKind::Flashlight,
};

static constexpr int BUTTON_PIN = 39;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr uint32_t CLOCK_SYNC_TIMEOUT_MS = 4000;
static constexpr uint32_t SOFT_SYNC_MATCH_WINDOW_MS = 120;
static constexpr uint32_t DUPLICATE_BEAT_GUARD_MS = 250;
static constexpr int32_t SOFT_SYNC_MAX_CORRECTION_MS = 30;
static constexpr int32_t HARD_SYNC_ERROR_MS = 180;
static constexpr int FLASH_PIN = 26;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t DEFAULT_BEATS_PER_BAR = 4;

size_t currentProgram = 0;
NodeCommand activeCommand = kPrograms[0];

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
FlashlightRenderer renderer(FLASH_PIN);

struct BurstState {
  bool active = false;
  uint8_t remaining = 0;
  uint32_t intervalMs = 0;
  uint32_t nextFlashAtMs = 0;
  int16_t intervalStepMs = 0;
  uint16_t flashDurationMs = 120;
};

BurstState burst;
bool remoteControlActive = false;
bool awaitingRemoteClock = false;
uint32_t lastRemoteCommandRevision = 0;
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
NodeCommandMessage pendingCommandMessage = {};
ClockSyncMessage pendingClockSyncMessage = {};

void onBeat();

uint32_t bpmToIntervalMs(uint16_t bpm) {
  return 60000UL / bpm;
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
  effectStartedAtMs = millis();
  burst.active = false;
  burst.remaining = 0;
  renderer.allOff();
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
  }
}

void processPendingCommandMessage(const NodeCommandMessage& message) {
  if (message.targetNodeKind != NODE_IDENTITY.nodeKind) {
    return;
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
  Serial.printf("REMOTE: %s | %u BPM\n",
                activeCommand.name,
                beatIntervalMs == 0 ? BPM : static_cast<unsigned>(60000UL / beatIntervalMs));
  Serial.println("-----");
}

void applyClockSync(const ClockSyncMessage& message) {
  if (message.beatSerial == lastClockBeatSerial) {
    return;
  }

  const uint32_t now = millis();
  lastClockBeatSerial = message.beatSerial;
  lastClockSyncAtMs = now;
  beatIntervalMs = bpmToIntervalMs(message.bpm);
  beatsPerBar = message.beatsPerBar;
  awaitingRemoteClock = false;
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
  NodeCommandMessage commandMessage = {};
  ClockSyncMessage clockMessage = {};

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
  portEXIT_CRITICAL(&radioMux);

  if (hadCommand) {
    processPendingCommandMessage(commandMessage);
  }

  if (hadClockSync) {
    applyClockSync(clockMessage);
  }
}

void printPrograms() {
  Serial.println("programs:");

  for (size_t i = 0; i < kProgramCount; ++i) {
    Serial.printf("  %u -> %s\n", static_cast<unsigned>(i + 1), kPrograms[i].name);
  }
}

void printCurrentProgram() {
  Serial.println();
  Serial.println("-----");
  Serial.printf("PROGRAM %u: %s | %u BPM\n",
                static_cast<unsigned>(currentProgram + 1),
                activeCommand.name,
                beatIntervalMs == 0 ? BPM : static_cast<unsigned>(60000UL / beatIntervalMs));
  Serial.println("-----");
}

void printHelp() {
  Serial.println();
  Serial.println("Button:");
  Serial.println("  short press -> next program");
  Serial.println("  long press  -> turn output off");
  Serial.println();
}

void selectProgram(size_t programIndex) {
  currentProgram = programIndex % kProgramCount;
  applyCommand(kPrograms[currentProgram]);
  remoteControlActive = false;
  awaitingRemoteClock = false;
  printCurrentProgram();
}

void selectNextProgram() {
  selectProgram((currentProgram + 1) % kProgramCount);
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

    Serial.print("FLASH");
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
    Serial.println("FLASH");
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
  }

  if (remoteControlActive && awaitingRemoteClock) {
    return;
  }

  while ((int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }
}

void setup() {
  Serial.begin(115200);
  delay(STARTUP_DELAY_MS);

  pinMode(BUTTON_PIN, INPUT);
  renderer.begin();

  Serial.println();
  Serial.println("Decaflash Node V1");
  Serial.printf(
    "device_type=%u node_kind=%u\n",
    static_cast<unsigned>(NODE_IDENTITY.deviceType),
    static_cast<unsigned>(NODE_IDENTITY.nodeKind)
  );
  beatIntervalMs = bpmToIntervalMs(BPM);
  beatsPerBar = DEFAULT_BEATS_PER_BAR;
  Serial.println("runtime=standalone flashlight demo");
  Serial.println("controls=atom button");
  Serial.println("renderer=flashlight");
  Serial.printf("clock=%u bpm %u/4\n", BPM, beatsPerBar);
  nextBeatAtMs = millis() + beatIntervalMs;
  const auto initResult = initEspNow();
  const bool espNowOk = initResult.ok();
  Serial.printf("wifi_set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("wifi_start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("wifi_set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("esp_now_init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("esp_now=%s\n", espNowOk ? "ok" : "failed");
  if (espNowOk) {
    esp_now_register_recv_cb(onEspNowReceive);
  }
  Serial.println("mode=local demo + remote command receive");
  Serial.println("future=saved default + rgb renderer");
  printPrograms();
  printHelp();

  selectProgram(0);
}

void loop() {
  processPendingRadio();
  serviceButton();
  serviceClock();
  serviceBurst();
}
