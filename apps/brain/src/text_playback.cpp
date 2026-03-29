#include "text_playback.h"

#include <Arduino.h>

#include <cstring>

#include "matrix_ui.h"

namespace decaflash::brain::text_playback {

namespace {

static constexpr size_t kTextBufferCapacity = 96;
static constexpr uint32_t kCharacterDisplayMs = 640;
static constexpr uint32_t kSpaceDisplayMs = 280;
static constexpr uint32_t kCharacterGapMs = 110;

char textPlaybackBuffer[kTextBufferCapacity] = {};
size_t textPlaybackLength = 0;
size_t textPlaybackIndex = 0;
bool textPlaybackActive = false;
bool textPlaybackCharacterVisible = false;
uint32_t nextTextPlaybackAtMs = 0;

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

  size_t length = 0;
  while (rawText[length] != '\0' && rawText[length] != '\r' && rawText[length] != '\n') {
    if ((length + 1U) >= kTextBufferCapacity) {
      break;
    }

    textPlaybackBuffer[length] = rawText[length];
    length++;
  }

  textPlaybackBuffer[length] = '\0';
  textPlaybackLength = length;
  textPlaybackIndex = 0;
  textPlaybackCharacterVisible = false;
  nextTextPlaybackAtMs = 0;
  textPlaybackActive = length > 0;

  if (!textPlaybackActive) {
    stopTextPlayback();
    return;
  }

  Serial.printf("text=start len=%u text=\"%s\"\n",
                static_cast<unsigned>(textPlaybackLength),
                textPlaybackBuffer);
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
    const char character = textPlaybackBuffer[textPlaybackIndex];
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
