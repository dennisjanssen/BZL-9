#pragma once
#include <cstdint>

#include "MoodEngine.h"
#include "WeatherService.h"

// Stage 5: overlays. All LVGL calls happen inside DisplayEngine -- init(),
// loop(), and these apply*/trigger* entry points are the only ways in, and
// all of them are meant to be called from the loop task (see section 4 of
// the brief: LVGL is not thread-safe). WebPortal already respects this --
// triggerExpression() is called from its command-queue drain, which runs
// on the loop task, not from an AsyncTCP handler directly.
namespace DisplayEngine {

void init();
void loop();

// Flips the panel 180 degrees for the other mounting orientation. Safe at
// runtime: both rotations are the same landscape shape, so only the scan
// mapping changes. Must be called from the loop task like everything else
// here.
void setDisplayFlipped(bool flipped);

// Perceptual brightness curve, 0-100, instant. Internally clamps to
// BACKLIGHT_MAX_SUSTAINED_PERCENT (Config.h).
void setBrightnessPercent(uint8_t percent);

// Same, but eased over durationMs -- used for the SLEEPING fade (brief:
// "fades to the configured sleep brightness over two seconds").
void fadeBrightnessPercent(uint8_t percent, uint32_t durationMs);

// Called once a second from main.cpp with MoodEngine's current effective
// state (base mood or an active reminder/glitch overlay).
void applyMoodState(MoodEngine::State state);

// Today's rain forecast, as opposed to current conditions. Drives an
// occasional cameo (a raincloud drifts in and the face glances up at it)
// rather than a sustained overlay -- it is a heads-up, not the weather.
void applyRainForecast(bool expected);

// Called whenever WeatherService's overlay changes. Lower priority than
// mood state -- suppressed while a mood overlay (reminder/glitch) is
// active, resumes once it clears.
void applyWeatherOverlay(WeatherService::Overlay overlay);

// Pins a weather overlay for durationMs so the portal can demonstrate one
// without waiting for the sky to co-operate. The real reading keeps being
// tracked underneath and takes over again when the simulation expires;
// Overlay::NONE ends it immediately. Loop task only, like the rest.
void simulateWeatherOverlay(WeatherService::Overlay overlay, uint32_t durationMs);

// One rain-forecast cameo, now. A one-shot: unlike simulateWeatherOverlay
// it changes no state at all, because the forecast cameo is already a
// one-shot rather than a sustained overlay.
void playRainForecastCameo();

// One-shot manual expression from the portal's /api/express. Valid values
// match the brief's set: "shock", "heart", "rage", "sleepy", "glitch",
// "hydrate". Unknown values are ignored (WebPortal already validates
// before this is ever called).
void triggerExpression(const char *expression);

}  // namespace DisplayEngine
