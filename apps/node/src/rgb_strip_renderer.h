#pragma once

#include <Arduino.h>

#include "decaflash_types.h"

struct SurfaceModulationState {
  bool active = false;
  uint8_t activity = 0;
  uint8_t shadowDepth = 0;
  uint8_t pocketChance = 0;
  uint8_t coolShift = 0;
  uint8_t colorDrift = 0;
};

class RgbStripRenderer {
 public:
  RgbStripRenderer() = default;

  void begin();
  void allOff();
  void setNodeEffect(decaflash::NodeEffect nodeEffect);
  void setCommand(const decaflash::RgbCommand& command);
  void flash100(uint16_t flashMs);
  void setLit(bool lit);
  void triggerPulseRow();
  void syncBeatClock(
    uint32_t now,
    uint32_t beatIntervalMs,
    uint8_t beatsPerBar,
    uint8_t beatInBar,
    uint32_t currentBar
  );
  void service(uint32_t now);
  SurfaceModulationState surfaceModulationState(uint32_t now) const;

 private:
  void renderSolid(uint8_t red, uint8_t green, uint8_t blue);
  void renderWave(uint32_t now);
  void renderPulse(uint32_t now);
  void renderPulseRow(uint32_t now);
  void renderHeartbeat(uint32_t now);
  void renderRiserPulse(uint32_t now);
  void renderRunner(uint32_t now);
  void applySurfaceModulation(uint32_t now);
  decaflash::NodeEffect nodeEffect_ = decaflash::NodeEffect::Pulse;
  decaflash::RgbCommand currentCommand_ = {};
  uint32_t effectStartedAtMs_ = 0;
  uint32_t pulseRowStartedAtMs_ = 0;
  uint32_t pulseRowEndsAtMs_ = 0;
  uint32_t beatStartedAtMs_ = 0;
  uint32_t beatIntervalMs_ = 500;
  uint8_t beatsPerBar_ = 4;
  uint8_t beatInBar_ = 1;
  uint32_t currentBar_ = 1;
  bool initialized_ = false;

  static constexpr uint8_t kLedCount = 15;
};
