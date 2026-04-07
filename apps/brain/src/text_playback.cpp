#include "text_playback.h"

#include <Arduino.h>

#include <cstring>

#include "matrix_ui.h"

namespace decaflash::brain::text_playback {

namespace {

static constexpr size_t kTextBufferCapacity = 96;
static constexpr uint32_t kCharacterDisplayMs = 400;
static constexpr uint32_t kSpaceDisplayMs = 160;
static constexpr uint32_t kCharacterGapMs = 66;
static constexpr uint8_t kGlyphUmlautA = 0x80;
static constexpr uint8_t kGlyphUmlautO = 0x81;
static constexpr uint8_t kGlyphUmlautU = 0x82;
static constexpr uint8_t kGlyphSharpS = 0x83;

uint8_t textPlaybackBuffer[kTextBufferCapacity] = {};
size_t textPlaybackLength = 0;
size_t textPlaybackIndex = 0;
bool textPlaybackActive = false;
bool textPlaybackCharacterVisible = false;
uint32_t nextTextPlaybackAtMs = 0;

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

void clearTextPlayback() {
  textPlaybackActive = false;
  textPlaybackCharacterVisible = false;
  textPlaybackIndex = 0;
  textPlaybackLength = 0;
  nextTextPlaybackAtMs = 0;
  textPlaybackBuffer[0] = '\0';
  decaflash::brain::matrix::clearAllPixels();
}

void stopTextPlayback(bool announce = true) {
  clearTextPlayback();

  if (announce) {
    Serial.println("text=clear");
  }
}

uint32_t textDisplayDurationMs(char character) {
  return (character == ' ') ? kSpaceDisplayMs : kCharacterDisplayMs;
}

void startTextPlayback(const char* rawText) {
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
  textPlaybackIndex = 0;
  textPlaybackCharacterVisible = false;
  nextTextPlaybackAtMs = 0;
  textPlaybackActive = length > 0;

  if (!textPlaybackActive) {
    stopTextPlayback();
    return;
  }

  Serial.printf("text=start len=%u\n", static_cast<unsigned>(textPlaybackLength));
}

}  // namespace

bool isActive() {
  return textPlaybackActive;
}

bool start(const char* text) {
  startTextPlayback(text);
  return textPlaybackActive;
}

void stop(bool announce) {
  stopTextPlayback(announce);
}

void printHelp() {
  Serial.println("text=api start(text) stop()");
}

bool serviceMatrix(uint32_t now) {
  if (!textPlaybackActive) {
    return false;
  }

  if ((int32_t)(now - nextTextPlaybackAtMs) < 0) {
    return true;
  }

  if (!textPlaybackCharacterVisible) {
    const uint8_t character = textPlaybackBuffer[textPlaybackIndex];
    decaflash::brain::matrix::drawTextCharacter(character);
    textPlaybackCharacterVisible = true;
    nextTextPlaybackAtMs = now + textDisplayDurationMs(character);
    return true;
  }

  decaflash::brain::matrix::clearAllPixels();
  textPlaybackCharacterVisible = false;
  textPlaybackIndex++;

  if (textPlaybackIndex >= textPlaybackLength) {
    clearTextPlayback();
    Serial.println("text=done");
    return true;
  }

  nextTextPlaybackAtMs = now + kCharacterGapMs;
  return true;
}

}  // namespace decaflash::brain::text_playback
