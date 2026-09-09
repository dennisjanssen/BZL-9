#include "WebPortal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ElegantOTA.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <algorithm>

#include "Config.h"
#include "ConfigStore.h"
#include "DisplayEngine.h"
#include "StatusLed.h"
#include "WeatherService.h"

namespace WebPortal {

namespace {

AsyncWebServer server(80);
DNSServer dnsServer;

// --- Cross-task status snapshot (published by the loop task, read by
// AsyncTCP-task web handlers) ---
SemaphoreHandle_t statusMutex = nullptr;
MoodEngine::State snapState = MoodEngine::State::NEUTRAL;
bool snapTimeUnknown = true;
bool snapMoodOverride = false;

const char *stateName(MoodEngine::State s) {
  switch (s) {
    case MoodEngine::State::SLEEPING: return "SLEEPING";
    case MoodEngine::State::SLEEPY: return "SLEEPY";
    case MoodEngine::State::NEUTRAL: return "NEUTRAL";
    case MoodEngine::State::BORED: return "BORED";
    case MoodEngine::State::EXCITED: return "EXCITED";
    case MoodEngine::State::FOCUSED: return "FOCUSED";
    case MoodEngine::State::GLITCHED: return "GLITCHED";
    case MoodEngine::State::HYDRATION_REMINDER: return "HYDRATION_REMINDER";
    case MoodEngine::State::POSTURE_REMINDER: return "POSTURE_REMINDER";
  }
  return "?";
}

// --- Command queue (AsyncTCP task enqueues, loop task drains/applies) ---
enum class CommandType : uint8_t { EXPRESS, CONFIG_UPDATE, REBOOT, MOOD };

struct ConfigUpdatePayload {
  bool hasWorkdayStart = false;
  uint16_t workdayStart = 0;
  bool hasWorkdayEnd = false;
  uint16_t workdayEnd = 0;
  bool hasTimezone = false;
  char timezone[64] = "";
  bool hasHydrationInterval = false;
  uint16_t hydrationInterval = 0;
  bool hasPostureInterval = false;
  uint16_t postureInterval = 0;
  bool hasSleepyLead = false;
  uint16_t sleepyLead = 0;
  bool hasLatitude = false;
  float latitude = 0;
  bool hasLongitude = false;
  float longitude = 0;
  bool hasWeatherPollInterval = false;
  uint16_t weatherPollInterval = 0;
  bool hasActiveBrightness = false;
  uint8_t activeBrightness = 0;
  bool hasSleepBrightness = false;
  uint8_t sleepBrightness = 0;
  bool hasDisplayFlipped = false;
  bool displayFlipped = false;
  bool hasLed = false;
  bool ledEnabled = false;
  uint8_t ledR = 0;
  uint8_t ledG = 0;
  uint8_t ledB = 0;
  bool hasGlitchInterval = false;
  uint16_t glitchInterval = 0;
  bool hasWifiSsid = false;
  char wifiSsid[33] = "";
  bool hasWifiPassword = false;
  char wifiPassword[65] = "";
  bool hasOtaPassword = false;
  char otaPassword[65] = "";
};

struct Command {
  CommandType type;
  char expression[16] = "";
  char mood[12] = "";
  ConfigUpdatePayload config;
};

QueueHandle_t commandQueue = nullptr;

void enqueue(const Command &cmd) {
  if (commandQueue == nullptr) return;
  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println("[WebPortal] command queue full, dropping command");
  }
}

// --- Wi-Fi provisioning / reconnect-with-backoff state machine ---
enum class NetState { CONNECTING, CONNECTED, WAITING_RETRY };
NetState netState = NetState::CONNECTING;
uint32_t netDeadlineMs = 0;
uint32_t netNextAttemptMs = 0;
uint8_t consecutiveFailures = 0;
uint32_t backoffMs = 5000;
bool apActive = false;
bool reconnectRequested = false;
constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kMaxBackoffMs = 60000;

void startApFallback() {
  if (apActive) return;
  WiFi.mode(WIFI_MODE_APSTA);
  IPAddress apIP(192, 168, 4, 1);
  // ASSUMPTION: open AP (no password) for ease of first-time setup --
  // brief doesn't specify one. Anyone in range can reach the config
  // portal while provisioning is incomplete.
  WiFi.softAP("BZL-9-Companion");
  dnsServer.start(53, "*", apIP);
  apActive = true;
  Serial.println("[WebPortal] AP fallback active: BZL-9-Companion (open)");
}

void beginStaAttempt() {
  auto cfg = ConfigStore::get();
  if (strlen(cfg.wifiSsid) == 0) {
    startApFallback();
    netState = NetState::WAITING_RETRY;
    netNextAttemptMs = 0;  // never auto-retry with empty creds
    return;
  }
  WiFi.mode(apActive ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  WiFi.begin(cfg.wifiSsid, cfg.wifiPassword);
  netState = NetState::CONNECTING;
  netDeadlineMs = millis() + kConnectTimeoutMs;
}

void updateNetworking() {
  if (reconnectRequested) {
    reconnectRequested = false;
    consecutiveFailures = 0;
    backoffMs = 5000;
    beginStaAttempt();
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (netState != NetState::CONNECTED) {
      Serial.printf("[WebPortal] STA connected, IP=%s\n", WiFi.localIP().toString().c_str());
      // Started here rather than once at boot, and torn down first
      // every time: the responder binds to the network interface, so
      // one started before a reconnect (or before Wi-Fi existed at
      // all) quietly stops answering without reporting an error.
      MDNS.end();
      if (MDNS.begin(MDNS_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WebPortal] mDNS up: http://%s.local/\n", MDNS_HOSTNAME);
      } else {
        Serial.println("[WebPortal] mDNS failed to start (the IP still works)");
      }
    }
    netState = NetState::CONNECTED;
    consecutiveFailures = 0;
    backoffMs = 5000;
    return;
  }

  uint32_t now = millis();
  switch (netState) {
    case NetState::CONNECTING:
      if (now >= netDeadlineMs) {
        consecutiveFailures++;
        Serial.printf("[WebPortal] STA connect attempt failed (%u consecutive)\n", consecutiveFailures);
        if (consecutiveFailures >= 2) startApFallback();
        netNextAttemptMs = now + backoffMs;
        backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
        netState = NetState::WAITING_RETRY;
      }
      break;
    case NetState::WAITING_RETRY:
      if (netNextAttemptMs != 0 && now >= netNextAttemptMs) {
        beginStaAttempt();
      }
      break;
    case NetState::CONNECTED:
      Serial.println("[WebPortal] STA dropped, will reconnect");
      netState = NetState::WAITING_RETRY;
      netNextAttemptMs = now;
      break;
  }
}

// --- Applying queued commands (loop task only) ---
void applyCommand(const Command &cmd) {
  switch (cmd.type) {
    case CommandType::EXPRESS:
      DisplayEngine::triggerExpression(cmd.expression);
      break;
    case CommandType::CONFIG_UPDATE: {
      const auto &c = cmd.config;
      if (c.hasWorkdayStart) ConfigStore::setWorkdayStartMinutes(c.workdayStart);
      if (c.hasWorkdayEnd) ConfigStore::setWorkdayEndMinutes(c.workdayEnd);
      if (c.hasTimezone) {
        ConfigStore::setTimezone(c.timezone);
        configTzTime(c.timezone, "pool.ntp.org", "time.nist.gov");
      }
      if (c.hasHydrationInterval) ConfigStore::setHydrationIntervalMinutes(c.hydrationInterval);
      if (c.hasPostureInterval) ConfigStore::setPostureIntervalMinutes(c.postureInterval);
      if (c.hasSleepyLead) ConfigStore::setSleepyLeadMinutes(c.sleepyLead);
      if (c.hasLatitude) ConfigStore::setLatitude(c.latitude);
      if (c.hasLongitude) ConfigStore::setLongitude(c.longitude);
      if (c.hasWeatherPollInterval) ConfigStore::setWeatherPollIntervalMinutes(c.weatherPollInterval);
      if (c.hasActiveBrightness) {
        ConfigStore::setActiveBrightnessPercent(c.activeBrightness);
        DisplayEngine::setBrightnessPercent(c.activeBrightness);
      }
      if (c.hasSleepBrightness) ConfigStore::setSleepBrightnessPercent(c.sleepBrightness);
      if (c.hasLed) {
        ConfigStore::setLed(c.ledEnabled, c.ledR, c.ledG, c.ledB);
        // Safe here: this drain runs on the loop task, and rgbLedWrite()
        // touches RMT, which must not be driven from AsyncTCP.
        StatusLed::apply(c.ledEnabled, c.ledR, c.ledG, c.ledB);
      }
      if (c.hasDisplayFlipped) {
        ConfigStore::setDisplayFlipped(c.displayFlipped);
        // Safe here: this drain runs on the loop task, not AsyncTCP.
        DisplayEngine::setDisplayFlipped(c.displayFlipped);
      }
      if (c.hasGlitchInterval) ConfigStore::setGlitchAverageIntervalMinutes(c.glitchInterval);
      if (c.hasWifiSsid || c.hasWifiPassword) {
        auto cur = ConfigStore::get();
        const char *ssid = c.hasWifiSsid ? c.wifiSsid : cur.wifiSsid;
        const char *pass = c.hasWifiPassword ? c.wifiPassword : cur.wifiPassword;
        if (ConfigStore::setWifiCredentials(ssid, pass)) reconnectRequested = true;
      }
      if (c.hasOtaPassword) {
        ConfigStore::setOtaPassword(c.otaPassword);
        ElegantOTA.setAuth("admin", c.otaPassword);
      }
      break;
    }
    case CommandType::MOOD: {
      const char *m = cmd.mood;
      if (strcmp(m, "auto") == 0) {
        MoodEngine::clearDayMoodOverride();
      } else if (strcmp(m, "neutral") == 0) {
        MoodEngine::setDayMoodOverride(MoodEngine::State::NEUTRAL);
      } else if (strcmp(m, "bored") == 0) {
        MoodEngine::setDayMoodOverride(MoodEngine::State::BORED);
      } else if (strcmp(m, "excited") == 0) {
        MoodEngine::setDayMoodOverride(MoodEngine::State::EXCITED);
      } else if (strcmp(m, "focused") == 0) {
        MoodEngine::setDayMoodOverride(MoodEngine::State::FOCUSED);
      } else if (strcmp(m, "sleepy") == 0) {
        MoodEngine::setDayMoodOverride(MoodEngine::State::SLEEPY);
      }
      break;
    }
    case CommandType::REBOOT:
      Serial.println("[WebPortal] reboot requested via portal");
      ESP.restart();
      break;
  }
}

// --- Route handlers ---
void handleStatus(AsyncWebServerRequest *request) {
  MoodEngine::State state;
  bool timeUnknown;
  bool moodOverride;
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  state = snapState;
  timeUnknown = snapTimeUnknown;
  moodOverride = snapMoodOverride;
  xSemaphoreGive(statusMutex);

  JsonDocument doc;
  doc["state"] = stateName(state);
  doc["uptimeSeconds"] = millis() / 1000;
  doc["timeUnknown"] = timeUnknown;
  doc["moodOverride"] = moodOverride;

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  bool connected = WiFi.status() == WL_CONNECTED;
  wifi["connected"] = connected;
  wifi["apActive"] = apActive;
  if (connected) {
    wifi["rssi"] = WiFi.RSSI();
    wifi["ip"] = WiFi.localIP().toString();
  }

  auto weather = WeatherService::current();
  if (weather.available) {
    JsonObject w = doc["weather"].to<JsonObject>();
    w["temperatureC"] = weather.temperatureC;
    w["weatherCode"] = weather.weatherCode;
    w["precipitationMm"] = weather.precipitationMm;
    w["stale"] = weather.stale;
    w["rainChancePercent"] = weather.rainChancePercent;
    w["rainSumMm"] = weather.rainSumMm;
    w["rainExpectedToday"] = weather.rainExpectedToday;
  } else {
    doc["weather"] = nullptr;
  }

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

void handleExpress(AsyncWebServerRequest *request, JsonVariant &json) {
  if (!json["expression"].is<const char *>()) {
    request->send(400, "application/json", "{\"error\":\"missing expression\"}");
    return;
  }
  const char *expr = json["expression"];
  static const char *kValid[] = {"shock",  "heart",   "rage",        "sleepy",
                                "glitch", "hydrate", "unimpressed", "grin",
                                "wave",   "whistle"};
  bool ok = false;
  for (const char *v : kValid) {
    if (strcmp(expr, v) == 0) {
      ok = true;
      break;
    }
  }
  if (!ok || strlen(expr) >= sizeof(Command::expression)) {
    request->send(400, "application/json", "{\"error\":\"invalid expression\"}");
    return;
  }
  Command cmd{};
  cmd.type = CommandType::EXPRESS;
  strncpy(cmd.expression, expr, sizeof(cmd.expression) - 1);
  enqueue(cmd);
  request->send(200, "application/json", "{\"ok\":true}");
}

// Mood buttons. Validated here, applied on the loop task -- MoodEngine
// has no internal locking, so an AsyncTCP handler must never write it.
void handleMood(AsyncWebServerRequest *request, JsonVariant &json) {
  if (!json["mood"].is<const char *>()) {
    request->send(400, "application/json", "{\"error\":\"missing mood\"}");
    return;
  }
  const char *m = json["mood"];
  static const char *kValid[] = {"auto", "neutral", "bored", "excited", "focused", "sleepy"};
  bool ok = false;
  for (const char *v : kValid) {
    if (strcmp(m, v) == 0) {
      ok = true;
      break;
    }
  }
  Command cmd{};
  if (!ok || strlen(m) >= sizeof(cmd.mood)) {
    request->send(400, "application/json", "{\"error\":\"invalid mood\"}");
    return;
  }
  cmd.type = CommandType::MOOD;
  strncpy(cmd.mood, m, sizeof(cmd.mood) - 1);
  enqueue(cmd);
  request->send(200, "application/json", "{\"ok\":true}");
}

void handleGetConfig(AsyncWebServerRequest *request) {
  auto cfg = ConfigStore::get();
  JsonDocument doc;
  doc["schemaVersion"] = cfg.schemaVersion;
  doc["workdayStartMinutes"] = cfg.workdayStartMinutes;
  doc["workdayEndMinutes"] = cfg.workdayEndMinutes;
  doc["timezone"] = cfg.timezone;
  doc["hydrationIntervalMinutes"] = cfg.hydrationIntervalMinutes;
  doc["postureIntervalMinutes"] = cfg.postureIntervalMinutes;
  doc["sleepyLeadMinutes"] = cfg.sleepyLeadMinutes;
  doc["latitude"] = cfg.latitude;
  doc["longitude"] = cfg.longitude;
  doc["weatherPollIntervalMinutes"] = cfg.weatherPollIntervalMinutes;
  doc["activeBrightnessPercent"] = cfg.activeBrightnessPercent;
  doc["sleepBrightnessPercent"] = cfg.sleepBrightnessPercent;
  doc["displayFlipped"] = cfg.displayFlipped;
  doc["ledEnabled"] = cfg.ledEnabled;
  doc["ledR"] = cfg.ledR;
  doc["ledG"] = cfg.ledG;
  doc["ledB"] = cfg.ledB;
  doc["glitchAverageIntervalMinutes"] = cfg.glitchAverageIntervalMinutes;
  doc["wifiSsid"] = cfg.wifiSsid;
  // wifiPassword and otaPassword are intentionally never included here.

  String out;
  serializeJson(doc, out);
  request->send(200, "application/json", out);
}

// Returns false (and has already sent a 400) if `field` is present but out
// of the given [lo, hi] range.
template <typename T>
bool validateRange(AsyncWebServerRequest *request, const char *field, T value, T lo, T hi) {
  if (value < lo || value > hi) {
    String msg = String("{\"error\":\"") + field + " out of range\"}";
    request->send(400, "application/json", msg);
    return false;
  }
  return true;
}

void handlePostConfig(AsyncWebServerRequest *request, JsonVariant &json) {
  Command cmd{};
  cmd.type = CommandType::CONFIG_UPDATE;
  auto &c = cmd.config;

  if (json["workdayStartMinutes"].is<int>()) {
    int v = json["workdayStartMinutes"];
    if (!validateRange(request, "workdayStartMinutes", v, 0, 1439)) return;
    c.hasWorkdayStart = true;
    c.workdayStart = static_cast<uint16_t>(v);
  }
  if (json["workdayEndMinutes"].is<int>()) {
    int v = json["workdayEndMinutes"];
    if (!validateRange(request, "workdayEndMinutes", v, 0, 1439)) return;
    c.hasWorkdayEnd = true;
    c.workdayEnd = static_cast<uint16_t>(v);
  }
  if (json["timezone"].is<const char *>()) {
    const char *v = json["timezone"];
    if (v[0] == '\0' || strlen(v) >= sizeof(c.timezone)) {
      request->send(400, "application/json", "{\"error\":\"timezone invalid\"}");
      return;
    }
    c.hasTimezone = true;
    strncpy(c.timezone, v, sizeof(c.timezone) - 1);
  }
  if (json["hydrationIntervalMinutes"].is<int>()) {
    int v = json["hydrationIntervalMinutes"];
    if (!validateRange(request, "hydrationIntervalMinutes", v, 5, 480)) return;
    c.hasHydrationInterval = true;
    c.hydrationInterval = static_cast<uint16_t>(v);
  }
  if (json["postureIntervalMinutes"].is<int>()) {
    int v = json["postureIntervalMinutes"];
    if (!validateRange(request, "postureIntervalMinutes", v, 5, 480)) return;
    c.hasPostureInterval = true;
    c.postureInterval = static_cast<uint16_t>(v);
  }
  if (json["sleepyLeadMinutes"].is<int>()) {
    int v = json["sleepyLeadMinutes"];
    if (!validateRange(request, "sleepyLeadMinutes", v, 5, 240)) return;
    c.hasSleepyLead = true;
    c.sleepyLead = static_cast<uint16_t>(v);
  }
  if (json["latitude"].is<float>()) {
    float v = json["latitude"];
    if (!validateRange(request, "latitude", v, -90.0f, 90.0f)) return;
    c.hasLatitude = true;
    c.latitude = v;
  }
  if (json["longitude"].is<float>()) {
    float v = json["longitude"];
    if (!validateRange(request, "longitude", v, -180.0f, 180.0f)) return;
    c.hasLongitude = true;
    c.longitude = v;
  }
  if (json["weatherPollIntervalMinutes"].is<int>()) {
    int v = json["weatherPollIntervalMinutes"];
    if (!validateRange(request, "weatherPollIntervalMinutes", v, 5, 180)) return;
    c.hasWeatherPollInterval = true;
    c.weatherPollInterval = static_cast<uint16_t>(v);
  }
  if (json["activeBrightnessPercent"].is<int>()) {
    int v = json["activeBrightnessPercent"];
    if (!validateRange(request, "activeBrightnessPercent", v, 1, static_cast<int>(BACKLIGHT_MAX_SUSTAINED_PERCENT))) return;
    c.hasActiveBrightness = true;
    c.activeBrightness = static_cast<uint8_t>(v);
  }
  if (json["sleepBrightnessPercent"].is<int>()) {
    int v = json["sleepBrightnessPercent"];
    if (!validateRange(request, "sleepBrightnessPercent", v, 0, static_cast<int>(BACKLIGHT_MAX_SUSTAINED_PERCENT))) return;
    c.hasSleepBrightness = true;
    c.sleepBrightness = static_cast<uint8_t>(v);
  }
  if (json["displayFlipped"].is<bool>()) {
    c.hasDisplayFlipped = true;
    c.displayFlipped = json["displayFlipped"];
  }
  // LED colour and on/off travel together. Presence of any one of them
  // means "apply the LED block", with the current stored value filling in
  // for whichever fields the client left out.
  bool ledTouched = json["ledEnabled"].is<bool>() || json["ledR"].is<int>() ||
                    json["ledG"].is<int>() || json["ledB"].is<int>();
  if (ledTouched) {
    auto cur = ConfigStore::get();
    int r = cur.ledR;
    int g = cur.ledG;
    int b = cur.ledB;
    if (json["ledR"].is<int>()) r = json["ledR"];
    if (json["ledG"].is<int>()) g = json["ledG"];
    if (json["ledB"].is<int>()) b = json["ledB"];
    if (!validateRange(request, "ledR", r, 0, 255)) return;
    if (!validateRange(request, "ledG", g, 0, 255)) return;
    if (!validateRange(request, "ledB", b, 0, 255)) return;
    c.hasLed = true;
    c.ledEnabled = json["ledEnabled"].is<bool>() ? json["ledEnabled"].as<bool>() : cur.ledEnabled;
    c.ledR = static_cast<uint8_t>(r);
    c.ledG = static_cast<uint8_t>(g);
    c.ledB = static_cast<uint8_t>(b);
  }
  if (json["glitchAverageIntervalMinutes"].is<int>()) {
    int v = json["glitchAverageIntervalMinutes"];
    if (!validateRange(request, "glitchAverageIntervalMinutes", v, 1, 120)) return;
    c.hasGlitchInterval = true;
    c.glitchInterval = static_cast<uint16_t>(v);
  }
  if (json["wifiSsid"].is<const char *>()) {
    const char *v = json["wifiSsid"];
    if (strlen(v) >= sizeof(c.wifiSsid)) {
      request->send(400, "application/json", "{\"error\":\"wifiSsid too long\"}");
      return;
    }
    c.hasWifiSsid = true;
    strncpy(c.wifiSsid, v, sizeof(c.wifiSsid) - 1);
  }
  if (json["wifiPassword"].is<const char *>()) {
    const char *v = json["wifiPassword"];
    if (strlen(v) >= sizeof(c.wifiPassword)) {
      request->send(400, "application/json", "{\"error\":\"wifiPassword too long\"}");
      return;
    }
    c.hasWifiPassword = true;
    strncpy(c.wifiPassword, v, sizeof(c.wifiPassword) - 1);
  }
  if (json["otaPassword"].is<const char *>()) {
    const char *v = json["otaPassword"];
    if (v[0] == '\0' || strlen(v) >= sizeof(c.otaPassword)) {
      request->send(400, "application/json", "{\"error\":\"otaPassword invalid\"}");
      return;
    }
    c.hasOtaPassword = true;
    strncpy(c.otaPassword, v, sizeof(c.otaPassword) - 1);
  }

  enqueue(cmd);
  request->send(200, "application/json", "{\"ok\":true}");
}

void handleReboot(AsyncWebServerRequest *request) {
  Command cmd{};
  cmd.type = CommandType::REBOOT;
  enqueue(cmd);
  request->send(200, "application/json", "{\"ok\":true}");
}

}  // namespace

void begin() {
  statusMutex = xSemaphoreCreateMutex();
  commandQueue = xQueueCreate(8, sizeof(Command));

  // LittleFS holds only web assets (Config.h/ConfigStore own the actual
  // settings, per the brief, precisely so a filesystem image upload can
  // never wipe configuration). Partition label must match partitions.csv's
  // "littlefs" row -- the Arduino default ("spiffs") would fail to mount.
  LittleFS.begin(false, "/littlefs", 10, "littlefs");

  server.on("/api/status", HTTP_GET, handleStatus);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/express", handleExpress));
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/mood", handleMood));
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.addHandler(new AsyncCallbackJsonWebHandler("/api/config", handlePostConfig));
  server.on("/api/reboot", HTTP_POST, handleReboot);

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
  // Captive-ish: any unmatched path in AP mode bounces back to the
  // dashboard so most phones/OSes offer to open it automatically. Brief
  // only asks for "captive-ish", not full captive-portal-detection
  // compliance.
  server.onNotFound([](AsyncWebServerRequest *request) { request->redirect("/"); });

  ElegantOTA.begin(&server, "admin", ConfigStore::get().otaPassword);
  server.begin();

  beginStaAttempt();
}

void loop() {
  updateNetworking();
  ElegantOTA.loop();

  Command cmd;
  while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
    applyCommand(cmd);
  }
}

void publishMoodSnapshot(MoodEngine::State state, bool timeUnknown) {
  xSemaphoreTake(statusMutex, portMAX_DELAY);
  snapState = state;
  snapTimeUnknown = timeUnknown;
  // Read here, on the loop task, rather than from the web handler --
  // MoodEngine is not safe to touch from AsyncTCP.
  snapMoodOverride = MoodEngine::dayMoodOverridden();
  xSemaphoreGive(statusMutex);
}

}  // namespace WebPortal
