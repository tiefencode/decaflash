#include "flashlight_renderer.h"

FlashlightRenderer::FlashlightRenderer(int flashPin) : flashPin_(flashPin) {}

void FlashlightRenderer::begin() {
  pinMode(flashPin_, OUTPUT);
  allOff();
}

void FlashlightRenderer::allOff() {
  digitalWrite(flashPin_, LOW);
  delay(8);
}

void FlashlightRenderer::flash100(uint16_t flashMs) {
  sendFlashPreset(kPresetShort100);
  delay(flashMs);
  allOff();
}

void FlashlightRenderer::sendFlashPreset(uint8_t preset) {
  allOff();

  for (uint8_t i = 0; i < preset; ++i) {
    digitalWrite(flashPin_, LOW);
    delayMicroseconds(4);
    digitalWrite(flashPin_, HIGH);
    delayMicroseconds(4);
  }
}
