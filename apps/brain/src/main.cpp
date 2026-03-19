#include <Arduino.h>
#include <M5Atom.h>

#include "command_examples.h"
#include "decaflash_types.h"
#include "espnow_transport.h"
#include "protocol.h"

using decaflash::DeviceType;
using decaflash::examples::kFlashCommandCount;
using decaflash::examples::kFlashCommands;
using decaflash::examples::kFlashQuadSkip;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::protocol::makeClockSyncMessage;
using decaflash::protocol::makeNodeCommandMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t COMMAND_REFRESH_MS = 10000;
static constexpr uint16_t BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;
static constexpr uint16_t MATRIX_FLASH_MS = 50;
static constexpr uint32_t UI_MODE_DISPLAY_MS = 3000;
uint32_t nextSendAtMs = 0;
bool espNowReady = false;
uint32_t commandRevision = 1;
uint32_t clockRevision = 1;
uint32_t beatSerial = 0;
uint32_t beatIntervalMs = 0;
uint32_t nextBeatAtMs = 0;
uint8_t beatInBar = 1;
uint32_t currentBar = 1;
uint32_t matrixOffAtMs = 0;
uint32_t uiFeedbackUntilMs = 0;
size_t currentModeIndex = 0;

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

void drawBeatFrame(uint8_t beat) {
  const uint32_t fillColor = (beat == 1)
    ? color(180, 180, 180)
    : color(0, 70, 160);

  for (uint8_t i = 0; i < 25; ++i) {
    M5.dis.drawpix(i, fillColor);
  }

  matrixOffAtMs = millis() + MATRIX_FLASH_MS;
}

void onBeat() {
  const uint8_t currentBeat = beatInBar;

  if (millis() >= uiFeedbackUntilMs) {
    drawBeatFrame(currentBeat);
  }
  Serial.printf("beat=%u/%u\n", currentBeat, BEATS_PER_BAR);

  if (espNowReady) {
    const auto sync = makeClockSyncMessage(
      clockRevision,
      ++beatSerial,
      BPM,
      BEATS_PER_BAR,
      currentBeat,
      currentBar
    );

    const auto result = esp_now_send(
      decaflash::espnow_transport::kBroadcastMac,
      reinterpret_cast<const uint8_t*>(&sync),
      sizeof(sync)
    );

    Serial.printf("send=clock_sync result=%d beat=%u bar=%lu\n",
                  result,
                  currentBeat,
                  static_cast<unsigned long>(currentBar));
  }

  beatInBar++;
  if (beatInBar > BEATS_PER_BAR) {
    beatInBar = 1;
    currentBar++;
    Serial.println();
  }
}

void sendCurrentCommand() {
  if (!espNowReady) {
    return;
  }

  const auto message = makeNodeCommandMessage(
    decaflash::NodeKind::Flashlight,
    kFlashCommands[currentModeIndex],
    commandRevision
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  Serial.printf("send=node_command result=%d mode=%u command=%s\n",
                result,
                static_cast<unsigned>(currentModeIndex + 1),
                message.command.name);
}

void showModeUi() {
  drawModeNumber(currentModeIndex);
  uiFeedbackUntilMs = millis() + UI_MODE_DISPLAY_MS;
  matrixOffAtMs = 0;
  Serial.printf("mode=%u\n", static_cast<unsigned>(currentModeIndex + 1));
}

void selectNextMode() {
  currentModeIndex = (currentModeIndex + 1) % kFlashCommandCount;
  commandRevision++;
  showModeUi();
  sendCurrentCommand();
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
  Serial.printf("mode=%s\n", espNowReady ? "broadcast command sender" : "startup only");
  Serial.println("button=short press cycles modes 1-5");

  beatIntervalMs = bpmToIntervalMs(BPM);
  nextBeatAtMs = millis() + beatIntervalMs;
  showModeUi();

  if (espNowReady) {
    sendCurrentCommand();
  }

  nextSendAtMs = millis() + COMMAND_REFRESH_MS;
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  if (M5.Btn.wasPressed()) {
    selectNextMode();
  }

  if (uiFeedbackUntilMs != 0 && (int32_t)(now - uiFeedbackUntilMs) >= 0) {
    uiFeedbackUntilMs = 0;
    clearMatrix();
  }

  if (matrixOffAtMs != 0 && (int32_t)(now - matrixOffAtMs) >= 0) {
    clearMatrix();
    matrixOffAtMs = 0;
  }

  while ((int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }

  if ((int32_t)(now - nextSendAtMs) >= 0) {
    sendCurrentCommand();
    nextSendAtMs += COMMAND_REFRESH_MS;
  }
}
