#include "brain_shell.h"

#include <Arduino.h>

#include <cctype>
#include <cstring>

#include "api_client.h"
#include "text_playback.h"
#include "wifi_manager.h"

namespace decaflash::brain::shell {

namespace {

static constexpr size_t kSerialCommandCapacity = 96;

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
    decaflash::brain::text_playback::start("HALLO");
    return;
  }

  if (strcmp(commandLine, "text clear") == 0 || strcmp(commandLine, "text stop") == 0) {
    decaflash::brain::text_playback::stop();
    return;
  }

  if (strncmp(commandLine, "text ", 5) == 0) {
    decaflash::brain::text_playback::start(commandLine + 5);
    return;
  }

  if (strcmp(commandLine, "chattie") == 0) {
    Serial.println("serial=hint usage=chattie <text>");
    return;
  }

  if (strncmp(commandLine, "chattie ", 8) == 0) {
    decaflash::brain::api_client::fetchCloudChattieInputToTextDisplay(commandLine + 8);
    return;
  }

  if (strcmp(commandLine, "wifi") == 0 || strcmp(commandLine, "wifi status") == 0) {
    decaflash::brain::wifi_manager::printStatus();
    return;
  }

  if (strcmp(commandLine, "wifi connect") == 0) {
    decaflash::brain::wifi_manager::connect();
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

  Serial.printf("serial=unknown cmd=\"%s\"\n", commandLine);
  printHelp();
}

}  // namespace

void printHelp() {
  Serial.println("serial=commands");
  Serial.println("  help");
  Serial.println("  text <message>");
  Serial.println("  text clear");
  Serial.println("  chattie <text>");
  Serial.println("  wifi status");
  Serial.println("  wifi scan");
  Serial.println("  wifi connect");
  Serial.println("  wifi disconnect");
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
      Serial.printf("serial=drop cmd_too_long=\"%s\"\n", serialCommandBuffer);
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
