#include <Arduino.h>

#include "command_examples.h"
#include "decaflash_types.h"
#include "protocol.h"

using decaflash::DeviceType;
using decaflash::examples::kFlashQuadSkip;
using decaflash::protocol::makeNodeCommandMessage;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("Decaflash Brain V1");
  Serial.printf("device_type=%u\n", static_cast<unsigned>(DEVICE_TYPE));
  const auto message = makeNodeCommandMessage(
    decaflash::NodeKind::Flashlight,
    kFlashQuadSkip,
    120,
    4,
    1
  );
  Serial.printf("protocol=dcfl/v%u\n", message.header.version);
  Serial.printf("example_command=%s\n", message.command.name);
  Serial.println("status=placeholder");
  Serial.println("notes=radio and microphone will be added in later steps");
}

void loop() {
  delay(1000);
}
