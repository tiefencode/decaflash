#include "api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "text_playback.h"
#include "wifi_manager.h"

#if __has_include("relay_config.h")
#include "relay_config.h"
#define DECAFLASH_RELAY_CONFIG_AVAILABLE 1
#else
#define DECAFLASH_RELAY_CONFIG_AVAILABLE 0
namespace decaflash::secrets {
static constexpr char kRelayTextUrl[] = "";
static constexpr char kRelayBearerToken[] = "";
}  // namespace decaflash::secrets
#endif

namespace decaflash::brain::api_client {

namespace {

static constexpr char kZenUrl[] = "https://api.github.com/zen";
static constexpr size_t kRelayTextCapacity = 160;

bool relayConfigured() {
#if DECAFLASH_RELAY_CONFIG_AVAILABLE
  return decaflash::secrets::kRelayTextUrl[0] != '\0';
#else
  return false;
#endif
}

bool ensureWifiConnected() {
  if (decaflash::brain::wifi_manager::isConnected()) {
    return true;
  }

  if (decaflash::brain::wifi_manager::connect()) {
    return true;
  }

  Serial.println("api=abort reason=wifi_not_connected");
  return false;
}

bool appendCharacter(char* destination, size_t capacity, size_t& length, char value) {
  if ((length + 1U) >= capacity) {
    return false;
  }

  destination[length++] = value;
  destination[length] = '\0';
  return true;
}

bool extractJsonTextField(const String& body, char* destination, size_t capacity) {
  if (capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  const char* keyStart = strstr(body.c_str(), "\"text\"");
  if (keyStart == nullptr) {
    return false;
  }

  const char* colon = strchr(keyStart, ':');
  if (colon == nullptr) {
    return false;
  }

  const char* cursor = colon + 1;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }

  if (*cursor != '"') {
    return false;
  }

  cursor++;
  size_t length = 0;
  while (*cursor != '\0' && *cursor != '"') {
    if (*cursor == '\\') {
      cursor++;
      if (*cursor == '\0') {
        break;
      }

      char decoded = *cursor;
      switch (*cursor) {
        case 'n':
        case 'r':
        case 't':
          decoded = ' ';
          break;
        case '"':
        case '\\':
        case '/':
          decoded = *cursor;
          break;
        default:
          decoded = *cursor;
          break;
      }

      if (!appendCharacter(destination, capacity, length, decoded)) {
        return false;
      }
      cursor++;
      continue;
    }

    if (!appendCharacter(destination, capacity, length, *cursor)) {
      return false;
    }
    cursor++;
  }

  return length > 0;
}

bool bodyToDisplayText(const String& body, char* destination, size_t capacity) {
  if (capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  if (body.length() == 0) {
    return false;
  }

  if (body[0] == '{') {
    return extractJsonTextField(body, destination, capacity);
  }

  size_t length = 0;
  for (size_t i = 0; i < body.length(); ++i) {
    char character = body[i];
    if (character == '\r' || character == '\n' || character == '\t') {
      character = ' ';
    }

    if (!appendCharacter(destination, capacity, length, character)) {
      return length > 0;
    }
  }

  return length > 0;
}

bool beginSecureGet(HTTPClient& http, WiFiClientSecure& client, const char* url, const char* label) {
  client.setInsecure();
  if (!http.begin(client, url)) {
    Serial.printf("api=begin_failed endpoint=%s\n", label);
    return false;
  }

  http.addHeader("User-Agent", "decaflash-brain");
  http.addHeader("Accept", "application/json, text/plain");
  return true;
}

}  // namespace

void printHelp() {
  Serial.println("api=commands api zen | api relay");
}

bool fetchRelayTextToTextDisplay() {
  if (!relayConfigured()) {
    Serial.println("api=relay_missing_config file=include/relay_config.h");
    return false;
  }

  if (!ensureWifiConnected()) {
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginSecureGet(http, client, decaflash::secrets::kRelayTextUrl, "relay")) {
    return false;
  }

  if (decaflash::secrets::kRelayBearerToken[0] != '\0') {
    String bearerValue = "Bearer ";
    bearerValue += decaflash::secrets::kRelayBearerToken;
    http.addHeader("Authorization", bearerValue);
  }

  const int statusCode = http.GET();
  if (statusCode <= 0) {
    Serial.printf("api=request_failed endpoint=relay err=%s\n",
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  Serial.printf("api=relay status=%d bytes=%u\n",
                statusCode,
                static_cast<unsigned>(body.length()));

  if (statusCode != HTTP_CODE_OK || body.isEmpty()) {
    Serial.println("api=relay_empty");
    return false;
  }

  char displayText[kRelayTextCapacity] = {};
  if (!bodyToDisplayText(body, displayText, sizeof(displayText))) {
    Serial.println("api=relay_parse_failed expected=text_plain_or_json_text_field");
    return false;
  }

  return decaflash::brain::text_playback::start(displayText);
}

bool fetchZenToTextDisplay() {
  if (!ensureWifiConnected()) {
    return false;
  }

  WiFiClientSecure client;
  HTTPClient http;
  if (!beginSecureGet(http, client, kZenUrl, "zen")) {
    return false;
  }

  const int statusCode = http.GET();
  if (statusCode <= 0) {
    Serial.printf("api=request_failed endpoint=zen err=%s\n",
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  Serial.printf("api=zen status=%d bytes=%u\n",
                statusCode,
                static_cast<unsigned>(body.length()));

  if (statusCode != HTTP_CODE_OK || body.isEmpty()) {
    Serial.println("api=zen_empty");
    return false;
  }

  return decaflash::brain::text_playback::start(body.c_str());
}

}  // namespace decaflash::brain::api_client
