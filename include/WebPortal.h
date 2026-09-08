#pragma once

#include "MoodEngine.h"

// Provisioning, dashboard, and JSON API. Per brief section 4 (hard
// requirement): AsyncWebServer handlers run on the AsyncTCP task, not the
// loop task that owns MoodEngine's per-second update() and DisplayEngine's
// LVGL calls. So handlers here never call MoodEngine/DisplayEngine
// directly -- they validate input and enqueue a Command, which
// WebPortal::loop() (called from the loop task, like DisplayEngine::loop())
// drains and applies. Status reads go through a small mutex-protected
// snapshot published by the loop task each cycle, not a live read of
// MoodEngine's internals. ConfigStore is separately thread-safe on its own
// (see ConfigStore.h) since it's read directly by GET/POST /api/config.
namespace WebPortal {

void begin();

// Call every loop() cycle from the same task that runs DisplayEngine::loop()
// and MoodEngine::update(). Drains the command queue (applying express/
// config-update/reboot) and services the Wi-Fi connect/reconnect/
// AP-fallback state machine. Never blocks.
void loop();

// Called by main.cpp once a second, right after MoodEngine::update(), so
// GET /api/status has something current to read without touching
// MoodEngine from the wrong task.
void publishMoodSnapshot(MoodEngine::State state, bool timeUnknown);

}  // namespace WebPortal
