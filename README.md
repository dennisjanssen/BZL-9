# BZL-9

A small desk companion. An ESP32-C6 drives a 1.47" LCD showing a procedurally
animated face — it blinks, looks around, yawns, gets bored, wears sunglasses when
the sun is out, and nags you to drink water. The screen is meant to be mounted as
the visor of a 3D-printed helmet, so the whole display is the face.

Everything is configured from a built-in web portal. There is no app and no cloud
service; the only outbound request is a weather lookup.

The face is drawn entirely from primitives at runtime — there are no image assets.

---

## Hardware

Built for the **Waveshare ESP32-C6-LCD-1.47** (or a clone of that reference
design). That specific board matters:

| | |
|---|---|
| MCU | ESP32-C6FH4 — single RISC-V core, **no PSRAM** |
| Flash | 4 MB |
| Display | ST7789, 172 × 320, driven in landscape (320 × 172) |
| Extras | onboard WS2812 RGB LED, microSD slot (unused) |

The pin map below is taken from the vendor's own demo, not guessed. If your board
differs, this is the one file you must change — see `include/Config.h`.

| Signal | GPIO |
|---|---|
| LCD SCLK | 7 |
| LCD MOSI | 6 |
| LCD MISO | 5 |
| LCD CS | 14 |
| LCD DC | 15 |
| LCD RST | 21 |
| LCD backlight (PWM) | 22 |
| RGB LED | 8 |

> **Backlight is capped at 50%.** Waveshare's own wiki warns that sustained full
> brightness can permanently damage this panel, so `BACKLIGHT_MAX_SUSTAINED_PERCENT`
> clamps it in firmware. Raise it at your own risk.

---

## Building

Uses [PlatformIO](https://platformio.org/). Note it pins the community
**pioarduino** platform fork rather than the official `espressif32` package —
the official one has not shipped Arduino-ESP32 core 3.x, which several of the
pinned libraries require.

```bash
git clone <your-repo-url> && cd BZL-9
pio run -e esp32-c6-bzl9
```

### Flashing

Two images have to be written, and **the firmware upload does not include the
web assets**:

```bash
pio run -e esp32-c6-bzl9 -t upload -t uploadfs
```

- `upload` writes the firmware.
- `uploadfs` packs `data/` (the dashboard) into the LittleFS partition.

If you skip `uploadfs`, the device boots and animates fine but serves a blank or
stale dashboard. This is the single most common mistake — if the portal looks
wrong after a change to anything in `data/`, run `uploadfs`.

---

## First-time setup

1. **Flash both images** (above) and power the board.
2. With no Wi-Fi credentials stored, it starts its own access point:
   **`BZL-9-Companion`** — open, no password.
3. Join it and open **<http://192.168.4.1>**. Any unmatched URL redirects there,
   so most phones offer the page automatically.
4. Fill in at minimum:
   - **Wi-Fi SSID / password** — for NTP time and weather.
   - **Latitude / longitude** — weather location. Defaults to Brussels, which is
     a placeholder, not a guess about you.
   - **Timezone** — a POSIX TZ string, e.g. `CET-1CEST,M3.5.0,M10.5.0/3`.
   - **Workday start / end** — drives the whole sleep schedule.
5. Save. It reconnects to your network; the dashboard is then at the IP shown in
   the serial log (or find it on your router).

The AP is deliberately open. It carries no secrets until you type them into it,
and requiring a password you cannot see anywhere is a poor first-run experience.
Set your Wi-Fi and it stops advertising.

If the face is upside down in your enclosure, tick **Flip display 180°**. It
applies immediately, no reboot.

---

## What it does

**Day moods** — cycles at random between `neutral`, `bored`, `excited` and
`focused`, holding each for 3–10 minutes. Each has its own resting pose: eye
height, mouth shape, head tilt, breathing rate, and how often it fidgets. Eyebrow
angle is faked by masking the top of each eye, so `focused` looks determined and
`bored` looks unimpressed.

You can pin a mood from the dashboard, or leave it on **Auto**. A pinned mood is
dropped overnight so a forgotten one does not outlive the day.

**Sleep schedule** — gets visibly sleepy for a configurable window before your
workday ends (heavy lids, drooping head, yawning), then sleeps once it is over:
eyes shut, `Zzz` drifting, and a snore. Brightness fades to the sleep level.

**Reminders** — on their own intervals, it drinks from a blue water bottle that
visibly empties over four sips, and does a stretch-then-walk routine for posture.

**Weather** — fetched from [Open-Meteo](https://open-meteo.com/) (no API key).
Sunglasses drop onto its face when it is clear, it shivers when it snows, rain
streaks fall when it rains, and when rain is *forecast later today* a small cloud
drifts in and it recoils from it, wide-eyed.

**Expressions** — one-shot reactions from the dashboard: `shock`, `heart`, `rage`,
`sleepy`, `glitch`, `hydrate`, `unimpressed`, `grin`.

**Status LED** — the onboard WS2812. Off by default; pick any colour in the
portal. Note this board's LED is wired **RGB, not the usual GRB** — if you port
this and red/green come out swapped, that is why.

---

## Configuration reference

All settings live in NVS (not the filesystem), so a `uploadfs` can never wipe
them. Every field is range-checked server-side and an out-of-range value is
rejected with HTTP 400 rather than silently clamped.

| Setting | Default | Range |
|---|---|---|
| Workday start / end | 09:00 / 17:30 | 00:00–23:59 |
| Timezone | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX TZ |
| Sleepy before end | 60 min | 5–240 |
| Hydration reminder | 60 min | 5–480 |
| Posture reminder | 45 min | 5–480 |
| Latitude / longitude | 50.8503 / 4.3517 | ±90 / ±180 |
| Weather poll | 15 min | 5–180 |
| Active brightness | 50% | 1–50 |
| Sleep brightness | 5% | 0–50 |
| Glitch avg. interval¹ | 10 min | 1–120 |
| Flip display 180° | off | — |
| Onboard LED | off, dim white | 0–255 per channel |

¹ Settable via the API only; it has no field in the dashboard form.

### API

| Endpoint | |
|---|---|
| `GET /api/status` | state, uptime, Wi-Fi, weather |
| `GET /api/config` | current settings (**never** returns passwords) |
| `POST /api/config` | partial update; only the keys you send change |
| `POST /api/express` | `{"expression": "shock"}` |
| `POST /api/mood` | `{"mood": "bored"}` or `"auto"` |
| `POST /api/reboot` | |

---

## OTA updates

Served at `/update` (ElegantOTA), behind HTTP basic auth as user `admin`.

> **The default password is `bzl9-setup`.** Change it in the portal before putting
> this on a network you share. It defaults to something rather than nothing
> because an empty password would leave the update endpoint wide open out of the box.

OTA writes to the inactive app slot, so a bad image cannot brick the device.

**Firmware only, in practice.** ElegantOTA's page offers a "filesystem" mode, but
it will not work on this partition table: the Arduino `Update` library looks for a
partition with the **`spiffs`** subtype (`0x82`), while `partitions.csv` declares
`littlefs` (`0x83`), so the lookup finds nothing and the upload fails. Dashboard
changes therefore need `uploadfs` over USB.

If you want working filesystem OTA, declare the partition with subtype `spiffs`
but keep the label `littlefs` — the mount call finds it by *label*, so LittleFS
keeps working while `Update` can finally see it. Changing the partition table
means one USB flash and it wipes the filesystem.

---

## Layout and limits

4 MB of flash, partitioned as two 1.6875 MB OTA slots plus 576 KB LittleFS:

```
nvs       0x009000   20 KB    settings
otadata   0x00e000    8 KB
app0      0x010000  1.6875 MB
app1      0x1C0000  1.6875 MB
littlefs  0x370000  576 KB    web assets (~11 KB used)
```

**The firmware sits at about 90% of one OTA slot.** If you add much, you will run
out. The cheapest fix is repartitioning: LittleFS holds ~11 KB of assets, so
shrinking it to 128 KB and giving the space to both app slots drops usage to
around 80% with no code changes. Be aware that moving the app offsets means one
USB flash (you cannot OTA into a new layout) and it wipes the filesystem.

Beyond that, roughly 36 KB of unused LVGL widget classes are linked because the
default theme references every enabled widget — disabling the ones this project
never creates is the next easiest win.

---

## Design notes

Two things are load-bearing and easy to break:

- **The animation layer composites.** Every animation writes one field of a shared
  channel struct and then calls `renderFace()`, which is the sole owner of object
  geometry. This is what lets blinking, breathing, gaze, head turns and mouth
  shapes run concurrently instead of taking turns. Setting positions directly
  will appear to work and then fight everything else.
- **Head turns fake 3D.** Features ride a virtual cylinder, so the eye turning
  away narrows and slides toward the edge while the near eye widens. That
  differential is why a turn reads as a head rather than a sliding picture.

`include/Config.h` carries the hardware facts, each with a note on where it was
sourced from. Anything genuinely undecided is marked `ASSUMPTION` in the source.

---

## Credits and scope

Inspired by the little robot from *Love, Death & Robots*, but the face here is
original geometry drawn from primitives — no artwork, fonts or other assets from
the show are included, and this is not affiliated with or endorsed by its rights
holders.

Weather by [Open-Meteo](https://open-meteo.com/). Built on
[LVGL](https://lvgl.io/), [LovyanGFX](https://github.com/lovyan03/LovyanGFX),
[ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer),
[ArduinoJson](https://arduinojson.org/) and
[ElegantOTA](https://github.com/ayushsharma82/ElegantOTA).

> The weather request uses TLS without certificate validation. It carries only
> public, non-sensitive data in both directions, but it is a deliberate tradeoff
> rather than an oversight — worth knowing if you extend it to anything else.
