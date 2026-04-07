#pragma once

#include <stddef.h>
#include <stdint.h>

namespace decaflash::brain::api_client {

bool fetchCloudChattieInputToTextDisplay(const char* input);
bool processRecordedAudioToTextDisplay(const int16_t* samples,
                                       size_t sampleCount,
                                       uint32_t sampleRateHz);

}  // namespace decaflash::brain::api_client
