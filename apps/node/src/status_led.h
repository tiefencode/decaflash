#pragma once

#include "decaflash_types.h"

class StatusLed {
 public:
  void begin();
  void showRoleConfirm(decaflash::NodeEffect nodeEffect);

 private:
  bool initialized_ = false;
};
