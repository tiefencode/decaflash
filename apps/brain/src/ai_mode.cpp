#include "ai_mode.h"

#include <Arduino.h>

#include "api_client.h"
#include "matrix_ui.h"
#include "node_text_channel.h"
#include "pdm_microphone.h"
#include "text_playback.h"

namespace decaflash::brain::ai_mode {

namespace {

static constexpr uint32_t kTogglePressMs = 1200;
static constexpr uint32_t kToggleDisplayMs = 780;
static constexpr uint32_t kMusicStableMs = 1500;
static constexpr uint32_t kRecognitionFallbackMs = 1200;
static constexpr uint32_t kRecognitionRecentOnsetMs = 180;
static constexpr uint8_t kRecognitionMinBeatConfidence = 18;
static constexpr uint32_t kFailureCooldownMs = 60000;
static constexpr uint32_t kSuccessCooldownMs = 420000;
static constexpr uint32_t kRecordDurationMs = 12000;

enum class TransientIcon : uint8_t {
  None = 0,
  AiEnabled = 1,
  AiDisabled = 2,
};

bool aiModeEnabled = false;
bool aiRecordingOwned = false;
uint32_t aiReadyToListenAtMs = 0;
uint32_t aiCooldownUntilMs = 0;
uint32_t aiMusicPresentSinceAtMs = 0;
uint32_t aiRecognitionArmedAtMs = 0;
TransientIcon transientIcon = TransientIcon::None;
uint32_t transientIconColor = 0;
uint32_t transientIconUntilMs = 0;

void resetListeningWindow() {
  aiMusicPresentSinceAtMs = 0;
  aiRecognitionArmedAtMs = 0;
}

void showTransientIcon(TransientIcon icon, uint32_t color, uint32_t durationMs) {
  transientIcon = icon;
  transientIconColor = color;
  transientIconUntilMs = millis() + durationMs;
}

bool transientIconActive(uint32_t now) {
  return transientIcon != TransientIcon::None &&
         (int32_t)(now - transientIconUntilMs) < 0;
}

void clearTransientIcon() {
  transientIcon = TransientIcon::None;
  transientIconColor = 0;
  transientIconUntilMs = 0;
}

void drawTransientIcon(uint32_t now) {
  switch (transientIcon) {
    case TransientIcon::AiEnabled:
      decaflash::brain::matrix::drawAiWaveAnimation(
        now - (transientIconUntilMs - kToggleDisplayMs),
        kToggleDisplayMs,
        true,
        transientIconColor);
      break;

    case TransientIcon::AiDisabled:
      decaflash::brain::matrix::drawAiWaveAnimation(
        now - (transientIconUntilMs - kToggleDisplayMs),
        kToggleDisplayMs,
        false,
        transientIconColor);
      break;

    case TransientIcon::None:
    default:
      break;
  }
}

void setCooldown(uint32_t now, uint32_t durationMs, const char* reason) {
  aiCooldownUntilMs = now + durationMs;
  aiReadyToListenAtMs = 0;
  resetListeningWindow();
  Serial.printf("AI: cooldown reason=%s ms=%lu\n",
                reason,
                static_cast<unsigned long>(durationMs));
}

void disableForWifiFailure(uint32_t now, const char* reason) {
  (void)now;
  aiModeEnabled = false;
  decaflash::brain::api_client::cancelAiWork();
  decaflash::brain::node_text::stopAiOwned();
  decaflash::brain::text_playback::stopAiOwned(false);
  aiRecordingOwned = false;
  aiReadyToListenAtMs = 0;
  aiCooldownUntilMs = 0;
  resetListeningWindow();
  Serial.printf("AI: disabled reason=%s\n", reason);
}

}  // namespace

uint32_t togglePressMs() {
  return kTogglePressMs;
}

bool enabled() {
  return aiModeEnabled;
}

void toggle(uint32_t now, PdmMicrophone& microphone) {
  aiModeEnabled = !aiModeEnabled;
  aiReadyToListenAtMs = 0;
  aiCooldownUntilMs = 0;
  resetListeningWindow();

  if (aiModeEnabled) {
    Serial.println("AI: enabled");
    showTransientIcon(TransientIcon::AiEnabled, 0x00FF00, kToggleDisplayMs);
    aiReadyToListenAtMs = now + kToggleDisplayMs;
    return;
  }

  microphone.cancelRecording();
  decaflash::brain::api_client::cancelAiWork();
  decaflash::brain::node_text::stopAiOwned();
  decaflash::brain::text_playback::stopAiOwned(false);
  aiRecordingOwned = false;
  Serial.println("AI: disabled");
  showTransientIcon(TransientIcon::AiDisabled, 0xFF0000, kToggleDisplayMs);
}

void service(uint32_t now, const PdmMicrophone& microphone) {
  if (!aiModeEnabled) {
    return;
  }

  if (decaflash::brain::text_playback::isActive()) {
    return;
  }

  if (aiRecordingOwned || microphone.recordingActive() || microphone.recordingReady()) {
    return;
  }

  if (aiCooldownUntilMs != 0) {
    if ((int32_t)(now - aiCooldownUntilMs) < 0) {
      return;
    }

    aiCooldownUntilMs = 0;
  }

  if (aiReadyToListenAtMs != 0) {
    if ((int32_t)(now - aiReadyToListenAtMs) < 0) {
      return;
    }

    aiReadyToListenAtMs = 0;
  }

  if (!microphone.musicPresent()) {
    resetListeningWindow();
    return;
  }

  if (aiMusicPresentSinceAtMs == 0) {
    aiMusicPresentSinceAtMs = now;
    return;
  }

  if ((now - aiMusicPresentSinceAtMs) < kMusicStableMs) {
    return;
  }

  if (aiRecognitionArmedAtMs == 0) {
    aiRecognitionArmedAtMs = now;
  }

  const bool beatReady =
    microphone.detectedBpm() != 0 &&
    microphone.beatConfidence() >= kRecognitionMinBeatConfidence;
  const uint32_t onsetAtMs = microphone.lastOnsetAtMs();
  const bool recentOnset =
    onsetAtMs != 0 &&
    (now >= onsetAtMs) &&
    ((now - onsetAtMs) <= kRecognitionRecentOnsetMs);
  const bool fallbackReady =
    (now - aiRecognitionArmedAtMs) >= kRecognitionFallbackMs;

  if (!fallbackReady && (!beatReady || !recentOnset)) {
    return;
  }

  if (!decaflash::brain::startMicrophoneRecording(kRecordDurationMs)) {
    setCooldown(now, kFailureCooldownMs, "record_start_failed");
    return;
  }

  aiRecordingOwned = true;
  resetListeningWindow();
  Serial.printf("AI: record duration_ms=%lu trigger=%s bpm=%u conf=%u\n",
                static_cast<unsigned long>(kRecordDurationMs),
                fallbackReady ? "fallback" : "beat",
                static_cast<unsigned>(microphone.detectedBpm()),
                static_cast<unsigned>(microphone.beatConfidence()));
}

bool blocksBeatDotOverlay(uint32_t now, const PdmMicrophone& microphone) {
  (void)microphone;
  return transientIconActive(now);
}

bool renderOverlay(uint32_t now, const PdmMicrophone& microphone) {
  (void)microphone;
  if (transientIconActive(now)) {
    drawTransientIcon(now);
    return true;
  }

  if (transientIcon != TransientIcon::None) {
    clearTransientIcon();
  }

  return false;
}

void handleRecordingProcessed(uint32_t now, bool processed) {
  const bool aiOwned = aiRecordingOwned;
  aiRecordingOwned = false;

  if (!aiOwned) {
    return;
  }

  setCooldown(now,
              processed ? kSuccessCooldownMs : kFailureCooldownMs,
              processed ? "song_displayed" : "no_match_or_error");
}

void handleWifiFailure(uint32_t now) {
  if (!aiModeEnabled) {
    return;
  }

  disableForWifiFailure(now, "wifi_unavailable");
}

bool useAiMeterTheme(const PdmMicrophone& microphone) {
  return aiRecordingOwned ||
         microphone.recordingActive() ||
         microphone.recordingReady() ||
         decaflash::brain::api_client::radioPauseActive();
}

bool ownsRecording() {
  return aiRecordingOwned;
}

}  // namespace decaflash::brain::ai_mode
