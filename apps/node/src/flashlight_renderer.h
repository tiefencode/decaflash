#pragma once

#include <Arduino.h>

class FlashlightRenderer {
 public:
  explicit FlashlightRenderer(int flashPin);

  void begin();
  void allOff();
  void flash100(uint16_t flashMs);

 private:
  void sendFlashPreset(uint8_t preset);

  int flashPin_;

  static constexpr uint8_t kPresetShort100 = 1;
};
