#include "MoodEngine.h"

#include <Arduino.h>
#include <algorithm>

#include "ConfigStore.h"

namespace MoodEngine {

namespace {

State baseState = State::NEUTRAL;
bool lastTimeKnown = false;

enum class Overlay { NONE, GLITCHED, HYDRATION, POSTURE };
Overlay overlay = Overlay::NONE;
uint32_t overlayEndMs = 0;

uint32_t hydrationElapsedMs = 0;
uint32_t postureElapsedMs = 0;

// --- Random day mood ---
// ASSUMPTION: the user asked for moods held "for a few minutes on end"
// without naming a duration, so this is a documented range rather than a
// config field. Weights favour NEUTRAL so the other three read as
// occasional character rather than constant mugging.
constexpr uint32_t kMoodHoldMinMs = 3UL * 60000UL;
constexpr uint32_t kMoodHoldMaxMs = 10UL * 60000UL;

State dayMood = State::NEUTRAL;
uint32_t moodRemainingMs = 0;

bool moodOverride = false;
State moodOverrideState = State::NEUTRAL;

// SLEEPY is selectable even though it is normally schedule-driven -- the
// user asked for it in the mood picker. SLEEPING is deliberately not:
// there is no way back from it except the clock.
bool isSelectableMood(State s) {
  return s == State::NEUTRAL || s == State::BORED || s == State::EXCITED ||
         s == State::FOCUSED || s == State::SLEEPY;
}

// Remembered across ticks purely for the SLEEPY hysteresis. Tracks what
// the SCHEDULE thinks, never what a manual override asked for.
bool scheduleSleepy = false;

State pickDayMood() {
  long roll = random(100);
  if (roll < 40) return State::NEUTRAL;
  if (roll < 62) return State::BORED;
  if (roll < 82) return State::EXCITED;
  return State::FOCUSED;
}

void rerollDayMood() {
  // Never hand back the mood that just ended -- repeating it would look
  // like the mood timer had stalled rather than rolled.
  State next = pickDayMood();
  if (next == dayMood) {
    next = pickDayMood();
    if (next == dayMood) next = (dayMood == State::NEUTRAL) ? State::BORED : State::NEUTRAL;
  }
  dayMood = next;
  moodRemainingMs =
      kMoodHoldMinMs + static_cast<uint32_t>(random(static_cast<long>(kMoodHoldMaxMs - kMoodHoldMinMs)));
}

void tickDayMood(bool awake) {
  if (!awake) {
    // Asleep or winding down: drop the held mood so a fresh one is drawn
    // on waking, rather than resuming a mood chosen yesterday. A manual
    // override goes with it -- otherwise one set on a Friday afternoon
    // would still be stuck there on Monday.
    moodRemainingMs = 0;
    dayMood = State::NEUTRAL;
    moodOverride = false;
    return;
  }
  if (moodOverride) {
    dayMood = moodOverrideState;
    return;
  }
  if (moodRemainingMs <= 1000) {
    rerollDayMood();
    return;
  }
  moodRemainingMs -= 1000;
}

// ASSUMPTION: the brief specifies glitch's on-screen duration (200-800ms,
// DisplayEngine section) but not how long a hydration/posture reminder
// stays up. 20s felt long enough to notice and read, short enough not to
// dominate. Not exposed via ConfigStore -- the brief only names the
// interval *between* reminders as a portal-configurable field, not this.
constexpr uint32_t kReminderDurationMs = 20000;

State overlayToState(Overlay o) {
  switch (o) {
    case Overlay::GLITCHED: return State::GLITCHED;
    case Overlay::HYDRATION: return State::HYDRATION_REMINDER;
    case Overlay::POSTURE: return State::POSTURE_REMINDER;
    default: return State::NEUTRAL;  // unreachable when o != NONE
  }
}

void refreshOverlayExpiry() {
  if (overlay != Overlay::NONE && millis() >= overlayEndMs) {
    overlay = Overlay::NONE;
  }
}

// 5-point hysteresis on the "within sleepyLeadMinutes of workday end"
// threshold, per brief section 5 -- widens (easier to stay, harder to
// re-enter) only while already SLEEPY. The lead time is configurable now;
// it used to be a hardcoded 45.
// The schedule's verdict ALONE, with no mood involved: SLEEPING, SLEEPY,
// or NEUTRAL meaning "no opinion, the day mood decides".
//
// Kept separate from the mood on purpose. The hysteresis used to test
// `baseState == SLEEPY`, and the mood-roll used to test the same -- which
// works only while SLEEPY can arrive from the clock. Now that it is also
// a pickable mood, a manual SLEEPY would have made update() conclude the
// device was winding down and clear the very override that set it, one
// second after it was chosen.
State computeScheduleState(const TimeInput &time) {
  if (!time.timeKnown) {
    scheduleSleepy = false;
    return State::NEUTRAL;
  }
  if (!time.isWorkday) {
    scheduleSleepy = false;
    return State::SLEEPING;
  }

  const auto &cfg = ConfigStore::get();
  int minutesUntilEnd =
      static_cast<int>(cfg.workdayEndMinutes) - static_cast<int>(time.minutesSinceMidnight);
  int lead = static_cast<int>(cfg.sleepyLeadMinutes);
  bool nearEnd = scheduleSleepy ? (minutesUntilEnd <= lead + 5) : (minutesUntilEnd <= lead);
  // minutesUntilEnd goes negative once the workday is over; isWorkday is
  // what actually ends the day, so guard against a stale negative window
  // reading as "nearly done" all evening.
  scheduleSleepy = nearEnd && minutesUntilEnd >= 0;
  return scheduleSleepy ? State::SLEEPY : State::NEUTRAL;
}

void maybeStartOverlay(const TimeInput &time) {
  // No reminders/glitches while SLEEPING, and never stack one overlay on
  // another.
  if (overlay != Overlay::NONE || baseState == State::SLEEPING) return;
  if (baseState == State::SLEEPY) return;  // let the wind-down read as a wind-down
  if (!time.timeKnown || !time.isWorkday) return;

  const auto &cfg = ConfigStore::get();

  // Priority when multiple are due in the same tick (ASSUMPTION, brief
  // asks this be defined explicitly): POSTURE > HYDRATION > GLITCHED.
  // Health-nudge reminders matter more than a cosmetic glitch, and a
  // missed reminder simply fires on the very next tick once the other
  // overlay clears -- neither one is dropped.
  if (postureElapsedMs >= static_cast<uint32_t>(cfg.postureIntervalMinutes) * 60000UL) {
    overlay = Overlay::POSTURE;
    overlayEndMs = millis() + kReminderDurationMs;
    postureElapsedMs = 0;
    return;
  }
  if (hydrationElapsedMs >= static_cast<uint32_t>(cfg.hydrationIntervalMinutes) * 60000UL) {
    overlay = Overlay::HYDRATION;
    overlayEndMs = millis() + kReminderDurationMs;
    hydrationElapsedMs = 0;
    return;
  }

  uint32_t avgIntervalSec = static_cast<uint32_t>(cfg.glitchAverageIntervalMinutes) * 60UL;
  if (avgIntervalSec > 0 && random(static_cast<long>(avgIntervalSec)) == 0) {
    overlay = Overlay::GLITCHED;
    overlayEndMs = millis() + random(200, 801);
  }
}

}  // namespace

void init() {
  baseState = State::NEUTRAL;
  overlay = Overlay::NONE;
  lastTimeKnown = false;
  hydrationElapsedMs = 0;
  postureElapsedMs = 0;
  dayMood = State::NEUTRAL;
  moodRemainingMs = 0;
  moodOverride = false;
}

void update(const TimeInput &time) {
  lastTimeKnown = time.timeKnown;

  if (baseState != State::SLEEPING && baseState != State::SLEEPY) {
    hydrationElapsedMs += 1000;
    postureElapsedMs += 1000;
  }

  State sched = computeScheduleState(time);
  bool scheduleHolds = (sched == State::SLEEPING || sched == State::SLEEPY);

  // Roll moods only while the SCHEDULE is not holding the face -- rolling
  // during real sleep would just burn holds nobody can see. Tested against
  // the schedule rather than baseState so that a manually picked SLEEPY
  // does not read as "winding down" and clear itself.
  tickDayMood(!scheduleHolds);

  maybeStartOverlay(time);
  refreshOverlayExpiry();

  baseState = scheduleHolds ? sched : dayMood;
}

void setDayMoodOverride(State mood) {
  if (!isSelectableMood(mood)) return;
  moodOverride = true;
  moodOverrideState = mood;
  dayMood = mood;
  moodRemainingMs = 0;
}

void clearDayMoodOverride() {
  moodOverride = false;
  moodRemainingMs = 0;  // reroll on the next tick rather than finishing the held one
}

bool dayMoodOverridden() { return moodOverride; }

State currentState() {
  refreshOverlayExpiry();
  return overlay == Overlay::NONE ? baseState : overlayToState(overlay);
}

bool timeUnknown() { return !lastTimeKnown; }

}  // namespace MoodEngine
