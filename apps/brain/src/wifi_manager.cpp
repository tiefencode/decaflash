#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

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

void printConnectionHint(wl_status_t status) {
  switch (status) {
    case WL_NO_SSID_AVAIL:
      Serial.println("wifi=hint check=ssid_name_hotspot_visibility_2.4ghz_band");
      Serial.println("wifi=hint iphone_hotspot try=enable_maximize_compatibility");
      break;

    case WL_CONNECT_FAILED:
      Serial.println("wifi=hint check=password_or_auth_method");
      break;

    case WL_DISCONNECTED:
      Serial.println("wifi=hint check=signal_strength_or_ap_state");
      break;

    default:
      break;
  }
}

bool targetSsidVisible(bool verbose) {
  const int networkCount = WiFi.scanNetworks(false, true);
  if (verbose) {
    Serial.printf("wifi=scan count=%d\n", networkCount);
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
      Serial.printf("wifi=scan ssid=%s rssi=%ld ch=%u\n",
                    ssid.c_str(),
                    static_cast<long>(rssi),
                    static_cast<unsigned>(channel));
    }

    if (ssid == decaflash::secrets::kWifiSsid) {
      foundTarget = true;
    }
  }

  if (verbose && networkCount > printedCount) {
    Serial.printf("wifi=scan more=%d\n", networkCount - printedCount);
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

bool connect() {
  if (!credentialsAvailable()) {
    Serial.println("wifi=missing_credentials file=include/wifi_credentials.h");
    return false;
  }

  if (isConnected()) {
    Serial.printf("wifi=connected ssid=%s ip=%s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.println("wifi=connect note=may_affect_espnow_channel");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(150);

  const bool ssidVisible = targetSsidVisible(false);
  Serial.printf("wifi=target visible=%s\n", ssidVisible ? "yes" : "no");
  WiFi.begin(decaflash::secrets::kWifiSsid, decaflash::secrets::kWifiPassword);

  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < kConnectTimeoutMs) {
    if (isConnected()) {
      Serial.printf("wifi=connected ssid=%s ip=%s rssi=%d\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      return true;
    }

    delay(kConnectPollMs);
  }

  const wl_status_t status = WiFi.status();
  Serial.printf("wifi=connect_failed status=%d name=%s\n",
                static_cast<int>(status),
                statusName(status));
  printConnectionHint(status);
  return false;
}

void disconnect() {
  if (!isConnected()) {
    const wl_status_t status = WiFi.status();
    Serial.printf("wifi=idle status=%d name=%s\n",
                  static_cast<int>(status),
                  statusName(status));
    return;
  }

  const String ssid = WiFi.SSID();
  WiFi.disconnect(true, false);
  Serial.printf("wifi=disconnected ssid=%s\n", ssid.c_str());
}

void scan() {
  WiFi.mode(WIFI_STA);
  targetSsidVisible(true);
}

void printStatus() {
  if (isConnected()) {
    Serial.printf("wifi=status connected ssid=%s ip=%s rssi=%d\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return;
  }

  const wl_status_t status = WiFi.status();
  Serial.printf("wifi=status disconnected wl=%d name=%s credentials=%s\n",
                static_cast<int>(status),
                statusName(status),
                credentialsAvailable() ? "yes" : "no");
  printConnectionHint(status);
}

}  // namespace decaflash::brain::wifi_manager
