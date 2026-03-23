#pragma once

#include <Arduino.h>

#include "decaflash_types.h"

class RgbStripRenderer {
 public:
  RgbStripRenderer() = default;

  void begin();
  void allOff();
  void setCommand(const decaflash::NodeCommand& command);
  void flash100(uint16_t flashMs);

 private:
  struct Palette {
    uint8_t primaryR;
    uint8_t primaryG;
    uint8_t primaryB;
    uint8_t accentR;
    uint8_t accentG;
    uint8_t accentB;
  };

  Palette paletteForCommand(const decaflash::NodeCommand& command) const;
  void renderSolid(uint8_t red, uint8_t green, uint8_t blue);
  void runStartupProbe();
  void renderPulse();

  decaflash::NodeCommand currentCommand_ = {};
  uint16_t pulseIndex_ = 0;
  bool initialized_ = false;

  static constexpr uint8_t kLedCount = 15;
};
