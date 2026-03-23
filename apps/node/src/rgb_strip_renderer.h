#pragma once

#include <Arduino.h>

#include "decaflash_types.h"

class RgbStripRenderer {
 public:
  RgbStripRenderer() = default;

  void begin();
  void allOff();
  void setNodeEffect(decaflash::NodeEffect nodeEffect);
  void setCommand(const decaflash::RgbCommand& command);
  void flash100(uint16_t flashMs);
  void triggerAccent();
  void service(uint32_t now);

 private:
  void renderSolid(uint8_t red, uint8_t green, uint8_t blue);
  void renderBreathe(uint32_t now);
  void renderBeatPulse(uint32_t now);
  void renderAccent(uint32_t now);
  void renderRunnerFlicker(uint32_t now);
  uint8_t breatheLevel(uint32_t now, uint8_t low, uint8_t high) const;
  uint8_t accentLevel(uint32_t now, uint8_t low, uint8_t high) const;
  uint8_t clampLevel(uint16_t level) const;
  void runStartupProbe();

  decaflash::NodeEffect nodeEffect_ = decaflash::NodeEffect::Pulse;
  decaflash::RgbCommand currentCommand_ = {};
  uint32_t effectStartedAtMs_ = 0;
  uint32_t accentStartedAtMs_ = 0;
  uint32_t accentEndsAtMs_ = 0;
  bool initialized_ = false;

  static constexpr uint8_t kLedCount = 15;
};
