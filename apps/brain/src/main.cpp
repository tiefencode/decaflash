#include <Arduino.h>

#include "decaflash_types.h"

using decaflash::DeviceType;

static constexpr DeviceType DEVICE_TYPE = DeviceType::Brain;

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("Decaflash Brain V1");
  Serial.printf("device_type=%u\n", static_cast<unsigned>(DEVICE_TYPE));
  Serial.println("status=placeholder");
  Serial.println("notes=radio and microphone will be added in later steps");
}

void loop() {
  delay(1000);
}
