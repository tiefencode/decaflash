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

void NodeOutput::setNodeKind(decaflash::NodeKind nodeKind) {
  if (nodeKindInitialized_ && nodeKind_ == nodeKind) {
    return;
  }

  if (nodeKindInitialized_) {
    allOff();
  }

  nodeKind_ = nodeKind;
  nodeKindInitialized_ = true;

  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.begin();
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.begin();
      break;
  }
}

void NodeOutput::setCommand(const decaflash::NodeCommand& command) {
  switch (nodeKind_) {
    case decaflash::NodeKind::RgbStrip:
      rgbStrip_.setCommand(command);
      break;

    case decaflash::NodeKind::Flashlight:
    default:
      flashlight_.setCommand(command);
      break;
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

const char* NodeOutput::rendererName() const {
  return rendererNameFor(nodeKind_);
}
