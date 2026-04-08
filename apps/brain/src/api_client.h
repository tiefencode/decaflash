#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::api_client {

void begin();
void service(uint32_t now);
bool busy();
void cancelAiWork();
bool queueCloudChattieInputToTextDisplay(const char* input);
bool queueRecordedAudioToTextDisplay(int16_t* samples,
                                     size_t sampleCount,
                                     uint32_t sampleRateHz,
                                     bool aiOwned);
bool takeRecordedAudioCompletion(bool& processed);

}  // namespace decaflash::brain::api_client
