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
using decaflash::protocol::NodeCommandMessage;

static constexpr NodeIdentity NODE_IDENTITY = {
  decaflash::DeviceType::Node,
  decaflash::NodeKind::Flashlight,
};

static constexpr int BUTTON_PIN = 39;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr int FLASH_PIN = 26;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;

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
uint32_t lastRemoteCommandRevision = 0;

uint32_t bpmToIntervalMs(uint16_t bpm) {
  return 60000UL / bpm;
}

void applyCommand(const NodeCommand& command) {
  activeCommand = command;
  effectStartedAtMs = millis();
  nextBeatAtMs = effectStartedAtMs + beatIntervalMs;
  beatInBar = 1;
  globalBeat = 0;
  currentBar = 1;
  burst.active = false;
  burst.remaining = 0;
  renderer.allOff();
}

void onEspNowReceive(const uint8_t* mac, const uint8_t* data, int len) {
  (void)mac;

  if (len != static_cast<int>(sizeof(NodeCommandMessage))) {
    return;
  }

  NodeCommandMessage message = {};
  memcpy(&message, data, sizeof(message));

  if (!isValidHeader(message.header, decaflash::protocol::MessageType::NodeCommand)) {
    return;
  }

  if (message.targetNodeKind != NODE_IDENTITY.nodeKind) {
    return;
  }

  if (remoteControlActive && message.commandRevision == lastRemoteCommandRevision) {
    return;
  }

  activeCommand = message.command;
  beatIntervalMs = bpmToIntervalMs(message.bpm);
  applyCommand(message.command);
  remoteControlActive = true;
  lastRemoteCommandRevision = message.commandRevision;

  Serial.println();
  Serial.println("-----");
  Serial.printf("REMOTE: %s | %u BPM\n", activeCommand.name, message.bpm);
  Serial.println("-----");
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
                BPM);
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
  const bool isTriggerBar =
    (command.triggerEveryBars <= 1) || ((currentBar % command.triggerEveryBars) == 0);
  const bool isTriggerBeat =
    ((command.triggerBeat == 0) || (beatInBar == command.triggerBeat)) && isTriggerBar;
  bool flashedOnBeat = false;

  switch (command.effect) {
    case EffectType::BeatPulse:
      if (isTriggerBeat) {
        Serial.println("FLASH");
        renderer.flash100(command.flashDurationMs);
        flashedOnBeat = true;
      }
      break;

    case EffectType::BarBurst:
      if (isTriggerBeat) {
        printBurstLine(command.burstCount);
        startBurst(command);
        flashedOnBeat = true;
      }
      break;

    case EffectType::Off:
    default:
      renderer.allOff();
      break;
  }

  if (!flashedOnBeat) {
    Serial.println("-");
  }

  globalBeat++;
  beatInBar++;

  if (beatInBar > BEATS_PER_BAR) {
    beatInBar = 1;
    currentBar++;
    Serial.println();
  }
}

void serviceClock() {
  const uint32_t now = millis();

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
  Serial.println("runtime=standalone flashlight demo");
  Serial.println("controls=atom button");
  Serial.println("renderer=flashlight");
  Serial.printf("clock=%u bpm 4/4\n", BPM);
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
  serviceButton();
  serviceClock();
  serviceBurst();
}
