#include "StatusLed.h"

#include <Arduino.h>
#include <soc/soc_caps.h>

#include "Config.h"
#include "ConfigStore.h"

// The core guards rgbLedWrite()'s entire body behind `#if SOC_RMT_SUPPORTED`
// -- so on a target without RMT it compiles to a no-op and the LED would
// silently never light, with nothing in the build to say why. Fail loudly
// instead if that ever changes.
#if !SOC_RMT_SUPPORTED
#error "StatusLed needs RMT: rgbLedWrite() is a no-op without SOC_RMT_SUPPORTED"
#endif

namespace StatusLed {

void apply(bool enabled, uint8_t r, uint8_t g, uint8_t b) {
  // rgbLedWriteOrdered() (not the deprecated neopixelWrite()) handles RMT
  // setup itself on each call -- there is no separate init to do.
  // RGB_LED_PIN is passed as the raw GPIO, not as RGB_BUILTIN, so the
  // core's builtin-pin remapping leaves it alone.
  //
  // The order is stated EXPLICITLY as RGB rather than relying on
  // rgbLedWrite(), whose default is GRB (the usual WS2812B wire order).
  // On this board that default came out with red and green swapped --
  // green showed red and red showed green, with blue correct, which is
  // exactly the GRB-vs-RGB signature since blue is the third byte either
  // way. Determined on the physical LED, not from a datasheet: treat it
  // as a fact about this board and do not "simplify" it back to
  // rgbLedWrite().
  if (enabled) {
    rgbLedWriteOrdered(RGB_LED_PIN, LED_COLOR_ORDER_RGB, r, g, b);
  } else {
    rgbLedWriteOrdered(RGB_LED_PIN, LED_COLOR_ORDER_RGB, 0, 0, 0);
  }
}

void begin() {
  auto cfg = ConfigStore::get();
  // Written unconditionally, including when disabled: the LED has never
  // been driven before this module existed, so its power-on state is not
  // something we know. Writing black gives it a defined one.
  apply(cfg.ledEnabled, cfg.ledR, cfg.ledG, cfg.ledB);
}

}  // namespace StatusLed
