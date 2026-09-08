#pragma once
#include <cstdint>

// The board's single onboard WS2812 (Config.h RGB_LED_PIN = 8, which the
// Arduino C6 variant corroborates as PIN_RGB_LED). Colour and on/off come
// from ConfigStore and are set from the portal.
//
// Call only from the loop task, and only occasionally: the core's
// rgbLedWrite() re-initialises an RMT channel on every call, so this is
// for boot and config changes -- it is not an animation API.
namespace StatusLed {

// Applies whatever ConfigStore currently holds. Safe to call before Wi-Fi
// is up; needs ConfigStore::begin() to have run.
void begin();

// Writes the LED directly. `enabled == false` writes black rather than
// leaving the data line idle, because a WS2812 latches its last colour --
// "off" is a colour you have to send.
void apply(bool enabled, uint8_t r, uint8_t g, uint8_t b);

}  // namespace StatusLed
