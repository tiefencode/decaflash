#include "wifi_manager.h"

#include <Arduino.h>
#include <WiFi.h>

#include "matrix_ui.h"

#if __has_include("wifi_credentials.h")
#include "wifi_credentials.h"
#define DECAFLASH_WIFI_CREDENTIALS_AVAILABLE 1
#ifndef DECAFLASH_WIFI_CREDENTIALS_LIST
#error "include/wifi_credentials.h must define DECAFLASH_WIFI_CREDENTIALS_LIST(X)"
#endif
#else
#define DECAFLASH_WIFI_CREDENTIALS_AVAILABLE 0
#endif

namespace decaflash::brain::wifi_manager {

namespace {

static constexpr uint32_t kConnectTimeoutMs = 15000;
static constexpr uint32_t kConnectPollMs = 250;
static constexpr uint8_t kScanListLimit = 12;
static constexpr uint32_t kConnectedIndicatorMs = 2000;
static constexpr uint32_t kFailedIndicatorMs = 1500;

struct WifiCredential {
  const char* ssid;
  const char* password;
};

#if DECAFLASH_WIFI_CREDENTIALS_AVAILABLE
#define DECAFLASH_WIFI_CREDENTIAL_ENTRY(ssidValue, passwordValue) {ssidValue, passwordValue},
static constexpr WifiCredential kConfiguredCredentials[] = {
  DECAFLASH_WIFI_CREDENTIALS_LIST(DECAFLASH_WIFI_CREDENTIAL_ENTRY)
};
#undef DECAFLASH_WIFI_CREDENTIAL_ENTRY
#else
static constexpr WifiCredential kConfiguredCredentials[] = {
  {"", ""},
};
#endif

static constexpr size_t kConfiguredCredentialCount =
  sizeof(kConfiguredCredentials) / sizeof(kConfiguredCredentials[0]);

enum class StatusIndicator : uint8_t {
  Idle = 0,
  Connecting = 1,
  Connected = 2,
  Failed = 3,
};

StatusIndicator statusIndicator = StatusIndicator::Idle;
uint32_t connectedIndicatorUntilMs = 0;
uint32_t failedIndicatorUntilMs = 0;

bool credentialConfigured(const WifiCredential& credential) {
  return credential.ssid != nullptr && credential.ssid[0] != '\0';
}

size_t configuredCredentialCount() {
  size_t count = 0;
  for (size_t i = 0; i < kConfiguredCredentialCount; ++i) {
    if (credentialConfigured(kConfiguredCredentials[i])) {
      count++;
    }
  }

  return count;
}

bool hasNonEmptyCredentials() {
  return configuredCredentialCount() > 0;
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

void setFailedIndicator(uint32_t now) {
  statusIndicator = StatusIndicator::Failed;
  connectedIndicatorUntilMs = 0;
  failedIndicatorUntilMs = now + kFailedIndicatorMs;
}

void setConnectedIndicator(uint32_t now) {
  statusIndicator = StatusIndicator::Connected;
  connectedIndicatorUntilMs = now + kConnectedIndicatorMs;
  failedIndicatorUntilMs = 0;
}

uint32_t yellowPulseColor(uint32_t now) {
  const uint32_t pulsePhase = now % 720U;
  const uint32_t pulseRamp = (pulsePhase < 360U) ? pulsePhase : (720U - pulsePhase);
  const uint8_t intensity = static_cast<uint8_t>(72U + ((pulseRamp * 183U) / 360U));
  return (static_cast<uint32_t>(intensity) << 16) |
         (static_cast<uint32_t>((intensity * 208U) / 255U) << 8);
}

void renderStatusPixelNow(uint32_t now) {
  uint32_t colorValue = 0;
  if (statusPixelColor(now, colorValue)) {
    decaflash::brain::matrix::drawStatusPixelOverlay(colorValue);
    return;
  }

  decaflash::brain::matrix::clearStatusPixel();
}

int findConfiguredCredentialIndex(const String& ssid) {
  for (size_t i = 0; i < kConfiguredCredentialCount; ++i) {
    if (!credentialConfigured(kConfiguredCredentials[i])) {
      continue;
    }

    if (ssid == kConfiguredCredentials[i].ssid) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

bool credentialAttempted(size_t credentialIndex,
                         const size_t* attemptedCredentials,
                         size_t attemptedCount) {
  for (size_t i = 0; i < attemptedCount; ++i) {
    if (attemptedCredentials[i] == credentialIndex) {
      return true;
    }
  }

  return false;
}

size_t collectVisibleCredentials(size_t* visibleCredentials,
                                 size_t visibleCapacity,
                                 bool verbose) {
  bool visibleByCredential[kConfiguredCredentialCount] = {};
  int32_t strongestRssiByCredential[kConfiguredCredentialCount] = {};
  uint8_t channelByCredential[kConfiguredCredentialCount] = {};

  const int networkCount = WiFi.scanNetworks(false, true);
  if (verbose) {
    Serial.printf("WIFI: scan count=%d\n", networkCount);
  }

  if (networkCount <= 0) {
    return 0;
  }

  const int printedCount = (networkCount > kScanListLimit) ? kScanListLimit : networkCount;
  for (int i = 0; i < networkCount; ++i) {
    const String ssid = WiFi.SSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    const uint8_t channel = WiFi.channel(i);
    const int credentialIndex = findConfiguredCredentialIndex(ssid);
    const bool configured = credentialIndex >= 0;

    if (verbose && i < printedCount) {
      Serial.printf("WIFI: scan ssid=%s rssi=%ld ch=%u\n",
                    ssid.c_str(),
                    static_cast<long>(rssi),
                    static_cast<unsigned>(channel));
    }

    if (!configured) {
      continue;
    }

    const size_t configuredIndex = static_cast<size_t>(credentialIndex);
    if (!visibleByCredential[configuredIndex] ||
        rssi > strongestRssiByCredential[configuredIndex]) {
      visibleByCredential[configuredIndex] = true;
      strongestRssiByCredential[configuredIndex] = rssi;
      channelByCredential[configuredIndex] = channel;
    }
  }

  if (verbose && networkCount > printedCount) {
    Serial.printf("WIFI: scan more=%d\n", networkCount - printedCount);
  }

  size_t visibleCount = 0;
  for (size_t i = 0; i < kConfiguredCredentialCount && visibleCount < visibleCapacity; ++i) {
    if (!credentialConfigured(kConfiguredCredentials[i]) || !visibleByCredential[i]) {
      continue;
    }

    visibleCredentials[visibleCount++] = i;

    if (verbose) {
      Serial.printf("WIFI: candidate ssid=%s rssi=%ld ch=%u priority=%u\n",
                    kConfiguredCredentials[i].ssid,
                    static_cast<long>(strongestRssiByCredential[i]),
                    static_cast<unsigned>(channelByCredential[i]),
                    static_cast<unsigned>(visibleCount));
    }
  }

  if (verbose && visibleCount == 0 && configuredCredentialCount() > 0) {
    Serial.println("WIFI: candidate none_visible");
  }

  WiFi.scanDelete();
  return visibleCount;
}

bool connectCredential(size_t credentialIndex, bool renderStatus) {
  if (credentialIndex >= kConfiguredCredentialCount) {
    return false;
  }

  const WifiCredential& credential = kConfiguredCredentials[credentialIndex];
  if (!credentialConfigured(credential)) {
    return false;
  }

  Serial.printf("WIFI: connect_attempt ssid=%s priority=%u/%u\n",
                credential.ssid,
                static_cast<unsigned>(credentialIndex + 1U),
                static_cast<unsigned>(configuredCredentialCount()));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(150);
  WiFi.begin(credential.ssid, credential.password != nullptr ? credential.password : "");

  const uint32_t startedAtMs = millis();
  while ((millis() - startedAtMs) < kConnectTimeoutMs) {
    if (renderStatus) {
      renderStatusPixelNow(millis());
    }

    if (isConnected()) {
      setConnectedIndicator(millis());
      if (renderStatus) {
        renderStatusPixelNow(millis());
      }
      Serial.printf("WIFI: connected ssid=%s ip=%s rssi=%d\n",
                    WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      return true;
    }

    delay(kConnectPollMs);
  }

  const wl_status_t status = WiFi.status();
  WiFi.disconnect(true, false);
  Serial.printf("WIFI: candidate_failed ssid=%s status=%d name=%s\n",
                credential.ssid,
                static_cast<int>(status),
                statusName(status));
  return false;
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

bool connect(bool renderStatus) {
  if (!credentialsAvailable()) {
    Serial.println("WIFI: missing_credentials file=include/wifi_credentials.h");
    setFailedIndicator(millis());
    if (renderStatus) {
      renderStatusPixelNow(millis());
    }
    return false;
  }

  if (isConnected()) {
    setConnectedIndicator(millis());
    Serial.printf("WIFI: connected ssid=%s ip=%s\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    return true;
  }

  statusIndicator = StatusIndicator::Connecting;
  connectedIndicatorUntilMs = 0;
  failedIndicatorUntilMs = 0;
  if (renderStatus) {
    renderStatusPixelNow(millis());
  }

  size_t visibleCredentials[kConfiguredCredentialCount] = {};
  const size_t visibleCredentialCount = collectVisibleCredentials(
    visibleCredentials,
    kConfiguredCredentialCount,
    false);
  size_t attemptedCredentials[kConfiguredCredentialCount] = {};
  size_t attemptedCount = 0;

  if (visibleCredentialCount == 0) {
    Serial.println("WIFI: no_configured_network_visible fallback=ordered_connect");
  }

  for (size_t i = 0; i < visibleCredentialCount; ++i) {
    const size_t credentialIndex = visibleCredentials[i];
    attemptedCredentials[attemptedCount++] = credentialIndex;
    if (connectCredential(credentialIndex, renderStatus)) {
      return true;
    }
  }

  for (size_t credentialIndex = 0; credentialIndex < kConfiguredCredentialCount; ++credentialIndex) {
    if (!credentialConfigured(kConfiguredCredentials[credentialIndex]) ||
        credentialAttempted(credentialIndex, attemptedCredentials, attemptedCount)) {
      continue;
    }

    attemptedCredentials[attemptedCount++] = credentialIndex;
    Serial.printf("WIFI: fallback_attempt ssid=%s reason=not_visible_in_scan\n",
                  kConfiguredCredentials[credentialIndex].ssid);
    if (connectCredential(credentialIndex, renderStatus)) {
      return true;
    }
  }

  setFailedIndicator(millis());
  if (renderStatus) {
    renderStatusPixelNow(millis());
  }
  Serial.printf("WIFI: connect_failed attempts=%u\n",
                static_cast<unsigned>(attemptedCount));
  return false;
}

void disconnect() {
  const wl_status_t status = WiFi.status();
  const bool wasConnected = status == WL_CONNECTED;
  const String ssid = wasConnected ? WiFi.SSID() : String();

  WiFi.disconnect(true, false);

  if (statusIndicator != StatusIndicator::Failed) {
    statusIndicator = StatusIndicator::Idle;
    connectedIndicatorUntilMs = 0;
    failedIndicatorUntilMs = 0;
  }

  if (wasConnected) {
    Serial.printf("WIFI: disconnected ssid=%s\n", ssid.c_str());
    return;
  }

  Serial.printf("WIFI: idle status=%d name=%s\n",
                static_cast<int>(status),
                statusName(status));
}

bool statusPixelColor(uint32_t now, uint32_t& colorValue) {
  if (statusIndicator == StatusIndicator::Failed) {
    if ((int32_t)(now - failedIndicatorUntilMs) < 0) {
      colorValue = 0xFF0000;
      return true;
    }

    statusIndicator = StatusIndicator::Idle;
    failedIndicatorUntilMs = 0;
  }

  if (!isConnected() && statusIndicator == StatusIndicator::Connected) {
    statusIndicator = StatusIndicator::Idle;
    connectedIndicatorUntilMs = 0;
  }

  switch (statusIndicator) {
    case StatusIndicator::Connecting:
      colorValue = yellowPulseColor(now);
      return true;

    case StatusIndicator::Connected:
      if ((int32_t)(now - connectedIndicatorUntilMs) >= 0) {
        statusIndicator = StatusIndicator::Idle;
        connectedIndicatorUntilMs = 0;
        colorValue = 0;
        return false;
      }
      colorValue = 0x00FF00;
      return true;

    case StatusIndicator::Failed:
      colorValue = 0xFF0000;
      return true;

    case StatusIndicator::Idle:
    default:
      colorValue = 0;
      return false;
  }
}

void scan() {
  WiFi.mode(WIFI_STA);
  size_t visibleCredentials[kConfiguredCredentialCount] = {};
  (void)collectVisibleCredentials(visibleCredentials, kConfiguredCredentialCount, true);
}

void printStatus() {
  if (isConnected()) {
    Serial.printf("WIFI: status connected ssid=%s ip=%s rssi=%d credentials=%u\n",
                  WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(),
                  static_cast<unsigned>(configuredCredentialCount()));
    return;
  }

  const wl_status_t status = WiFi.status();
  Serial.printf("WIFI: status disconnected wl=%d name=%s credentials=%u\n",
                static_cast<int>(status),
                statusName(status),
                static_cast<unsigned>(configuredCredentialCount()));
}

}  // namespace decaflash::brain::wifi_manager
