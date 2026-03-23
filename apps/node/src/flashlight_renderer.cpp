#include "flashlight_renderer.h"

void FlashlightRenderer::begin() {
  pinMode(kFlashPin, OUTPUT);
  allOff();
}

void FlashlightRenderer::allOff() {
  digitalWrite(kFlashPin, LOW);
  delay(8);
}

void FlashlightRenderer::setCommand(const decaflash::NodeCommand& command) {
  (void)command;
}

void FlashlightRenderer::flash100(uint16_t flashMs) {
  sendFlashPreset(kPresetShort100);
  delay(flashMs);
  allOff();
}

void FlashlightRenderer::sendFlashPreset(uint8_t preset) {
  allOff();

  for (uint8_t i = 0; i < preset; ++i) {
    digitalWrite(kFlashPin, LOW);
    delayMicroseconds(4);
    digitalWrite(kFlashPin, HIGH);
    delayMicroseconds(4);
  }
}
