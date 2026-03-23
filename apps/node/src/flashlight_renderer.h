#pragma once

#include <Arduino.h>

#include "decaflash_types.h"

class FlashlightRenderer {
 public:
  FlashlightRenderer() = default;

  void begin();
  void allOff();
  void setCommand(const decaflash::NodeCommand& command);
  void flash100(uint16_t flashMs);

 private:
  void sendFlashPreset(uint8_t preset);

  static constexpr int kFlashPin = 26;

  static constexpr uint8_t kPresetShort100 = 1;
};
