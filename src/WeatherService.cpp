#include "WeatherService.h"

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include "ConfigStore.h"

namespace WeatherService {

namespace {

SemaphoreHandle_t mutex = nullptr;
TaskHandle_t taskHandle = nullptr;
Reading snapshot;
uint32_t lastSuccessMs = 0;
bool everSucceeded = false;

constexpr uint32_t kStaleAfterMs = 3600000UL;  // 1 hour, per the brief

// The configured poll interval applies only AFTER a fetch that actually
// worked. It used to be the delay after every pass, including passes that
// fetched nothing -- and since this task is started immediately after the
// non-blocking Wi-Fi connect begins, its first pass always runs before the
// association completes. That meant a device with a default 15 minute
// interval showed no weather for 15 minutes after every boot, and longer
// on a first-ever boot, where Wi-Fi is not configured until the user has
// been through the portal.
//
// Not connected yet: only a WiFi.status() check, no traffic at all, so
// this can be cheap and frequent.
constexpr uint32_t kNotConnectedRetryMs = 2000;
// Connected but the fetch failed: back off, so a lasting failure (DNS,
// captive portal, the API down) does not hammer Open-Meteo for hours.
constexpr uint32_t kFailRetryMinMs = 15000;
constexpr uint32_t kFailRetryMaxMs = 300000;

// ASSUMPTION: the brief names three overlays (rain, sunglasses, shiver)
// and says to map WMO codes to them "with an explicit switch", but doesn't
// give the mapping itself. Grouped by the standard WMO weather-code table
// (https://open-meteo.com/en/docs -- WMO Weather interpretation codes):
// clear/mainly-clear -> sunglasses; any drizzle/rain/showers/thunderstorm
// -> rain; any snow -> shiver. Partly cloudy, overcast, and fog map to no
// overlay -- none of the three fit those conditions.
// ASSUMPTION: "rain is coming today" needs both a real chance AND a real
// amount. Probability alone flags a near-certain 0.1mm as weather worth
// reacting to, which is a damp breeze; amount alone flags a downpour the
// model only half believes in.
constexpr int kRainChanceThresholdPercent = 50;
constexpr float kRainSumThresholdMm = 0.5f;

Overlay mapWeatherCode(int code) {
  switch (code) {
    case 0:
    case 1:
      return Overlay::SUNGLASSES;
    case 2:
    case 3:
    case 45:
    case 48:
      return Overlay::NONE;
    case 51:
    case 53:
    case 55:
    case 56:
    case 57:
    case 61:
    case 63:
    case 65:
    case 66:
    case 67:
    case 80:
    case 81:
    case 82:
    case 95:
    case 96:
    case 99:
      return Overlay::RAIN;
    case 71:
    case 73:
    case 75:
    case 77:
    case 85:
    case 86:
      return Overlay::SHIVER;
    default:
      Serial.printf("[WeatherService] unmapped WMO weather_code: %d\n", code);
      return Overlay::NONE;
  }
}

void publish(const Reading &r) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  snapshot = r;
  xSemaphoreGive(mutex);
}

bool fetchOnce() {
  auto cfg = ConfigStore::get();
  // `daily` needs `timezone` or Open-Meteo buckets the day by GMT, which
  // would roll "today" over at the wrong hour; `timezone=auto` resolves it
  // from the coordinates. forecast_days=1 keeps the response small -- we
  // only ever look at index 0. Field names verified against a live
  // response, not assumed.
  char url[288];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,"
           "precipitation&daily=precipitation_probability_max,precipitation_sum&forecast_days=1&timezone=auto",
           cfg.latitude, cfg.longitude);

  NetworkClientSecure client;
  // ASSUMPTION: skips certificate validation rather than pinning/bundling
  // a root CA. This call only ever reads public, non-sensitive weather
  // data (nothing sensitive is sent either), so full TLS chain validation
  // felt like disproportionate effort here -- flagged rather than done
  // silently.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    Serial.println("[WeatherService] http.begin() failed");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[WeatherService] GET failed, HTTP %d\n", code);
    http.end();
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    Serial.printf("[WeatherService] JSON parse failed: %s\n", err.c_str());
    return false;
  }
  if (!doc["current"]["temperature_2m"].is<float>() || !doc["current"]["weather_code"].is<int>()) {
    Serial.println("[WeatherService] response missing expected fields");
    return false;
  }

  Reading r;
  r.available = true;
  r.stale = false;
  r.temperatureC = doc["current"]["temperature_2m"];
  r.weatherCode = doc["current"]["weather_code"];
  r.precipitationMm = doc["current"]["precipitation"] | 0.0f;
  r.overlay = mapWeatherCode(r.weatherCode);

  // The daily block is treated as optional: a missing or null entry leaves
  // the forecast simply unknown rather than failing the whole fetch, since
  // the current conditions above are still perfectly usable.
  auto chance = doc["daily"]["precipitation_probability_max"][0];
  auto sum = doc["daily"]["precipitation_sum"][0];
  if (chance.is<int>()) r.rainChancePercent = chance.as<int>();
  if (sum.is<float>()) r.rainSumMm = sum.as<float>();
  r.rainExpectedToday = r.rainChancePercent >= kRainChanceThresholdPercent &&
                        r.rainSumMm >= kRainSumThresholdMm;

  publish(r);

  lastSuccessMs = millis();
  everSucceeded = true;
  Serial.printf("[WeatherService] updated: %.1fC, code=%d, overlay=%d, rain today %d%% / %.1fmm -> %s\n",
                r.temperatureC, r.weatherCode, static_cast<int>(r.overlay), r.rainChancePercent,
                r.rainSumMm, r.rainExpectedToday ? "expected" : "no");
  return true;
}

void task(void *) {
  uint32_t failRetryMs = kFailRetryMinMs;

  for (;;) {
    bool connected = WiFi.status() == WL_CONNECTED;
    bool fetched = false;
    if (connected) {
      fetched = fetchOnce();
      if (!fetched && everSucceeded) {
        // Failure: keep the last reading in place (don't clear
        // `available`), just let the staleness check below catch up.
        Serial.println("[WeatherService] fetch failed, keeping last reading");
      }
    }

    if (everSucceeded) {
      bool stale = (millis() - lastSuccessMs) > kStaleAfterMs;
      xSemaphoreTake(mutex, portMAX_DELAY);
      snapshot.stale = stale;
      xSemaphoreGive(mutex);
    }

    uint32_t waitMs;
    if (fetched) {
      failRetryMs = kFailRetryMinMs;
      auto cfg = ConfigStore::get();
      waitMs = static_cast<uint32_t>(cfg.weatherPollIntervalMinutes) * 60000UL;
    } else if (!connected) {
      waitMs = kNotConnectedRetryMs;
    } else {
      waitMs = failRetryMs;
      uint32_t next = failRetryMs * 2;
      failRetryMs = next > kFailRetryMaxMs ? kFailRetryMaxMs : next;
    }

    // Returns early when requestRefresh() notifies, so new coordinates or
    // a fresh connection take effect now rather than at the end of the
    // interval. pdTRUE clears the count on the way out, so a burst of
    // requests collapses into the one fetch they all wanted.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
  }
}

}  // namespace

void begin() {
  mutex = xSemaphoreCreateMutex();
  // Priority 1 (just above idle): weather data is never urgent, and this
  // must not compete with the LVGL/loop task for CPU time.
  xTaskCreate(task, "weather", 8192, nullptr, 1, &taskHandle);
}

void requestRefresh() {
  if (taskHandle != nullptr) xTaskNotifyGive(taskHandle);
}

Reading current() {
  xSemaphoreTake(mutex, portMAX_DELAY);
  Reading r = snapshot;
  xSemaphoreGive(mutex);
  return r;
}

}  // namespace WeatherService
