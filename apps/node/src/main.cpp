#include <Arduino.h>

#include "decaflash_types.h"

using decaflash::DefaultPreset;
using decaflash::DeviceType;
using decaflash::EffectType;
using decaflash::NodeKind;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Node;
static constexpr NodeKind NODE_KIND = NodeKind::Flashlight;

static constexpr int FLASH_PIN = 26;
static constexpr int BUTTON_PIN = 39;
static constexpr uint8_t PRESET_SHORT_100 = 1;
static constexpr uint16_t FLASH_MS = 240;
static constexpr uint32_t BUTTON_DEBOUNCE_MS = 30;
static constexpr uint32_t BUTTON_LONG_PRESS_MS = 900;
static constexpr uint32_t STARTUP_DELAY_MS = 500;
static constexpr uint16_t FAST_FLASH_INTERVAL_MS = 333;

static constexpr DefaultPreset PROGRAMS[] = {
  { "Flash 3Hz", EffectType::Strobe, 255, FAST_FLASH_INTERVAL_MS },
  { "Pulse Slow", EffectType::PulseSlow, 255, 900 },
  { "Strobe", EffectType::Strobe, 255, 120 },
};

static constexpr size_t PROGRAM_COUNT = sizeof(PROGRAMS) / sizeof(PROGRAMS[0]);

size_t currentProgram = 0;

bool buttonPressed = false;
bool longPressHandled = false;
uint32_t buttonChangedAtMs = 0;
uint32_t buttonPressedAtMs = 0;
bool lastButtonLevel = HIGH;

uint32_t effectStartedAtMs = 0;
uint32_t nextPulseAtMs = 0;
bool flashlightEnabled = false;

void hardOff() {
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW);
  delay(8);
}

void sendFlashPreset(uint8_t preset) {
  hardOff();

  for (uint8_t i = 0; i < preset; ++i) {
    digitalWrite(FLASH_PIN, LOW);
    delayMicroseconds(4);
    digitalWrite(FLASH_PIN, HIGH);
    delayMicroseconds(4);
  }
}

void setFlashlightEnabled(bool enabled) {
  if (enabled == flashlightEnabled) {
    return;
  }

  if (enabled) {
    sendFlashPreset(PRESET_SHORT_100);
  } else {
    hardOff();
  }

  flashlightEnabled = enabled;
}

void fireFlashPulse() {
  sendFlashPreset(PRESET_SHORT_100);
  delay(FLASH_MS);
  hardOff();
  flashlightEnabled = false;
}

void printPrograms() {
  Serial.println("programs:");

  for (size_t i = 0; i < PROGRAM_COUNT; ++i) {
    Serial.printf("  %u -> %s\n", static_cast<unsigned>(i + 1), PROGRAMS[i].name);
  }
}

void printCurrentProgram() {
  const auto& program = PROGRAMS[currentProgram];
  Serial.printf(
    "program=%u \"%s\"\n",
    static_cast<unsigned>(currentProgram + 1),
    program.name
  );
}

void printHelp() {
  Serial.println();
  Serial.println("Button:");
  Serial.println("  short press -> next program");
  Serial.println("  long press  -> turn output off");
  Serial.println();
}

void selectProgram(size_t programIndex) {
  currentProgram = programIndex % PROGRAM_COUNT;
  effectStartedAtMs = millis();
  nextPulseAtMs = effectStartedAtMs;
  flashlightEnabled = false;
  hardOff();
  printCurrentProgram();
}

void selectNextProgram() {
  selectProgram((currentProgram + 1) % PROGRAM_COUNT);
}

void outputOffFromButton() {
  Serial.println("button=long_press");
  hardOff();
  flashlightEnabled = false;
  effectStartedAtMs = millis();
  nextPulseAtMs = effectStartedAtMs;
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

void serviceProgram() {
  const auto& program = PROGRAMS[currentProgram];
  const uint32_t now = millis();

  switch (program.effect) {
    case EffectType::On:
      setFlashlightEnabled(true);
      break;

    case EffectType::PulseSlow:
      if ((int32_t)(now - nextPulseAtMs) >= 0) {
        fireFlashPulse();
        nextPulseAtMs = millis() + program.intervalMs;
      }
      break;

    case EffectType::Strobe:
      if ((int32_t)(now - nextPulseAtMs) >= 0) {
        fireFlashPulse();
        nextPulseAtMs = millis() + program.intervalMs;
      }
      break;

    case EffectType::Off:
    default:
      setFlashlightEnabled(false);
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(STARTUP_DELAY_MS);

  pinMode(BUTTON_PIN, INPUT);
  hardOff();

  Serial.println();
  Serial.println("Decaflash Node V1");
  Serial.printf(
    "device_type=%u node_kind=%u\n",
    static_cast<unsigned>(DEVICE_TYPE),
    static_cast<unsigned>(NODE_KIND)
  );
  Serial.println("runtime=standalone flashlight demo");
  Serial.println("controls=atom button");
  Serial.println("future=saved default + esp-now");
  printPrograms();
  printHelp();

  selectProgram(0);
}

void loop() {
  serviceButton();
  serviceProgram();
}
