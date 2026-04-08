#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pdm_microphone.h"

namespace decaflash::brain::api_client {

void begin();
void service(uint32_t now);
bool busy();
void cancelAiWork();
bool queueCloudChattieInputToTextDisplay(const char* input);
bool queueRecordedAudioToTextDisplay(RecordedAudioClip& recording, bool aiOwned);
bool takeRecordedAudioCompletion(bool& processed);

}  // namespace decaflash::brain::api_client
