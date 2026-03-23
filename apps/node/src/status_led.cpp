#include "status_led.h"

#include <FastLED.h>

namespace {

static constexpr uint8_t kStatusLedPin = 27;
static constexpr uint8_t kStatusLedCount = 1;
static constexpr uint8_t kConfirmPulseCount = 2;
static constexpr uint16_t kConfirmOnMs = 110;
static constexpr uint16_t kConfirmOffMs = 70;

CRGB gStatusLed[kStatusLedCount];

CRGB roleColor(decaflash::NodeEffect nodeEffect) {
  switch (nodeEffect) {
    case decaflash::NodeEffect::Wash:
      return CRGB(0, 32, 255);

    case decaflash::NodeEffect::Pulse:
      return CRGB(32, 0, 255);

    case decaflash::NodeEffect::Accent:
      return CRGB(255, 0, 0);

    case decaflash::NodeEffect::Flicker:
      return CRGB(255, 0, 160);

    case decaflash::NodeEffect::None:
    default:
      return CRGB::White;
  }
}

void showColor(const CRGB& color) {
  gStatusLed[0] = color;
  FastLED.show();
}

}  // namespace

void StatusLed::begin() {
  if (initialized_) {
    return;
  }

  FastLED.addLeds<NEOPIXEL, kStatusLedPin>(gStatusLed, kStatusLedCount);
  showColor(CRGB::Black);
  initialized_ = true;
}

void StatusLed::showRoleConfirm(decaflash::NodeEffect nodeEffect) {
  if (!initialized_) {
    begin();
  }

  const CRGB color = roleColor(nodeEffect);
  for (uint8_t i = 0; i < kConfirmPulseCount; ++i) {
    showColor(color);
    delay(kConfirmOnMs);
    showColor(CRGB::Black);
    if (i + 1 < kConfirmPulseCount) {
      delay(kConfirmOffMs);
    }
  }
}
