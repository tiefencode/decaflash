#include "brain_shell.h"

#include <Arduino.h>

#include <cctype>
#include <cstring>
#include <cstdlib>

#include "api_client.h"
#include "node_text_channel.h"
#include "pdm_microphone.h"
#include "sync_debug.h"
#include "text_playback.h"
#include "wifi_manager.h"

namespace decaflash::brain::shell {

namespace {

static constexpr size_t kSerialCommandCapacity = 96;
static constexpr uint32_t kDefaultRecordDurationMs = 12000;

char serialCommandBuffer[kSerialCommandCapacity] = {};
size_t serialCommandLength = 0;
bool promptVisible = false;
bool lastCharacterWasCarriageReturn = false;

void eraseLastInputCodepoint() {
  if (serialCommandLength == 0) {
    return;
  }

  do {
    serialCommandLength--;
  } while (serialCommandLength > 0 &&
           (static_cast<unsigned char>(serialCommandBuffer[serialCommandLength]) & 0xC0U) == 0x80U);

  serialCommandBuffer[serialCommandLength] = '\0';
  Serial.print("\b \b");
}

void printPrompt() {
  Serial.print("brain> ");
  promptVisible = true;
}

void handleCommand(const char* commandLine) {
  if (commandLine == nullptr || commandLine[0] == '\0') {
    return;
  }

  if (strcmp(commandLine, "help") == 0) {
    printHelp();
    return;
  }

  if (strcmp(commandLine, "text") == 0) {
    decaflash::brain::node_text::start("HALLO");
    decaflash::brain::text_playback::start("HALLO");
    return;
  }

  if (strcmp(commandLine, "text clear") == 0 || strcmp(commandLine, "text stop") == 0) {
    decaflash::brain::node_text::stop();
    decaflash::brain::text_playback::stop();
    return;
  }

  if (strncmp(commandLine, "text ", 5) == 0) {
    decaflash::brain::node_text::start(commandLine + 5);
    decaflash::brain::text_playback::start(commandLine + 5);
    return;
  }

  if (strcmp(commandLine, "chattie") == 0) {
    Serial.println("SERIAL: hint usage=chattie <text>");
    return;
  }

  if (strncmp(commandLine, "chattie ", 8) == 0) {
    decaflash::brain::api_client::queueCloudChattieInputToTextDisplay(commandLine + 8);
    return;
  }

  if (strcmp(commandLine, "record") == 0) {
    decaflash::brain::startMicrophoneRecording(kDefaultRecordDurationMs);
    return;
  }

  if (strncmp(commandLine, "record ", 7) == 0) {
    char* end = nullptr;
    const unsigned long durationMs = strtoul(commandLine + 7, &end, 10);
    if (end == (commandLine + 7) || (end != nullptr && *end != '\0')) {
      Serial.println("SERIAL: hint usage=record [duration_ms]");
      return;
    }

    decaflash::brain::startMicrophoneRecording(
      static_cast<uint32_t>(durationMs));
    return;
  }

  if (strcmp(commandLine, "wifi") == 0 || strcmp(commandLine, "wifi status") == 0) {
    decaflash::brain::wifi_manager::printStatus();
    return;
  }

  if (strcmp(commandLine, "wifi connect") == 0) {
    Serial.println("WIFI: warning manual_connect_can_pause_espnow if_channel!=1");
    decaflash::brain::wifi_manager::connect(true);
    return;
  }

  if (strcmp(commandLine, "wifi scan") == 0) {
    decaflash::brain::wifi_manager::scan();
    return;
  }

  if (strcmp(commandLine, "wifi disconnect") == 0) {
    decaflash::brain::wifi_manager::disconnect();
    return;
  }

  if (strcmp(commandLine, "sync") == 0 || strcmp(commandLine, "sync status") == 0) {
    decaflash::brain::sync_debug::requestStatusPrint();
    return;
  }

  if (strcmp(commandLine, "sync log on") == 0) {
    decaflash::brain::sync_debug::setAutoLogEnabled(true);
    Serial.println("SYNC: log=on");
    return;
  }

  if (strcmp(commandLine, "sync log off") == 0) {
    decaflash::brain::sync_debug::setAutoLogEnabled(false);
    Serial.println("SYNC: log=off");
    return;
  }

  Serial.printf("SERIAL: unknown cmd=\"%s\"\n", commandLine);
  printHelp();
}

}  // namespace

void printHelp() {
  Serial.println("SERIAL: commands");
  Serial.println("  help");
  Serial.println("  text <message>");
  Serial.println("  text clear");
  Serial.println("  chattie <text>");
  Serial.println("  record");
  Serial.println("  record <duration_ms>");
  Serial.println("  wifi status");
  Serial.println("  wifi scan");
  Serial.println("  wifi connect      # may pause ESP-NOW if AP channel != 1");
  Serial.println("  wifi disconnect");
  Serial.println("  sync status");
  Serial.println("  sync log on");
  Serial.println("  sync log off");
}

void serviceSerialInput() {
  if (!promptVisible && serialCommandLength == 0) {
    printPrompt();
  }

  while (Serial.available() > 0) {
    const int rawValue = Serial.read();
    if (rawValue < 0) {
      return;
    }

    const char character = static_cast<char>(rawValue);
    if (character == '\r' || character == '\n') {
      if (character == '\n' && lastCharacterWasCarriageReturn) {
        lastCharacterWasCarriageReturn = false;
        continue;
      }

      lastCharacterWasCarriageReturn = (character == '\r');
      Serial.print("\r\n");
      promptVisible = false;

      if (serialCommandLength == 0) {
        printPrompt();
        continue;
      }

      serialCommandBuffer[serialCommandLength] = '\0';
      handleCommand(serialCommandBuffer);
      serialCommandLength = 0;
      serialCommandBuffer[0] = '\0';
      printPrompt();
      continue;
    }

    lastCharacterWasCarriageReturn = false;

    if (character == '\b' || character == 127) {
      eraseLastInputCodepoint();
      continue;
    }

    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x20U || byte == 0x7FU) {
      continue;
    }

    if ((serialCommandLength + 1U) >= kSerialCommandCapacity) {
      Serial.print("\r\n");
      promptVisible = false;
      serialCommandBuffer[serialCommandLength] = '\0';
      Serial.printf("SERIAL: drop cmd_too_long=\"%s\"\n", serialCommandBuffer);
      serialCommandLength = 0;
      serialCommandBuffer[0] = '\0';
      printPrompt();
      continue;
    }

    serialCommandBuffer[serialCommandLength++] = character;
    Serial.write(static_cast<uint8_t>(character));
  }
}

}  // namespace decaflash::brain::shell
