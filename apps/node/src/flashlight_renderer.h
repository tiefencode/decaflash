#pragma once

#include <Arduino.h>

#include "decaflash_types.h"

class FlashlightRenderer {
 public:
  FlashlightRenderer() = default;

  void begin();
  void allOff();
  void setCommand(const decaflash::FlashCommand& command);
  void flash100(uint16_t flashMs);
  void setLit(bool lit);
  void service(uint32_t now);

 private:
  void sendFlashPreset(uint8_t preset);
  void setOutput(bool lit);

  static constexpr int kFlashPin = 26;
  static constexpr uint8_t kPresetShort100 = 1;
  bool outputLit_ = false;
};
