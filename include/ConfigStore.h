#pragma once
#include <cstdint>
#include <cstring>

// Config lives in Preferences (NVS), not LittleFS -- so a filesystem/web-
// asset upload can never wipe it, and it can be read at boot before Wi-Fi
// comes up (WebPortal, Stage 4, depends on this; this module does not
// depend on WebPortal or WiFi at all).
//
// Every field has a documented default and a validated range. set*()
// functions return false and leave the field unchanged on an out-of-range
// value -- Stage 4's WebPortal is expected to turn that into an HTTP 400
// rather than clamp silently, per the brief.
namespace ConfigStore {

constexpr uint16_t kSchemaVersion = 5;  // v5: added onboard RGB LED fields

struct Config {
  uint16_t schemaVersion = kSchemaVersion;

  // --- Schedule ---
  uint16_t workdayStartMinutes = 540;   // 09:00. Range 0-1439.
  uint16_t workdayEndMinutes = 1050;    // 17:30. Range 0-1439, must be > start.
  char timezone[64] = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX TZ string.

  // --- Reminders ---
  uint16_t hydrationIntervalMinutes = 60;  // Range 5-480.
  uint16_t postureIntervalMinutes = 45;    // Range 5-480.

  // How long before the end of the workday the face starts getting
  // sleepy. Range 5-240. Default 60 -- the user asked for "the hour
  // before my workday ends"; it was a hardcoded 45 before this existed.
  uint16_t sleepyLeadMinutes = 60;

  // --- Location (for WeatherService, Stage 5) ---
  // ASSUMPTION: default coordinates are Brussels, Belgium -- a placeholder,
  // not a fact about the user's actual location. Must be set for real
  // weather data to make sense.
  float latitude = 50.8503f;   // Range -90..90.
  float longitude = 4.3517f;   // Range -180..180.
  uint16_t weatherPollIntervalMinutes = 15;  // Range 5-180, per brief section 5.

  // --- Display ---
  // Both capped at BACKLIGHT_MAX_SUSTAINED_PERCENT (Config.h) -- the vendor
  // wiki warns sustained full brightness can overheat this panel.
  uint8_t activeBrightnessPercent = 50;  // Range 1-50.
  uint8_t sleepBrightnessPercent = 5;    // Range 0-50.
  // Which way up the panel is mounted in the helmet. false = LCD_ROTATION,
  // true = the same landscape view rotated 180 degrees. Default false is a
  // guess about the enclosure, not a fact -- it is exactly what this field
  // exists to let the user correct. Any bool is valid.
  bool displayFlipped = false;

  // --- Onboard RGB LED (WS2812 on RGB_LED_PIN) ---
  // ASSUMPTION: defaults to OFF. This LED was never driven before the
  // portal could set it, so off matches the behaviour the user already
  // has, and StatusLed writes black at boot so its state is at least
  // defined (a WS2812 latches its last colour indefinitely).
  // ASSUMPTION: default colour is a dim white. These parts are extremely
  // bright at full scale -- 255,255,255 on a desk ornament is a torch --
  // so the default is somewhere you would actually leave it. Any 0-255
  // value is valid; brightness is carried by the channel values
  // themselves rather than a separate field.
  bool ledEnabled = false;
  uint8_t ledR = 50;
  uint8_t ledG = 50;
  uint8_t ledB = 50;

  // --- MoodEngine tuning ---
  // ASSUMPTION: brief says GLITCHED is "random" without giving a rate.
  // Expressed as an average interval rather than a raw per-second
  // probability so it reads sensibly as a config field.
  uint16_t glitchAverageIntervalMinutes = 10;  // Range 1-120.

  // --- Network / OTA (unused until Stage 4, schema owns them from the start) ---
  char wifiSsid[33] = "";
  char wifiPassword[65] = "";
  // NVS-only; WebPortal must never echo this back in any GET response.
  // ASSUMPTION: defaults to a known placeholder rather than empty --
  // leaving it blank would mean an *unauthenticated* /update endpoint out
  // of the box, which undercuts "gate it behind a password". Change this
  // via the portal before relying on it for anything.
  char otaPassword[65] = "bzl9-setup";

  bool operator==(const Config &o) const { return memcmp(this, &o, sizeof(Config)) == 0; }
};

// Loads from NVS, applying documented defaults for any missing key and
// running schema migrations if the stored version is older than current.
// Safe to call on corrupt/empty NVS -- falls back to all defaults.
void begin();

// Returns a snapshot copy, not a reference -- ConfigStore is internally
// mutex-protected (get()/set*() may be called from any task, in
// particular WebPortal's AsyncTCP task) per the brief's section 4
// requirement that web handlers never reach into shared mutable state
// directly.
Config get();

// Each returns false (config unchanged) if the value is out of its
// documented range above.
bool setWorkdayStartMinutes(uint16_t v);
bool setWorkdayEndMinutes(uint16_t v);
bool setTimezone(const char *v);
bool setHydrationIntervalMinutes(uint16_t v);
bool setPostureIntervalMinutes(uint16_t v);
bool setSleepyLeadMinutes(uint16_t v);
bool setLatitude(float v);
bool setLongitude(float v);
bool setWeatherPollIntervalMinutes(uint16_t v);
bool setActiveBrightnessPercent(uint8_t v);
bool setSleepBrightnessPercent(uint8_t v);
bool setDisplayFlipped(bool v);
// Set as a group: the portal always sends colour and on/off together, and
// applying them one at a time would flash intermediate colours.
bool setLed(bool enabled, uint8_t r, uint8_t g, uint8_t b);
bool setGlitchAverageIntervalMinutes(uint16_t v);
bool setWifiCredentials(const char *ssid, const char *password);
bool setOtaPassword(const char *v);

}  // namespace ConfigStore
