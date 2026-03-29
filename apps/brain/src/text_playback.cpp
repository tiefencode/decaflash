#include "text_playback.h"

#include <Arduino.h>

#include <cctype>
#include <cstring>

#include "matrix_ui.h"

namespace decaflash::brain::text_playback {

namespace {

static constexpr size_t kSerialCommandCapacity = 96;
static constexpr size_t kTextBufferCapacity = 96;
static constexpr uint32_t kCharacterDisplayMs = 640;
static constexpr uint32_t kSpaceDisplayMs = 280;
static constexpr uint32_t kCharacterGapMs = 110;

char serialCommandBuffer[kSerialCommandCapacity] = {};
size_t serialCommandLength = 0;
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

void handleSerialCommand(const char* commandLine) {
  if (commandLine == nullptr || commandLine[0] == '\0') {
    return;
  }

  if (strcmp(commandLine, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(commandLine, "text") == 0) {
    startTextPlayback("HALLO");
    return;
  }

  if (strcmp(commandLine, "text clear") == 0 || strcmp(commandLine, "text stop") == 0) {
    stopTextPlayback();
    return;
  }

  if (strncmp(commandLine, "text ", 5) == 0) {
    startTextPlayback(commandLine + 5);
    return;
  }

  Serial.printf("serial=unknown cmd=\"%s\"\n", commandLine);
  printHelp();
}

}  // namespace

bool isActive() {
  return textPlaybackActive;
}

void printHelp() {
  Serial.println("serial=text <message> | text clear | help");
}

void serviceSerialInput() {
  while (Serial.available() > 0) {
    const int rawValue = Serial.read();
    if (rawValue < 0) {
      return;
    }

    const char character = static_cast<char>(rawValue);
    if (character == '\r' || character == '\n') {
      if (serialCommandLength == 0) {
        continue;
      }

      serialCommandBuffer[serialCommandLength] = '\0';
      handleSerialCommand(serialCommandBuffer);
      serialCommandLength = 0;
      serialCommandBuffer[0] = '\0';
      continue;
    }

    if (!std::isprint(static_cast<unsigned char>(character))) {
      continue;
    }

    if ((serialCommandLength + 1U) >= kSerialCommandCapacity) {
      serialCommandBuffer[serialCommandLength] = '\0';
      Serial.printf("serial=drop cmd_too_long=\"%s\"\n", serialCommandBuffer);
      serialCommandLength = 0;
      serialCommandBuffer[0] = '\0';
      continue;
    }

    serialCommandBuffer[serialCommandLength++] = character;
  }
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
