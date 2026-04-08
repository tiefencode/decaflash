#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "matrix_ui.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#define DECAFLASH_WIFI_CREDENTIALS_AVAILABLE 1
#else
#define DECAFLASH_WIFI_CREDENTIALS_AVAILABLE 0
namespace decaflash::secrets {
static constexpr char kWifiSsid[] = "";
static constexpr char kWifiPassword[] = "";
}  // namespace decaflash::secrets
#endif

namespace decaflash::brain::wifi_manager {

namespace {

static constexpr uint32_t kConnectTimeoutMs = 15000;
static constexpr uint32_t kConnectPollMs = 250;
static constexpr uint8_t kScanListLimit = 12;

uint32_t wifiPulseColor(uint32_t now) {
  const uint32_t cycleMs = 820;
  const uint32_t phase = now % cycleMs;
  const uint32_t halfCycleMs = cycleMs / 2U;
  const uint32_t ramp = (phase < halfCycleMs) ? phase : (cycleMs - phase);
  const uint32_t intensity = (ramp * 255U) / halfCycleMs;
  const uint8_t red = static_cast<uint8_t>(35U + ((intensity * 220U) / 255U));
  const uint8_t green = static_cast<uint8_t>(12U + ((intensity * 208U) / 255U));
  return (static_cast<uint32_t>(red) << 16) |
         (static_cast<uint32_t>(green) << 8);
}

bool hasNonEmptyCredentials() {
  return decaflash::secrets::kWifiSsid[0] != '\0';
}

const char* statusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:
      return "idle";
    case WL_NO_SSID_AVAIL:
      return "no_ssid";
    case WL_SCAN_COMPLETED:
      return "scan_done";
    case WL_CONNECTED:
      return "connected";
    case WL_CONNECT_FAILED:
      return "connect_failed";
    case WL_CONNECTION_LOST:
      return "connection_lost";
    case WL_DISCONNECTED:
      return "disconnected";
    default:
      return "unknown";
  }
}

bool targetSsidVisible(bool verbose) {
  const int networkCount = WiFi.scanNetworks(false, true);
  if (verbose) {
    Serial.printf("WIFI: scan count=%d\n", networkCount);
  }

  if (networkCount <= 0) {
    return false;
  }

  bool foundTarget = false;
  const uint8_t printedCount = (networkCount > kScanListLimit) ? kScanListLimit : networkCount;
  for (uint8_t i = 0; i < printedCount; ++i) {
    const String ssid = WiFi.SSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    const uint8_t channel = WiFi.channel(i);

    if (verbose) {
      Serial.printf("WIFI: scan ssid=%s rssi=%ld ch=%u\n",
                    ssid.c_str(),
                    static_cast<long>(rssi),
                    static_cast<unsigned>(channel));
    }

    if (ssid == decaflash::secrets::kWifiSsid) {
      foundTarget = true;
    }
  }

  if (verbose && networkCount > printedCount) {
    Serial.printf("WIFI: scan more=%d\n", networkCount - printedCount);
  }

  WiFi.scanDelete();
  return foundTarget;
}

}  // namespace

bool credentialsAvailable() {
#if DECAFLASH_WIFI_CREDENTIALS_AVAILABLE
  return hasNonEmptyCredentials();
#else
  return false;
#endif
}

bool isConnected() {
  return WiFi.status() == WL_CONNECTED;
}

uint8_t currentChannel() {
  return isConnected() ? static_cast<uint8_t>(WiFi.channel()) : 0;
}

bool connect() {
  if (!credentialsAvailable()) {
    Serial.println("WIFI: missing_credentials file=include/wifi_credentials.h");
    return false;
  }

  if (isConnected()) {
    Serial.printf("WIFI: connected ssid=%s ip=%s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  decaflash::brain::matrix::drawWifiConnectingIcon(millis(), wifiPulseColor(millis()));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.begin(decaflash::secrets::kWifiSsid, decaflash::secrets::kWifiPassword);

  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < kConnectTimeoutMs) {
    decaflash::brain::matrix::drawWifiConnectingIcon(millis(), wifiPulseColor(millis()));

    if (isConnected()) {
      Serial.printf("WIFI: connected ssid=%s ip=%s rssi=%d\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      return true;
    }

    delay(kConnectPollMs);
  }

  const wl_status_t status = WiFi.status();
  Serial.printf("WIFI: connect_failed status=%d name=%s\n",
                static_cast<int>(status),
                statusName(status));
  return false;
}

void disconnect() {
  if (!isConnected()) {
    const wl_status_t status = WiFi.status();
    Serial.printf("WIFI: idle status=%d name=%s\n",
                  static_cast<int>(status),
                  statusName(status));
    return;
  }

  const String ssid = WiFi.SSID();
  WiFi.disconnect(true, false);
  Serial.printf("WIFI: disconnected ssid=%s\n", ssid.c_str());
}

void scan() {
  WiFi.mode(WIFI_STA);
  targetSsidVisible(true);
}

void printStatus() {
  if (isConnected()) {
    Serial.printf("WIFI: status connected ssid=%s ip=%s rssi=%d\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return;
  }

  const wl_status_t status = WiFi.status();
  Serial.printf("WIFI: status disconnected wl=%d name=%s credentials=%s\n",
                static_cast<int>(status),
                statusName(status),
                credentialsAvailable() ? "yes" : "no");
}

}  // namespace decaflash::brain::wifi_manager
