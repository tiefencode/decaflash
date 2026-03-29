#include "api_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "text_playback.h"
#include "wifi_manager.h"

namespace decaflash::brain::api_client {

namespace {

static constexpr char kZenUrl[] = "https://api.github.com/zen";

}  // namespace

void printHelp() {
  Serial.println("api=commands api zen");
}

bool fetchZenToTextDisplay() {
  if (!decaflash::brain::wifi_manager::isConnected() &&
      !decaflash::brain::wifi_manager::connect()) {
    Serial.println("api=abort reason=wifi_not_connected");
    return false;
  }

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, kZenUrl)) {
    Serial.println("api=begin_failed endpoint=zen");
    return false;
  }

  http.addHeader("User-Agent", "decaflash-brain");
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

  decaflash::brain::text_playback::start(body.c_str());
  return true;
}

}  // namespace decaflash::brain::api_client
