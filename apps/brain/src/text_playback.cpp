#include "text_playback.h"

#include <Arduino.h>

#include <cstring>

#include "matrix_ui.h"

namespace decaflash::brain::text_playback {

namespace {

static constexpr size_t kTextBufferCapacity = 96;
static constexpr int16_t kTextLeadingColumns = 5;
static constexpr uint32_t kIntroFlashPeakMs = 34;
static constexpr uint32_t kIntroFlashTailMs = 56;
static constexpr uint32_t kIntroFlashAnimationMs = kIntroFlashPeakMs + kIntroFlashTailMs;
static constexpr uint32_t kIntroFlashFrameMs = 16;
static constexpr uint32_t kIntroPauseMs = 1000;
static constexpr uint32_t kScrollStepMs = 130;
static constexpr uint8_t kGlyphUmlautA = 0x80;
static constexpr uint8_t kGlyphUmlautO = 0x81;
static constexpr uint8_t kGlyphUmlautU = 0x82;
static constexpr uint8_t kGlyphSharpS = 0x83;

enum class PlaybackPhase : uint8_t {
  IntroFlash = 0,
  IntroPause = 1,
  Scroll = 2,
};

uint8_t textPlaybackBuffer[kTextBufferCapacity] = {};
size_t textPlaybackLength = 0;
size_t textPlaybackColumns = 0;
int16_t textPlaybackColumn = 0;
bool textPlaybackActive = false;
uint32_t nextTextPlaybackAtMs = 0;
PlaybackPhase playbackPhase = PlaybackPhase::IntroFlash;
uint32_t playbackPhaseStartedAtMs = 0;
Owner textPlaybackOwner = Owner::Manual;

bool appendPlaybackCharacter(size_t& length, uint8_t character) {
  if ((length + 1U) >= kTextBufferCapacity) {
    return false;
  }

  textPlaybackBuffer[length] = character;
  length++;
  return true;
}

bool appendUtf8NormalizedCharacter(const char*& cursor, size_t& length) {
  const uint8_t byte0 = static_cast<uint8_t>(cursor[0]);

  if (byte0 < 0x80) {
    cursor++;
    return appendPlaybackCharacter(length, byte0);
  }

  if (byte0 == 0xC3 && cursor[1] != '\0') {
    const uint8_t byte1 = static_cast<uint8_t>(cursor[1]);
    cursor += 2;

    switch (byte1) {
      case 0x84:
      case 0xA4:
        return appendPlaybackCharacter(length, kGlyphUmlautA);

      case 0x96:
      case 0xB6:
        return appendPlaybackCharacter(length, kGlyphUmlautO);

      case 0x9C:
      case 0xBC:
        return appendPlaybackCharacter(length, kGlyphUmlautU);

      case 0x9F:
        return appendPlaybackCharacter(length, kGlyphSharpS);

      default:
        return appendPlaybackCharacter(length, '?');
    }
  }

  if (byte0 == 0xE1 && cursor[1] != '\0' && cursor[2] != '\0' &&
      static_cast<uint8_t>(cursor[1]) == 0xBA &&
      static_cast<uint8_t>(cursor[2]) == 0x9E) {
    cursor += 3;
    return appendPlaybackCharacter(length, kGlyphSharpS);
  }

  cursor++;

  while ((*cursor != '\0') &&
         ((static_cast<uint8_t>(*cursor) & 0xC0U) == 0x80U)) {
    cursor++;
  }

  return appendPlaybackCharacter(length, '?');
}

uint32_t introFlashColor(uint32_t elapsedMs) {
  if (elapsedMs < kIntroFlashPeakMs) {
    return 0xFFFFFF;
  }

  const uint32_t tailElapsedMs = elapsedMs - kIntroFlashPeakMs;
  if (tailElapsedMs >= kIntroFlashTailMs) {
    return 0x000000;
  }

  const uint8_t blue = static_cast<uint8_t>(
    196U - ((tailElapsedMs * 196U) / kIntroFlashTailMs));
  const uint8_t green = static_cast<uint8_t>((static_cast<uint16_t>(blue) * 210U) / 255U);
  const uint8_t red = static_cast<uint8_t>((static_cast<uint16_t>(blue) * 120U) / 255U);
  return (static_cast<uint32_t>(red) << 16) |
         (static_cast<uint32_t>(green) << 8) |
         static_cast<uint32_t>(blue);
}

void clearTextPlayback() {
  textPlaybackActive = false;
  textPlaybackLength = 0;
  textPlaybackColumns = 0;
  textPlaybackColumn = 0;
  nextTextPlaybackAtMs = 0;
  playbackPhase = PlaybackPhase::IntroFlash;
  playbackPhaseStartedAtMs = 0;
  textPlaybackOwner = Owner::Manual;
  textPlaybackBuffer[0] = '\0';
  decaflash::brain::matrix::clearAllPixels();
}

void stopTextPlayback(bool announce = true) {
  clearTextPlayback();

  if (announce) {
    Serial.println("TEXT: clear");
  }
}

void startTextPlayback(const char* rawText, uint32_t delayMs, Owner owner) {
  if (rawText == nullptr) {
    stopTextPlayback();
    return;
  }

  while (*rawText == ' ') {
    rawText++;
  }

  const char* cursor = rawText;
  size_t length = 0;
  while (*cursor != '\0' && *cursor != '\r' && *cursor != '\n') {
    if (!appendUtf8NormalizedCharacter(cursor, length)) {
      break;
    }
  }

  textPlaybackLength = length;
  textPlaybackColumns =
    decaflash::brain::matrix::measureTextColumns(textPlaybackBuffer, textPlaybackLength);
  textPlaybackColumn = -kTextLeadingColumns;
  playbackPhaseStartedAtMs = millis() + delayMs;
  nextTextPlaybackAtMs = playbackPhaseStartedAtMs;
  playbackPhase = PlaybackPhase::IntroFlash;
  textPlaybackActive = (length > 0) && (textPlaybackColumns > 0);
  textPlaybackOwner = owner;

  if (!textPlaybackActive) {
    stopTextPlayback();
    return;
  }

  Serial.printf("TEXT: start len=%u cols=%u\n",
                static_cast<unsigned>(textPlaybackLength),
                static_cast<unsigned>(textPlaybackColumns));
}

}  // namespace

bool isActive() {
  return textPlaybackActive;
}

bool isAiOwnedActive() {
  return textPlaybackActive && textPlaybackOwner == Owner::Ai;
}

bool start(const char* text, uint32_t delayMs, Owner owner) {
  startTextPlayback(text, delayMs, owner);
  return textPlaybackActive;
}

void stop(bool announce) {
  stopTextPlayback(announce);
}

bool stopAiOwned(bool announce) {
  if (!isAiOwnedActive()) {
    return false;
  }

  stopTextPlayback(announce);
  return true;
}

void printHelp() {
  Serial.println("TEXT: api start(text) stop()");
}

bool serviceMatrix(uint32_t now) {
  if (!textPlaybackActive) {
    return false;
  }

  if ((int32_t)(now - nextTextPlaybackAtMs) < 0) {
    return true;
  }

  if (playbackPhase == PlaybackPhase::IntroFlash) {
    const uint32_t elapsedMs = now - playbackPhaseStartedAtMs;
    if (elapsedMs >= kIntroFlashAnimationMs) {
      decaflash::brain::matrix::clearAllPixels();
      playbackPhase = PlaybackPhase::IntroPause;
      playbackPhaseStartedAtMs = now;
      nextTextPlaybackAtMs = now + kIntroPauseMs;
      return true;
    }

    decaflash::brain::matrix::drawSolidColor(introFlashColor(elapsedMs));
    nextTextPlaybackAtMs = now + kIntroFlashFrameMs;
    return true;
  }

  if (playbackPhase == PlaybackPhase::IntroPause) {
    playbackPhase = PlaybackPhase::Scroll;
  }

  if (textPlaybackColumn >= static_cast<int16_t>(textPlaybackColumns)) {
    clearTextPlayback();
    Serial.println("TEXT: done");
    return true;
  }

  decaflash::brain::matrix::drawTextWindow(
    textPlaybackBuffer,
    textPlaybackLength,
    textPlaybackColumn);
  textPlaybackColumn++;
  nextTextPlaybackAtMs = now + kScrollStepMs;
  return true;
}

}  // namespace decaflash::brain::text_playback
