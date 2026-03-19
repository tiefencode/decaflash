#include <Arduino.h>

#include "command_examples.h"
#include "decaflash_types.h"
#include "espnow_transport.h"
#include "protocol.h"

using decaflash::DeviceType;
using decaflash::examples::kFlashQuadSkip;
using decaflash::espnow_transport::ensureBroadcastPeer;
using decaflash::espnow_transport::initEspNow;
using decaflash::protocol::makeNodeCommandMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;
static constexpr uint32_t COMMAND_REFRESH_MS = 10000;
uint32_t nextSendAtMs = 0;
bool espNowReady = false;
uint32_t commandRevision = 1;

void sendDemoCommand() {
  if (!espNowReady) {
    return;
  }

  const auto message = makeNodeCommandMessage(
    decaflash::NodeKind::Flashlight,
    kFlashQuadSkip,
    120,
    4,
    commandRevision
  );

  const auto result = esp_now_send(
    decaflash::espnow_transport::kBroadcastMac,
    reinterpret_cast<const uint8_t*>(&message),
    sizeof(message)
  );

  Serial.printf("send=node_command result=%d command=%s\n", result, message.command.name);
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("Decaflash Brain V1");
  Serial.printf("device_type=%u\n", static_cast<unsigned>(DEVICE_TYPE));
  const auto message = makeNodeCommandMessage(decaflash::NodeKind::Flashlight, kFlashQuadSkip, 120, 4, 1);
  Serial.printf("protocol=dcfl/v%u\n", message.header.version);
  Serial.printf("example_command=%s\n", message.command.name);

  const auto initResult = initEspNow();
  const auto peerResult = initResult.ok() ? ensureBroadcastPeer() : decltype(ensureBroadcastPeer()){};
  espNowReady = initResult.ok() && peerResult.ok();

  Serial.printf("wifi_set_mode=%d\n", static_cast<int>(initResult.wifiSetMode));
  Serial.printf("wifi_start=%d\n", static_cast<int>(initResult.wifiStart));
  Serial.printf("wifi_set_channel=%d\n", static_cast<int>(initResult.wifiSetChannel));
  Serial.printf("esp_now_init=%d\n", static_cast<int>(initResult.espNowInit));
  Serial.printf("esp_now=%s\n", initResult.ok() ? "ok" : "failed");
  Serial.printf("peer_exists=%s\n", peerResult.alreadyExisted ? "yes" : "no");
  Serial.printf("add_peer=%d\n", static_cast<int>(peerResult.addPeer));
  Serial.printf("mode=%s\n", espNowReady ? "broadcast command sender" : "startup only");

  if (espNowReady) {
    sendDemoCommand();
  }

  nextSendAtMs = millis() + COMMAND_REFRESH_MS;
}

void loop() {
  const uint32_t now = millis();

  if ((int32_t)(now - nextSendAtMs) >= 0) {
    sendDemoCommand();
    nextSendAtMs += COMMAND_REFRESH_MS;
  }
}
