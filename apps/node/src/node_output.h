#pragma once

#include "decaflash_types.h"
#include "flashlight_renderer.h"
#include "rgb_strip_renderer.h"
#include "status_led.h"

class NodeOutput {
 public:
  void setNodeProfile(decaflash::NodeKind nodeKind, decaflash::NodeEffect nodeEffect);
  void setFlashCommand(const decaflash::FlashCommand& command);
  void setRgbCommand(const decaflash::RgbCommand& command);
  void showRoleConfirm(decaflash::NodeEffect nodeEffect);
  void triggerRgbAccent();
  void syncBeatClock(
    uint32_t now,
    uint32_t beatIntervalMs,
    uint8_t beatsPerBar,
    uint8_t beatInBar,
    uint32_t currentBar
  );
  void allOff();
  void flash100(uint16_t flashMs);
  void service(uint32_t now);
  const char* rendererName() const;

 private:
  FlashlightRenderer flashlight_;
  RgbStripRenderer rgbStrip_;
  StatusLed statusLed_;
  decaflash::NodeKind nodeKind_ = decaflash::NodeKind::Flashlight;
  decaflash::NodeEffect nodeEffect_ = decaflash::NodeEffect::Pulse;
  bool nodeKindInitialized_ = false;
};
