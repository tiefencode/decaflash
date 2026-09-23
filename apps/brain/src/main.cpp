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
#include "node_text_channel.h"
#include "text_playback.h"
#include "wifi_manager.h"

using decaflash::DeviceType;
using decaflash::scenes::kSceneCount;
using decaflash::scenes::sceneName;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::protocol::makeBrainHelloMessage;
using decaflash::protocol::makeClockSyncMessage;
using decaflash::protocol::makeNodeTextMessage;
using decaflash::protocol::makeSceneSelectMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t SCENE_SELECT_REFRESH_MS = 30000;
static constexpr uint16_t DEFAULT_BPM = 120;
static constexpr uint8_t BEATS_PER_BAR = 4;
static constexpr uint16_t BEAT_DOT_FLASH_MS = 140;
static constexpr uint32_t METER_REFRESH_MS = 40;
static constexpr uint32_t UI_SCENE_DISPLAY_MS = 3000;
static constexpr uint32_t ESPNOW_RECOVERY_INTERVAL_MS = 1000;
static constexpr uint16_t MIN_BPM = 60;
static constexpr uint16_t MAX_BPM = 180;
static constexpr size_t kSceneSlots = kSceneCount;
bool espNowReady = false;
bool brainLive = false;
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
bool pendingClockSync = false;
decaflash::brain::PdmMicrophone microphone;
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
bool nodeTextChannelActive = false;
decaflash::brain::text_playback::Owner nodeTextChannelOwner =
  decaflash::brain::text_playback::Owner::Manual;

uint32_t nextSceneSelectAtMs = 0;

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

void requestEspNowRecovery(const char* reason) {
  espNowReady = false;
  espNowRecoveryRequested = true;
  espNowRecoveryReason = reason;
}

namespace {

bool prepareNodeText(const char* rawText, char* buffer, size_t bufferLength) {
  if (rawText == nullptr || buffer == nullptr || bufferLength == 0) {
    return false;
  }

  while (*rawText == ' ') {
    rawText++;
  }

  size_t length = 0;
  while (rawText[length] != '\0' &&
         rawText[length] != '\r' &&
         rawText[length] != '\n' &&
         (length + 1U) < bufferLength) {
    buffer[length] = rawText[length];
    length++;
  }

  buffer[length] = '\0';
  return length > 0;
}

bool sendNodeText(decaflash::NodeKind targetNodeKind, const char* text, uint8_t flags) {
  const auto message = makeNodeTextMessage(targetNodeKind, text, flags);
  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result == ESP_OK) {
    return true;
  }

  if (result == ESP_ERR_ESPNOW_NOT_INIT) {
    requestEspNowRecovery("node_text_not_init");
  }

  Serial.printf("SEND: node_text result=%d kind=%s flags=%u\n",
                result,
                nodeKindName(targetNodeKind),
                static_cast<unsigned>(flags));
  return false;
}

}  // namespace

namespace decaflash::brain::node_text {

bool start(const char* text, text_playback::Owner owner) {
  if (!brainLive || !espNowReady || decaflash::brain::api_client::radioPauseActive()) {
    return false;
  }

  char buffer[decaflash::protocol::kNodeTextLength] = {};
  if (!prepareNodeText(text, buffer, sizeof(buffer))) {
    return false;
  }

  const bool sentFlash = sendNodeText(decaflash::NodeKind::Flashlight, buffer, 0);
  const bool sentRgb = sendNodeText(decaflash::NodeKind::RgbStrip, buffer, 0);
  nodeTextChannelActive = sentFlash || sentRgb;
  nodeTextChannelOwner = owner;
  return nodeTextChannelActive;
}

void stop() {
  nodeTextChannelActive = false;

  if (!brainLive || !espNowReady || decaflash::brain::api_client::radioPauseActive()) {
    return;
  }

  (void)sendNodeText(decaflash::NodeKind::Flashlight,
                     "",
                     decaflash::protocol::kNodeTextFlagCancel);
  (void)sendNodeText(decaflash::NodeKind::RgbStrip,
                     "",
                     decaflash::protocol::kNodeTextFlagCancel);
}

bool stopAiOwned() {
  if (!nodeTextChannelActive ||
      nodeTextChannelOwner != text_playback::Owner::Ai) {
    return false;
  }

  stop();
  return true;
}

}  // namespace decaflash::brain::node_text

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

  espNowRecoveryRequested = false;
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

  // Cloud uploads own the shared Wi-Fi radio. Keep ESP-NOW completely idle
  // until the managed Wi-Fi session disconnects and requests one recovery.
  if (radioPauseActive) {
    return;
  }

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

    if (espNowBlockedByChannel) {
      Serial.printf("ESP-NOW: channel_ok channel=%u\n",
                    static_cast<unsigned>(
                      wifiConnected ? wifiChannel : decaflash::espnow_transport::kWifiChannel));
    }

    espNowBlockedByChannel = false;
    requestEspNowRecovery(wifiConnected ? "wifi_state_changed" : "wifi_disconnected");
  }

  if (!espNowBlockedByChannel &&
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

void sendSceneSelect() {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady || !brainLive) {
    return;
  }

  const auto message = makeSceneSelectMessage(static_cast<uint8_t>(currentSceneIndex));

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  if (result != ESP_OK) {
    if (result == ESP_ERR_ESPNOW_NOT_INIT) {
      requestEspNowRecovery("scene_select_not_init");
    }
    Serial.printf("SEND: scene_select result=%d scene=%u name=%s\n",
                  result,
                  static_cast<unsigned>(currentSceneIndex + 1),
                  sceneName(currentSceneIndex));
  }
}

void sendCurrentCommands() {
  nextSceneSelectAtMs = millis();
}

void serviceSceneSelect(uint32_t now) {
  if (decaflash::brain::api_client::radioPauseActive() ||
      !espNowReady ||
      !brainLive ||
      static_cast<int32_t>(now - nextSceneSelectAtMs) < 0) {
    return;
  }

  sendSceneSelect();
  nextSceneSelectAtMs = millis() + SCENE_SELECT_REFRESH_MS;
}

void sendBrainHello() {
  if (decaflash::brain::api_client::radioPauseActive() || !espNowReady) {
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
  showSceneUi();
  sendCurrentCommands();
  requestClockSync();
}

void activateBrain() {
  brainLive = true;
  resetAudioClockFollow();
  beatInBar = 1;
  currentBar = 1;
  beatIntervalMs = bpmToIntervalMs(currentBpm);
  nextBeatAtMs = millis() + beatIntervalMs;
  showSceneUi();
  sendCurrentCommands();
  requestClockSync();
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
  Serial.printf("PROTOCOL: dcfl/v%u\n", decaflash::protocol::kProtocolVersion);
  microphone.begin();
  decaflash::brain::api_client::begin();

  const auto initResult = initEspNow();
  const auto peerResult = initResult.ok() ? ensureBroadcastPeer() : decltype(ensureBroadcastPeer()){};
  espNowReady = initResult.ok() && peerResult.ok();
  Serial.printf("WIFI: set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("WIFI: start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("WIFI: set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("ESP-NOW: init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("ESP-NOW: state=%s\n", espNowReady ? "ok" : "failed");
  Serial.printf("ESP-NOW: peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("ESP-NOW: add_peer=%d\n", static_cast<int>(peerResult.addPeer));
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
  serviceSceneSelect(now);

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

  updateIdleMatrixUi(now);
  updateBeatDotOverlay(now);
  updateStatusPixelOverlay(now);
}
