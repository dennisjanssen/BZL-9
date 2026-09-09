#include "ConfigStore.h"

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "Config.h"

namespace ConfigStore {

namespace {
Preferences prefs;
Config current;
SemaphoreHandle_t mutex = nullptr;

// The NVS namespace every stored setting lives under. Renamed from "kvrc"
// with the project; the user accepted re-provisioning over the AP rather
// than writing a key-copying migration, so anything stored under the old
// name is simply abandoned (and erased -- see kLegacyNamespace below).
// NVS namespace names are capped at 15 characters.
constexpr const char *kNamespace = "bzl9";

// The pre-rename namespace. Its contents are dead but would otherwise sit
// in NVS forever taking up entries, so begin() erases it once. Safe to
// delete this and the eraseLegacyNamespace() call together once no device
// is likely to still be carrying pre-rename storage.
constexpr const char *kLegacyNamespace = "kvrc";

// RAII guard so every public function below just declares one of these
// instead of manually pairing take/give at each return point.
struct LockGuard {
  LockGuard() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~LockGuard() { xSemaphoreGive(mutex); }
};

void loadAll() {
  current.schemaVersion = prefs.getUShort("schemaVer", kSchemaVersion);

  current.scheduleEnabled = prefs.getBool("schedOn", true);
  current.workdayStartMinutes = prefs.getUShort("wdStart", 540);
  current.workdayEndMinutes = prefs.getUShort("wdEnd", 1050);
  prefs.getString("tz", current.timezone, sizeof(current.timezone));
  if (strlen(current.timezone) == 0) {
    strncpy(current.timezone, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(current.timezone) - 1);
  }

  current.hydrationIntervalMinutes = prefs.getUShort("hydroInt", 60);
  // Falls back to the pre-rename key so a device that was already
  // running keeps whatever interval it was configured with; the first
  // write through setMovementIntervalMinutes() moves it to "moveInt".
  current.movementIntervalMinutes =
      prefs.getUShort("moveInt", prefs.getUShort("postureInt", 45));

  current.latitude = prefs.getFloat("lat", 50.8503f);
  current.longitude = prefs.getFloat("lon", 4.3517f);
  current.weatherPollIntervalMinutes = prefs.getUShort("wxPollMin", 15);

  current.activeBrightnessPercent = prefs.getUChar("activeBr", 50);
  current.sleepBrightnessPercent = prefs.getUChar("sleepBr", 5);
  current.displayFlipped = prefs.getBool("dispFlip", false);
  current.sleepyLeadMinutes = prefs.getUShort("sleepyLead", 60);
  current.ledEnabled = prefs.getBool("ledOn", false);
  current.ledR = prefs.getUChar("ledR", 50);
  current.ledG = prefs.getUChar("ledG", 50);
  current.ledB = prefs.getUChar("ledB", 50);

  current.glitchAverageIntervalMinutes = prefs.getUShort("glitchInt", 10);

  prefs.getString("wifiSsid", current.wifiSsid, sizeof(current.wifiSsid));
  prefs.getString("wifiPass", current.wifiPassword, sizeof(current.wifiPassword));
  prefs.getString("otaPass", current.otaPassword, sizeof(current.otaPassword));
  if (strlen(current.otaPassword) == 0) {
    strncpy(current.otaPassword, "bzl9-setup", sizeof(current.otaPassword) - 1);
  }
}

void migrate() {
  if (current.schemaVersion < 2) {
    // v1 -> v2: removed MoodEngine's metric-decay/interaction fields
    // (Energy/Attention/Fun draining + Pet/Pay-attention). Their stored
    // values are no longer read by loadAll() above; explicitly delete the
    // keys so they don't linger in NVS indefinitely.
    prefs.remove("enDecay");
    prefs.remove("atDecay");
    prefs.remove("funDecay");
    prefs.remove("petInc");
    prefs.remove("atInc");
    prefs.remove("enTaper");
  }
  // v4 -> v5 added the RGB LED fields; v3 -> v4 added `sleepyLeadMinutes`;
  // v2 -> v3 added `displayFlipped`.
  // None needs a migration step, for the same reason:
  // Neither needs a migration step, for the same reason:
  // v2 -> v3 added `displayFlipped`. No migration step is needed: a
  // missing NVS key already reads back as its documented default (false)
  // via prefs.getBool(), so there is nothing to move or delete. The
  // version still bumps so the store records which schema wrote it.

  // Future bumps append another `if (current.schemaVersion < N) { ... }`
  // step here, each moving the store forward exactly one version.
  if (current.schemaVersion != kSchemaVersion) {
    current.schemaVersion = kSchemaVersion;
    prefs.putUShort("schemaVer", kSchemaVersion);
  }
}

}  // namespace

// Reclaims the pre-rename namespace. Idempotent: on a device that never
// had one, this opens an empty namespace, clears nothing and closes again.
void eraseLegacyNamespace() {
  Preferences legacy;
  if (!legacy.begin(kLegacyNamespace, false)) return;
  legacy.clear();
  legacy.end();
}

void begin() {
  // Called once at boot, before WebPortal exists -- no concurrent access
  // is possible yet, so loadAll()/migrate() run unlocked. The mutex only
  // needs to be in place before the first get()/set*() call after this.
  mutex = xSemaphoreCreateMutex();

  // Corrupt/empty NVS: Preferences::begin() creates the namespace if
  // missing, and every getter above supplies its documented default for
  // any key that isn't present -- so a blank or damaged store just
  // resolves to all-defaults rather than failing.
  eraseLegacyNamespace();

  prefs.begin(kNamespace, false);
  loadAll();
  migrate();
}

Config get() {
  LockGuard lock;
  return current;
}

bool setWorkdayStartMinutes(uint16_t v) {
  if (v > 1439) return false;
  LockGuard lock;
  current.workdayStartMinutes = v;
  prefs.putUShort("wdStart", v);
  return true;
}

bool setWorkdayEndMinutes(uint16_t v) {
  if (v > 1439) return false;
  LockGuard lock;
  current.workdayEndMinutes = v;
  prefs.putUShort("wdEnd", v);
  return true;
}

bool setTimezone(const char *v) {
  if (v == nullptr || v[0] == '\0' || strlen(v) >= sizeof(Config::timezone)) return false;
  LockGuard lock;
  strncpy(current.timezone, v, sizeof(current.timezone) - 1);
  current.timezone[sizeof(current.timezone) - 1] = '\0';
  prefs.putString("tz", current.timezone);
  return true;
}

bool setHydrationIntervalMinutes(uint16_t v) {
  if (v < 5 || v > 480) return false;
  LockGuard lock;
  current.hydrationIntervalMinutes = v;
  prefs.putUShort("hydroInt", v);
  return true;
}

bool setMovementIntervalMinutes(uint16_t v) {
  if (v < 5 || v > 480) return false;
  LockGuard lock;
  current.movementIntervalMinutes = v;
  prefs.putUShort("moveInt", v);
  return true;
}

bool setLatitude(float v) {
  if (v < -90.0f || v > 90.0f) return false;
  LockGuard lock;
  current.latitude = v;
  prefs.putFloat("lat", v);
  return true;
}

bool setLongitude(float v) {
  if (v < -180.0f || v > 180.0f) return false;
  LockGuard lock;
  current.longitude = v;
  prefs.putFloat("lon", v);
  return true;
}

bool setWeatherPollIntervalMinutes(uint16_t v) {
  if (v < 5 || v > 180) return false;
  LockGuard lock;
  current.weatherPollIntervalMinutes = v;
  prefs.putUShort("wxPollMin", v);
  return true;
}

bool setActiveBrightnessPercent(uint8_t v) {
  if (v < 1 || v > BACKLIGHT_MAX_SUSTAINED_PERCENT) return false;
  LockGuard lock;
  current.activeBrightnessPercent = v;
  prefs.putUChar("activeBr", v);
  return true;
}

bool setSleepBrightnessPercent(uint8_t v) {
  if (v > BACKLIGHT_MAX_SUSTAINED_PERCENT) return false;
  LockGuard lock;
  current.sleepBrightnessPercent = v;
  prefs.putUChar("sleepBr", v);
  return true;
}

// No range to reject -- both values are meaningful. Returns bool only to
// match the shape of every other setter here.
bool setScheduleEnabled(bool v) {
  LockGuard lock;
  current.scheduleEnabled = v;
  prefs.putBool("schedOn", v);
  return true;
}

bool setSleepyLeadMinutes(uint16_t v) {
  if (v < 5 || v > 240) return false;
  LockGuard lock;
  current.sleepyLeadMinutes = v;
  prefs.putUShort("sleepyLead", v);
  return true;
}

// No range to reject -- every uint8_t is a valid channel value, and both
// enabled states are meaningful. Returns bool purely to match the shape of
// the other setters.
bool setLed(bool enabled, uint8_t r, uint8_t g, uint8_t b) {
  LockGuard lock;
  current.ledEnabled = enabled;
  current.ledR = r;
  current.ledG = g;
  current.ledB = b;
  prefs.putBool("ledOn", enabled);
  prefs.putUChar("ledR", r);
  prefs.putUChar("ledG", g);
  prefs.putUChar("ledB", b);
  return true;
}

bool setDisplayFlipped(bool v) {
  LockGuard lock;
  current.displayFlipped = v;
  prefs.putBool("dispFlip", v);
  return true;
}

bool setGlitchAverageIntervalMinutes(uint16_t v) {
  if (v < 1 || v > 120) return false;
  LockGuard lock;
  current.glitchAverageIntervalMinutes = v;
  prefs.putUShort("glitchInt", v);
  return true;
}

bool setWifiCredentials(const char *ssid, const char *password) {
  if (ssid == nullptr || strlen(ssid) >= sizeof(Config::wifiSsid)) return false;
  if (password == nullptr || strlen(password) >= sizeof(Config::wifiPassword)) return false;
  LockGuard lock;
  strncpy(current.wifiSsid, ssid, sizeof(current.wifiSsid) - 1);
  current.wifiSsid[sizeof(current.wifiSsid) - 1] = '\0';
  strncpy(current.wifiPassword, password, sizeof(current.wifiPassword) - 1);
  current.wifiPassword[sizeof(current.wifiPassword) - 1] = '\0';
  prefs.putString("wifiSsid", current.wifiSsid);
  prefs.putString("wifiPass", current.wifiPassword);
  return true;
}

bool setOtaPassword(const char *v) {
  if (v == nullptr || strlen(v) >= sizeof(Config::otaPassword)) return false;
  LockGuard lock;
  strncpy(current.otaPassword, v, sizeof(current.otaPassword) - 1);
  current.otaPassword[sizeof(current.otaPassword) - 1] = '\0';
  prefs.putString("otaPass", current.otaPassword);
  return true;
}

}  // namespace ConfigStore
