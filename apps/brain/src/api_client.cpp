#include "api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "text_playback.h"
#include "wifi_manager.h"

#if __has_include("cloud_config.h")
#include "cloud_config.h"
#define DECAFLASH_CLOUD_CONFIG_AVAILABLE 1
#else
#define DECAFLASH_CLOUD_CONFIG_AVAILABLE 0
namespace decaflash::secrets {
static constexpr char kCloudChattieUrl[] = "";
static constexpr char kBrainSharedSecret[] = "";
}  // namespace decaflash::secrets
#endif

namespace decaflash::brain::api_client {

namespace {

static constexpr size_t kCloudTextCapacity = 96;
static constexpr char kCloudChattieMonitorPrompt[] =
  "Schreibe genau einen sehr kurzen kreativen deutschen Text fuer eine 5x5-LED-Matrix. "
  "Nutze die Nutzereingabe als Inspiration. Keine Erklaerung. Keine Emojis. "
  "Maximal 8 Woerter.";

bool fetchCloudChattieText(const char* input,
                           const char* instructions,
                           char* destination,
                           size_t capacity);

bool cloudConfigured() {
#if DECAFLASH_CLOUD_CONFIG_AVAILABLE
  return decaflash::secrets::kCloudChattieUrl[0] != '\0' &&
         decaflash::secrets::kBrainSharedSecret[0] != '\0';
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

bool extractJsonStringField(const String& body,
                            const char* fieldName,
                            char* destination,
                            size_t capacity) {
  if (capacity == 0) {
    return false;
  }

  destination[0] = '\0';
  char fieldPattern[40] = {};
  snprintf(fieldPattern, sizeof(fieldPattern), "\"%s\"", fieldName);

  const char* keyStart = strstr(body.c_str(), fieldPattern);
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

void appendJsonEscaped(String& destination, const char* source) {
  if (source == nullptr) {
    return;
  }

  while (*source != '\0') {
    switch (*source) {
      case '\\':
        destination += "\\\\";
        break;

      case '"':
        destination += "\\\"";
        break;

      case '\n':
        destination += "\\n";
        break;

      case '\r':
        destination += "\\r";
        break;

      case '\t':
        destination += "\\t";
        break;

      default:
        destination += *source;
        break;
    }

    source++;
  }
}

bool beginSecureRequest(HTTPClient& http,
                        WiFiClientSecure& client,
                        const char* url,
                        const char* label) {
  client.setInsecure();
  if (!http.begin(client, url)) {
    Serial.printf("api=begin_failed endpoint=%s\n", label);
    return false;
  }

  http.addHeader("User-Agent", "decaflash-brain");
  http.addHeader("Accept", "application/json, text/plain");
  return true;
}

void addCloudAuthorizationHeader(HTTPClient& http) {
  String bearerValue = "Bearer ";
  bearerValue += decaflash::secrets::kBrainSharedSecret;
  http.addHeader("Authorization", bearerValue);
}

bool postCloudJson(HTTPClient& http,
                   WiFiClientSecure& client,
                   const char* url,
                   const char* label,
                   const String& payload,
                   String& responseBody) {
  if (!beginSecureRequest(http, client, url, label)) {
    return false;
  }

  addCloudAuthorizationHeader(http);
  http.addHeader("Content-Type", "application/json");

  const int statusCode = http.POST(payload);
  if (statusCode <= 0) {
    Serial.printf("api=request_failed endpoint=%s err=%s\n",
                  label,
                  http.errorToString(statusCode).c_str());
    http.end();
    return false;
  }

  responseBody = http.getString();
  http.end();

  Serial.printf("api=%s status=%d bytes=%u\n",
                label,
                statusCode,
                static_cast<unsigned>(responseBody.length()));

  if (statusCode != HTTP_CODE_OK || responseBody.isEmpty()) {
    Serial.printf("api=%s_empty\n", label);
    return false;
  }

  return true;
}

bool fetchCloudChattieText(const char* input,
                           const char* instructions,
                           char* destination,
                           size_t capacity) {
  if (capacity == 0 || input == nullptr || instructions == nullptr) {
    return false;
  }

  destination[0] = '\0';

  String payload = "{\"instructions\":\"";
  appendJsonEscaped(payload, instructions);
  payload += "\",\"input\":\"";
  appendJsonEscaped(payload, input);
  payload += "\"}";

  WiFiClientSecure client;
  HTTPClient http;
  String body;
  if (!postCloudJson(http, client, decaflash::secrets::kCloudChattieUrl, "chattie", payload, body)) {
    return false;
  }

  if (!extractJsonStringField(body, "text", destination, capacity)) {
    Serial.println("api=chattie_parse_failed expected=text");
    return false;
  }

  Serial.printf("api=chattie_text text=\"%s\"\n", destination);
  return true;
}

}  // namespace

bool fetchCloudChattieInputToTextDisplay(const char* input) {
  if (!cloudConfigured()) {
    Serial.println("api=cloud_missing_config file=include/cloud_config.h");
    Serial.println("api=cloud_expected keys=kCloudChattieUrl,kBrainSharedSecret");
    return false;
  }

  if (input == nullptr || input[0] == '\0') {
    Serial.println("api=chattie_missing_input");
    return false;
  }

  if (!ensureWifiConnected()) {
    return false;
  }

  char displayText[kCloudTextCapacity] = {};
  if (!fetchCloudChattieText(
        input, kCloudChattieMonitorPrompt, displayText, sizeof(displayText))) {
    return false;
  }

  return decaflash::brain::text_playback::start(displayText);
}

}  // namespace decaflash::brain::api_client
