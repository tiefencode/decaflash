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
using decaflash::protocol::makeBrainHelloMessage;
using decaflash::protocol::makeClockSyncMessage;
using decaflash::protocol::makeNodeCommandMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t COMMAND_REFRESH_MS = 10000;
static constexpr uint16_t DEFAULT_BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;
static constexpr uint16_t MATRIX_FLASH_MS = 50;
static constexpr uint32_t UI_MODE_DISPLAY_MS = 3000;
static constexpr uint32_t BUTTON_TAP_WINDOW_MS = 450;
static constexpr uint32_t TAP_TEMPO_TIMEOUT_MS = 1600;
static constexpr uint16_t TAP_TEMPO_MIN_BPM = 60;
static constexpr uint16_t TAP_TEMPO_MAX_BPM = 180;
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
size_t currentModeIndex = 0;
bool pendingSingleTap = false;
uint32_t pendingSingleTapAtMs = 0;
uint32_t lastTapAtMs = 0;
uint32_t tapIntervalsMs[3] = {0, 0, 0};
uint8_t tapIntervalCount = 0;

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

void drawBeatFrame(uint8_t beat) {
  const uint32_t fillColor = (beat == 1)
    ? color(180, 180, 180)
    : color(0, 70, 160);

  for (uint8_t i = 0; i < 25; ++i) {
    M5.dis.drawpix(i, fillColor);
  }

  matrixOffAtMs = millis() + MATRIX_FLASH_MS;
}

void drawTapTempoFlash() {
  for (uint8_t i = 0; i < 25; ++i) {
    M5.dis.drawpix(i, color(255, 90, 0));
  }

  matrixOffAtMs = millis() + MATRIX_FLASH_MS;
}

void onBeat() {
  const uint8_t currentBeat = beatInBar;

  if (!brainLive) {
    return;
  }

  if (millis() >= uiFeedbackUntilMs && millis() >= tapTempoUiUntilMs) {
    drawBeatFrame(currentBeat);
  }

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

void sendCommand(const decaflash::NodeCommand& command) {
  if (!espNowReady || !brainLive) {
    return;
  }

  const auto message = makeNodeCommandMessage(
    decaflash::NodeKind::Flashlight,
    command,
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

void sendCurrentCommand() {
  sendCommand(kFlashCommands[currentModeIndex]);
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

  currentBpm = clampBpm(static_cast<uint16_t>(60000UL / averageIntervalMs));
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  clockRevision++;

  Serial.printf("tap_tempo=bpm:%u\n", static_cast<unsigned>(currentBpm));
}

void selectNextMode() {
  currentModeIndex = (currentModeIndex + 1) % kFlashCommandCount;
  commandRevision++;
  showModeUi();
  sendCurrentCommand();
}

void activateBrain() {
  brainLive = true;
  beatSerial = 0;
  beatInBar = 1;
  currentBar = 1;
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  commandRevision++;
  showModeUi();
  sendCurrentCommand();
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

  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  clearMatrix();

  if (espNowReady) {
    sendBrainHello();
  }
}

void loop() {
  M5.update();
  const uint32_t now = millis();

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

  while (brainLive && (int32_t)(now - nextBeatAtMs) >= 0) {
    onBeat();
    nextBeatAtMs += beatIntervalMs;
  }

  if (brainLive && (int32_t)(now - nextSendAtMs) >= 0) {
    sendCurrentCommand();
    nextSendAtMs += COMMAND_REFRESH_MS;
  }
}
