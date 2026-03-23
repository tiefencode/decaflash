#include "node_output.h"

namespace {

const char* rendererNameFor(decaflash::NodeKind nodeKind) {
  switch (nodeKind) {
    case decaflash::NodeKind::RgbStrip:
      return "rgb_strip";

    case decaflash::NodeKind::Flashlight:
    default:
      return "flash";
  }
}

}  // namespace

void NodeOutput::setNodeProfile(decaflash::NodeKind nodeKind, decaflash::NodeEffect nodeEffect) {
  statusLed_.begin();

  const bool sameProfile =
    nodeKindInitialized_ && nodeKind_ == nodeKind && nodeEffect_ == nodeEffect;
  if (sameProfile) {
    return;
  }

  if (nodeKindInitialized_ && nodeKind_ != nodeKind) {
    allOff();
  }

  nodeKind_ = nodeKind;
  nodeEffect_ = nodeEffect;
  nodeKindInitialized_ = true;

  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.begin();
      rgbStrip_.setNodeEffect(nodeEffect_);
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.begin();
      break;
  }
}

void NodeOutput::setFlashCommand(const decaflash::FlashCommand& command) {
  flashlight_.setCommand(command);
}

void NodeOutput::setRgbCommand(const decaflash::RgbCommand& command) {
  rgbStrip_.setNodeEffect(nodeEffect_);
  rgbStrip_.setCommand(command);
}

void NodeOutput::showRoleConfirm(decaflash::NodeEffect nodeEffect) {
  statusLed_.showRoleConfirm(nodeEffect);
}

void NodeOutput::triggerRgbAccent() {
  if (nodeKind_ == decaflash::NodeKind::RgbStrip) {
    rgbStrip_.triggerAccent();
  }
}

void NodeOutput::syncBeatClock(
  uint32_t now,
  uint32_t beatIntervalMs,
  uint8_t beatsPerBar,
  uint8_t beatInBar,
  uint32_t currentBar
) {
  if (nodeKind_ == decaflash::NodeKind::RgbStrip) {
    rgbStrip_.syncBeatClock(now, beatIntervalMs, beatsPerBar, beatInBar, currentBar);
  }
}

void NodeOutput::allOff() {
  if (!nodeKindInitialized_) {
    return;
  }

  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.allOff();
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.allOff();
      break;
  }
}

void NodeOutput::flash100(uint16_t flashMs) {
  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.flash100(flashMs);
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.flash100(flashMs);
      break;
  }
}

void NodeOutput::service(uint32_t now) {
  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.service(now);
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.service(now);
      break;
  }
}

const char* NodeOutput::rendererName() const {
  return rendererNameFor(nodeKind_);
}
