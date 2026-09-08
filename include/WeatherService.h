#pragma once
#include <cstdint>

// Open-Meteo only, no API key. Runs its own FreeRTOS task (brief section
// 5: "must never stall the render loop") since HTTPClient's GET() is a
// blocking call. Reads are a mutex-protected snapshot copy, same pattern
// as MoodEngine's status snapshot in WebPortal.
namespace WeatherService {

// Overlay DisplayEngine should composite for the current conditions.
// ASSUMPTION: the exact WMO-code-to-overlay mapping (WeatherService.cpp)
// since the brief names the three overlays without giving the mapping.
enum class Overlay { NONE, RAIN, SUNGLASSES, SHIVER };

struct Reading {
  bool available = false;  // false until the first successful fetch ever
  bool stale = false;      // true once the last successful fetch is >1h old
  float temperatureC = 0.0f;
  int weatherCode = -1;
  float precipitationMm = 0.0f;
  Overlay overlay = Overlay::NONE;

  // --- Today's forecast, as opposed to `overlay`'s right-now conditions ---
  // `overlay` says what it is doing outside at this moment; these say what
  // the rest of today holds. Deliberately NOT folded into the Overlay enum:
  // sunny-now-with-rain-later is a real and interesting combination, and
  // one exclusive value could not express it.
  int rainChancePercent = -1;  // -1 until a forecast has been read
  float rainSumMm = 0.0f;
  bool rainExpectedToday = false;
};

void begin();

Reading current();

}  // namespace WeatherService
