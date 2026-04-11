#include "flashlight_renderer.h"

void FlashlightRenderer::begin() {
  pinMode(kFlashPin, OUTPUT);
  allOff();
}

void FlashlightRenderer::setOutput(bool lit) {
  if (lit) {
    sendFlashPreset(kPresetShort100);
  } else {
    digitalWrite(kFlashPin, LOW);
  }

  outputLit_ = lit;
}

void FlashlightRenderer::allOff() {
  setOutput(false);
  delay(8);
}

void FlashlightRenderer::setCommand(const decaflash::FlashCommand& command) {
  (void)command;
}

void FlashlightRenderer::flash100(uint16_t flashMs) {
  setOutput(true);
  delay(flashMs);
  allOff();
}

void FlashlightRenderer::setLit(bool lit) {
  if (lit == outputLit_) {
    return;
  }

  setOutput(lit);
}

void FlashlightRenderer::service(uint32_t now) {
  (void)now;
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
