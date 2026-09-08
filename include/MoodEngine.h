#pragma once
#include <cstdint>

// State is a pure function of the schedule (plus reminder intervals and
// random glitch timing), evaluated once per second. Reads ConfigStore
// directly for its tunables -- call ConfigStore::begin() before
// MoodEngine::init().
//
// Day moods (NEUTRAL/BORED/EXCITED/FOCUSED) are drawn at random and held
// for kMoodHold* minutes at a time; SLEEPY takes over in the configurable
// window before the workday ends, and SLEEPING outside workday hours.
//
// REVISED: originally also tracked three decaying metrics (Energy,
// Attention, Fun) driving BORED/HAPPY states and "Pet"/"Pay attention"
// interactions, Tamagotchi-style. Removed at the user's request -- the
// draining-meters/needs-attention mechanic was a distraction rather than
// a useful desk-companion behavior. What's left is purely schedule- and
// interval-driven: no metric to feed, nothing that demands upkeep.
namespace MoodEngine {

enum class State {
  SLEEPING,
  SLEEPY,
  // --- Randomly scheduled day moods ---
  // These are NOT a return of the removed Tamagotchi metric system: there
  // is nothing to feed, nothing draining, and nothing the user has to
  // attend to. They are picked at random and held for a few minutes so
  // the face isn't the same face all day.
  NEUTRAL,
  BORED,
  EXCITED,
  FOCUSED,
  // --- Transient overlays (priority over the above) ---
  GLITCHED,
  HYDRATION_REMINDER,
  POSTURE_REMINDER,
};

struct TimeInput {
  bool timeKnown = false;             // false => clock has never synced
  bool isWorkday = false;             // caller-computed; MoodEngine does not
                                       // re-derive this from minutesSinceMidnight
  uint16_t minutesSinceMidnight = 0;  // 0-1439
};

void init();

// Forces a mood, overriding the random rotation -- driven by the portal's
// mood buttons. Accepts the four day moods plus SLEEPY (which is normally
// schedule-driven but is offered in the picker too); anything else,
// SLEEPING included, is ignored. The schedule still wins: once it says
// SLEEPY or SLEEPING that takes over regardless, and the override is
// dropped at that point, so a forgotten one can't outlive the day.
void setDayMoodOverride(State mood);
void clearDayMoodOverride();
bool dayMoodOverridden();


// Call exactly once per second -- reminder/glitch timing assumes a fixed
// 1s step rather than measuring elapsed time itself.
void update(const TimeInput &time);

// Effective state: an active reminder/glitch overlay takes priority over
// the base state (SLEEPING/SLEEPY/NEUTRAL) but does not replace it -- the
// base state keeps evaluating underneath and resumes once the overlay
// ends. Safe to call more often than update() (e.g. from DisplayEngine's
// render loop); overlay expiry is checked against the real clock
// (millis()) independent of the 1s update() cadence, so a short glitch
// overlay reads as generally under a second on-screen even though state
// is only recomputed once a second.
State currentState();

// True when time has never synced. Per the brief: never enter SLEEPING
// while this is true (stay NEUTRAL instead) -- DisplayEngine should show a
// small indicator.
bool timeUnknown();

}  // namespace MoodEngine
