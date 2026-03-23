#pragma once

#include "command_examples.h"

namespace decaflash::node {

struct ProgramSet {
  const NodeCommand* programs;
  size_t count;
};

inline ProgramSet programSetFor(NodeKind nodeKind) {
  switch (nodeKind) {
    case NodeKind::RgbStrip:
      return {decaflash::examples::kRgbCommands, decaflash::examples::kRgbCommandCount};

    case NodeKind::Flashlight:
    default:
      return {decaflash::examples::kFlashCommands, decaflash::examples::kFlashCommandCount};
  }
}

}  // namespace decaflash::node
