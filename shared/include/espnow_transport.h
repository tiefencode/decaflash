#pragma once

#include <Arduino.h>
#include <esp_now.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "protocol.h"

namespace decaflash::espnow_transport {

static constexpr uint8_t kBroadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static constexpr uint8_t kWifiChannel = 1;

struct InitResult {
  esp_err_t netifInit = ESP_FAIL;
  esp_err_t eventLoopCreate = ESP_FAIL;
  esp_err_t wifiInit = ESP_FAIL;
  esp_err_t wifiSetMode = ESP_FAIL;
  esp_err_t wifiStart = ESP_FAIL;
  esp_err_t wifiSetChannel = ESP_FAIL;
  esp_err_t espNowInit = ESP_FAIL;

  bool ok() const {
    return netifInit == ESP_OK &&
           (eventLoopCreate == ESP_OK || eventLoopCreate == ESP_ERR_INVALID_STATE) &&
           wifiInit == ESP_OK &&
           wifiSetMode == ESP_OK &&
           wifiStart == ESP_OK &&
           wifiSetChannel == ESP_OK &&
           espNowInit == ESP_OK;
  }
};

struct PeerResult {
  bool alreadyExisted = false;
  esp_err_t addPeer = ESP_FAIL;

  bool ok() const {
    return alreadyExisted || addPeer == ESP_OK;
  }
};

struct RecoveryResult {
  esp_err_t wifiStart = ESP_FAIL;
  esp_err_t wifiSetChannel = ESP_FAIL;
  esp_err_t espNowDeinit = ESP_FAIL;
  esp_err_t espNowInit = ESP_FAIL;
  PeerResult peer = {};

  bool ok() const {
    const bool wifiStartOk = wifiStart == ESP_OK || wifiStart == ESP_ERR_WIFI_CONN;
    const bool deinitOk = espNowDeinit == ESP_OK || espNowDeinit == ESP_ERR_ESPNOW_NOT_INIT;
    return wifiStartOk &&
           wifiSetChannel == ESP_OK &&
           deinitOk &&
           espNowInit == ESP_OK &&
           peer.ok();
  }
};

inline InitResult initEspNow() {
  InitResult result;

  result.netifInit = esp_netif_init();
  if (result.netifInit != ESP_OK && result.netifInit != ESP_ERR_INVALID_STATE) {
    return result;
  }
  if (result.netifInit == ESP_ERR_INVALID_STATE) {
    result.netifInit = ESP_OK;
  }

  result.eventLoopCreate = esp_event_loop_create_default();
  if (result.eventLoopCreate != ESP_OK && result.eventLoopCreate != ESP_ERR_INVALID_STATE) {
    return result;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  result.wifiInit = esp_wifi_init(&cfg);
  if (result.wifiInit != ESP_OK) {
    return result;
  }

  result.wifiSetMode = esp_wifi_set_mode(WIFI_MODE_STA);
  if (result.wifiSetMode != ESP_OK) {
    return result;
  }

  result.wifiStart = esp_wifi_start();
  if (result.wifiStart != ESP_OK && result.wifiStart != ESP_ERR_WIFI_CONN) {
    return result;
  }

  result.wifiSetChannel = esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
  if (result.wifiSetChannel != ESP_OK) {
    return result;
  }

  result.espNowInit = esp_now_init();
  return result;
}

inline PeerResult ensureBroadcastPeer() {
  PeerResult result;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (esp_now_is_peer_exist(kBroadcastMac)) {
    result.alreadyExisted = true;
    result.addPeer = ESP_OK;
    return result;
  }

  result.addPeer = esp_now_add_peer(&peerInfo);
  return result;
}

inline bool stationAssociated() {
  wifi_ap_record_t accessPointInfo = {};
  return esp_wifi_sta_get_ap_info(&accessPointInfo) == ESP_OK;
}

inline RecoveryResult recoverEspNow() {
  RecoveryResult result;

  result.wifiStart = esp_wifi_start();
  if (result.wifiStart != ESP_OK && result.wifiStart != ESP_ERR_WIFI_CONN) {
    return result;
  }

  if (stationAssociated()) {
    result.wifiSetChannel = ESP_OK;
  } else {
    result.wifiSetChannel = esp_wifi_set_channel(kWifiChannel, WIFI_SECOND_CHAN_NONE);
    if (result.wifiSetChannel != ESP_OK) {
      return result;
    }
  }

  result.espNowDeinit = esp_now_deinit();
  result.espNowInit = esp_now_init();
  if (result.espNowInit != ESP_OK) {
    return result;
  }

  result.peer = ensureBroadcastPeer();
  return result;
}

inline bool isValidHeader(const protocol::MessageHeader& header, protocol::MessageType type) {
  return header.magic == protocol::kProtocolMagic &&
         header.version == protocol::kProtocolVersion &&
         header.type == type;
}

}  // namespace decaflash::espnow_transport
