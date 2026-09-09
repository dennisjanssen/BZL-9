// Stage 5: WeatherService, overlays, glitch outbursts. OTA landed in
// Stage 4 already. This wires MoodEngine's state and WeatherService's
// reading into DisplayEngine's rendering, and fades the backlight for
// SLEEPING per brief section 5.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include "ConfigStore.h"
#include "StatusLed.h"
#include "DisplayEngine.h"
#include "MoodEngine.h"
#include "WeatherService.h"
#include "WebPortal.h"

namespace {

uint32_t lastTickMs = 0;
constexpr uint32_t kTickIntervalMs = 1000;  // MoodEngine::update() contract: once/sec
constexpr uint32_t kSleepFadeMs = 2000;     // brief section 5: "over two seconds"

MoodEngine::State lastLoggedState = MoodEngine::State::NEUTRAL;
bool wasSleeping = false;

const char *stateName(MoodEngine::State s) {
  switch (s) {
    case MoodEngine::State::SLEEPING: return "SLEEPING";
    case MoodEngine::State::SLEEPY: return "SLEEPY";
    case MoodEngine::State::NEUTRAL: return "NEUTRAL";
    case MoodEngine::State::GLITCHED: return "GLITCHED";
    case MoodEngine::State::HYDRATION_REMINDER: return "HYDRATION_REMINDER";
    case MoodEngine::State::MOVEMENT_REMINDER: return "MOVEMENT_REMINDER";
  }
  return "?";
}

// Deliberately not using the core's getLocalTime() helper -- with no time
// synced yet it internally calls delay(10) once before giving up, which
// would violate the no-delay() rule. time()/localtime_r() directly is
// just as easy and fully non-blocking.
MoodEngine::TimeInput getTimeInput() {
  auto cfg = ConfigStore::get();
  time_t now;
  time(&now);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  MoodEngine::TimeInput t;
  t.timeKnown = tmNow.tm_year > (2016 - 1900);  // same heuristic esp32-hal-time.c uses
  if (t.timeKnown) {
    t.minutesSinceMidnight = static_cast<uint16_t>(tmNow.tm_hour * 60 + tmNow.tm_min);
    // ASSUMPTION: workday = Mon-Fri within [start,end]. The brief doesn't
    // say which weekdays count; weekends off is the obvious default for a
    // desk companion.
    bool isWeekday = tmNow.tm_wday >= 1 && tmNow.tm_wday <= 5;
    t.isWorkday = isWeekday && t.minutesSinceMidnight >= cfg.workdayStartMinutes &&
                  t.minutesSinceMidnight <= cfg.workdayEndMinutes;
  } else {
    t.minutesSinceMidnight = 0;
    t.isWorkday = false;
  }
  return t;
}

void tick() {
  MoodEngine::update(getTimeInput());

  auto state = MoodEngine::currentState();
  bool timeUnknown = MoodEngine::timeUnknown();
  WebPortal::publishMoodSnapshot(state, timeUnknown);

  DisplayEngine::applyMoodState(state);
  auto wx = WeatherService::current();
  DisplayEngine::applyWeatherOverlay(wx.overlay);
  // No point warning that rain is coming while it is already raining --
  // the RAIN overlay is already saying so, louder.
  DisplayEngine::applyRainForecast(wx.rainExpectedToday &&
                                   wx.overlay != WeatherService::Overlay::RAIN);

  bool sleeping = state == MoodEngine::State::SLEEPING;
  if (sleeping != wasSleeping) {
    auto cfg = ConfigStore::get();
    DisplayEngine::fadeBrightnessPercent(sleeping ? cfg.sleepBrightnessPercent : cfg.activeBrightnessPercent,
                                          kSleepFadeMs);
    wasSleeping = sleeping;
  }

  if (state != lastLoggedState) {
    Serial.printf("state=%s\n", stateName(state));
    lastLoggedState = state;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  uint32_t waitStart = millis();
  while (!Serial && millis() - waitStart < 3000) {
    // non-blocking wait for USB CDC enumeration
  }

  ConfigStore::begin();
  MoodEngine::init();
  DisplayEngine::init();
  StatusLed::begin();
  DisplayEngine::setBrightnessPercent(ConfigStore::get().activeBrightnessPercent);

  // Harmless to call before Wi-Fi is up -- SNTP just waits for a network.
  configTzTime(ConfigStore::get().timezone, "pool.ntp.org", "time.nist.gov");

  WebPortal::begin();
  WeatherService::begin();

  lastTickMs = millis();
}

void loop() {
  DisplayEngine::loop();
  WebPortal::loop();

  uint32_t now = millis();
  if (now - lastTickMs >= kTickIntervalMs) {
    lastTickMs = now;
    tick();
  }

  vTaskDelay(1);
}
