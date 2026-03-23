#pragma once

#include "decaflash_types.h"
#include "flashlight_renderer.h"
#include "rgb_strip_renderer.h"

class NodeOutput {
 public:
  void setNodeKind(decaflash::NodeKind nodeKind);
  void setCommand(const decaflash::NodeCommand& command);
  void allOff();
  void flash100(uint16_t flashMs);
  const char* rendererName() const;

 private:
  FlashlightRenderer flashlight_;
  RgbStripRenderer rgbStrip_;
  decaflash::NodeKind nodeKind_ = decaflash::NodeKind::Flashlight;
  bool nodeKindInitialized_ = false;
};
