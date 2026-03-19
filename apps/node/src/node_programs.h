#pragma once

#include "command_examples.h"

namespace decaflash::node {

static constexpr const NodeCommand* kPrograms = decaflash::examples::kFlashCommands;
static constexpr size_t kProgramCount = decaflash::examples::kFlashCommandCount;

}  // namespace decaflash::node
