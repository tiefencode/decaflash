#pragma once

#include <stdint.h>

namespace decaflash::brain {

class PdmMicrophone;

namespace ai_mode {

uint32_t togglePressMs();
void toggle(uint32_t now, PdmMicrophone& microphone);
void service(uint32_t now, const PdmMicrophone& microphone);
bool blocksBeatDotOverlay(uint32_t now, const PdmMicrophone& microphone);
bool renderOverlay(uint32_t now, const PdmMicrophone& microphone);
void handleRecordingProcessed(uint32_t now, bool processed);
void handleWifiFailure(uint32_t now);
bool useAiMeterTheme(const PdmMicrophone& microphone);
bool ownsRecording();

}  // namespace ai_mode

}  // namespace decaflash::brain
