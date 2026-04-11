#pragma once

#include "text_playback.h"

namespace decaflash::brain::node_text {

bool start(const char* text, text_playback::Owner owner = text_playback::Owner::Manual);
void stop();
bool stopAiOwned();

}  // namespace decaflash::brain::node_text
