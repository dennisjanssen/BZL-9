# BZL-9

A small desk companion. An ESP32-C6 drives a 1.47" LCD showing a procedurally
animated face — it blinks, looks around, yawns, gets bored, wears sunglasses when
the sun is out, and nags you to drink water. The screen is meant to be mounted as
the visor of a 3D-printed helmet, so the whole display is the face.

Everything is configured from a built-in web portal. There is no app and no cloud
service; the only outbound request is a weather lookup.

The face is drawn entirely from primitives at runtime — there are no image assets.

Source and issues: <https://github.com/dennisjanssen/BZL-9>

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
git clone https://github.com/dennisjanssen/BZL-9.git && cd BZL-9
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
5. Save. It reconnects to your network and is then reachable at
   **<http://bzl9.local>** — it registers that name over mDNS on every
   connect. The IP is also printed to the serial log and shown on the
   dashboard, in case mDNS is blocked on your network.

The AP is deliberately open. It carries no secrets until you type them into it,
and requiring a password you cannot see anywhere is a poor first-run experience.
Set your Wi-Fi and it stops advertising.

If the face is upside down in your enclosure, tick **Flip display 180°**. It
applies immediately, no reboot.

---

## What it does

**Day moods** — cycles at random between `neutral`, `bored` and `excited`,
holding each for 3–10 minutes. Each has its own resting pose: eye height, mouth
shape, head tilt, breathing rate, and how often it fidgets. Eyebrow angle is faked
by masking the top of each eye, so `bored` looks unimpressed.

`focused` exists as a further mood but is **never drawn at random** — it is
reserved for external callers, so that when something sets it (the Claude Code
hooks below, or the dashboard) you know the face is reacting to that and not just
idling. A signal you cannot tell apart from ordinary behaviour is not a signal.
While `focused` is held, the weather cameos and random glitches are suppressed
for the same reason: sunglasses dropping onto the face mid-task would undo it.
Hydration and movement reminders still fire — those are for you, not decoration.

You can pin a mood from the dashboard, or leave it on **Auto**. A pinned mood is
dropped overnight so a forgotten one does not outlive the day.

**Sleep schedule** — gets visibly sleepy for a configurable window before your
workday ends (heavy lids, drooping head, yawning), then sleeps once it is over:
eyes shut, and a snore — the face rises as the mouth falls open and a bubble
inflates from it, then pops. Brightness fades to the sleep level.

**Reminders** — on their own intervals, it drinks from a blue water bottle that
visibly empties over four sips, and for the movement reminder it gets up
and walks a couple of laps around the visor.

**Weather** — fetched from [Open-Meteo](https://open-meteo.com/) (no API key).
Sunglasses drop onto its face when it is clear, it shivers when it snows, rain
streaks fall when it rains, and when rain is *forecast later today* a small cloud
drifts in and it recoils from it, wide-eyed.

**Expressions** — one-shot reactions from the dashboard: `shock`, `heart`, `rage`,
`sleepy`, `glitch`, `hydrate`, `unimpressed`, `grin`, `wave`, `whistle`,
`movement`. `hydrate` and `movement` each run one cycle of the
corresponding reminder, so you can see them without waiting for the
interval.

`wave` raises a small hand beside the face and waves it — meant for "I need you"
rather than the alarm that `shock` conveys. `whistle` purses the mouth into a
small circle that keeps changing pitch, sways and bobs the head, and sends music
notes drifting off for about five seconds; it also fires on its own now and then
as an idle flourish.

**Status LED** — the onboard WS2812. Off by default; pick any colour in the
portal. Note this board's LED is wired **RGB, not the usual GRB** — if you port
this and red/green come out swapped, that is why.

---

## Claude Code integration

The device answers to **`bzl9.local`** on your network (mDNS), and the mood and
expression endpoints hold no state in flash — they can be called as often as you
like. That makes it easy to drive the face from
[Claude Code hooks](https://code.claude.com/docs/en/hooks), so you can tell at a
glance whether Claude is working or waiting for you.

**Use `focused`, not one of the other moods.** It is the one mood the random
rotation never picks, so if the face is focused, something asked for it — no
other mood can give you that. While it is held, the weather cameos and random
glitches are suppressed too, so nothing decorative interrupts the signal.
Substituting `bored` or `excited` would work mechanically and tell you nothing,
because the face reaches those on its own.

Put this in `~/.claude/settings.json` (all projects) or `.claude/settings.json`
(one project):

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "timeout": 5,
            "command": "curl -s -X POST http://bzl9.local/api/mood -H \"Content-Type: application/json\" -d \"{\\\"mood\\\":\\\"focused\\\"}\" --max-time 2"
          }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          {
            "type": "command",
            "timeout": 5,
            "command": "curl -s -X POST http://bzl9.local/api/mood -H \"Content-Type: application/json\" -d \"{\\\"mood\\\":\\\"auto\\\"}\" --max-time 2"
          }
        ]
      }
    ],
    "Notification": [
      {
        "matcher": "permission_prompt",
        "hooks": [
          {
            "type": "command",
            "timeout": 5,
            "command": "curl -s -X POST http://bzl9.local/api/express -H \"Content-Type: application/json\" -d \"{\\\"expression\\\":\\\"wave\\\"}\" --max-time 2"
          }
        ]
      }
    ],
    "SessionEnd": [
      {
        "hooks": [
          {
            "type": "command",
            "timeout": 5,
            "command": "curl -s -X POST http://bzl9.local/api/mood -H \"Content-Type: application/json\" -d \"{\\\"mood\\\":\\\"auto\\\"}\" --max-time 2"
          }
        ]
      }
    ]
  }
}
```

The content-type header is not optional — the device rejects the request without
it. The inner quotes are escaped twice because the shell command is itself a
JSON string.

### Or use the wrapper (recommended on Windows)

`tools/bzl9.cmd` does the same thing without the nested quoting, and is what the
hooks should call on Windows — it does not care which shell Claude Code invokes,
and it **always exits 0**:

```json
"command": "\"C:\\path\\to\\BZL-9\\tools\\bzl9.cmd\" working"
```

with `working`, `waiting` or `attention` as the argument. Check connectivity
first with:

```
tools\bzl9.cmd test
```

which prints `/api/status` and tells you whether the hooks will work.

> **Why always exit 0.** `Stop` is a *blockable* hook event — a hook exiting
> with code 2 stops Claude's turn — and curl uses exit 2 for "failed to
> initialize". Without that guard, unplugging the device could hang your
> assistant. The wrapper makes it impossible.

Edit `HOST` at the top of the script if you use an IP instead of mDNS.

| Hook | Fires | Face |
|---|---|---|
| `UserPromptSubmit` | before Claude starts a turn | settles into `focused` |
| `Stop` | Claude has finished, waiting on you | back to the mood rotation |
| `Notification` (`permission_prompt`) | Claude is blocked on approval | raises a hand and waves |
| `SessionEnd` | session closes | releases |

Two things worth knowing:

- **`Stop` is a blockable event** — a hook exiting with code 2 stops Claude's
  turn. Curl only uses exit 2 for "failed to initialize", but it is worth making
  the command incapable of returning it (append `|| true`, or wrap it in a script
  that always exits 0) rather than risking your assistant hanging because a desk
  ornament did not answer.
- **`--max-time` matters.** Hooks run synchronously; without a cap, an
  unreachable device would stall each turn until the hook timeout.

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
| Movement reminder | 45 min | 5–480 |
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
| `GET /api/status` | state, uptime, Wi-Fi, weather, `moodOverride` |
| `GET /api/config` | current settings (**never** returns passwords) |
| `POST /api/config` | partial update; only the keys you send change |
| `POST /api/express` | `{"expression": "…"}` |
| `POST /api/mood` | `{"mood": "…"}` |
| `POST /api/reboot` | |

`POST /api/mood` accepts `auto`, `neutral`, `bored`, `excited`, `focused` and
`sleepy`. `auto` returns to the random rotation; anything else pins that mood
until changed, and is dropped overnight.

`POST /api/express` accepts `shock`, `heart`, `rage`, `sleepy`, `glitch`,
`hydrate`, `unimpressed`, `grin`, `wave`, `whistle` and `movement`. These
are one-shot and release themselves.

Both take `Content-Type: application/json`, reject anything outside those lists
with HTTP 400, and hold no state in flash — safe to call as often as you like.
`POST /api/config` is the exception: it writes NVS, so **do not** drive it from
anything that fires per-turn.

Note `/api/status` is a **1 Hz snapshot**, so a change you just POSTed may take a
second to appear there.

---

## OTA updates

Served at `/update` (ElegantOTA), behind HTTP basic auth as user `admin`.

> **The default password is `bzl9-setup`.** Change it in the portal before putting
> this on a network you share. It defaults to something rather than nothing
> because an empty password would leave the update endpoint wide open out of the box.

OTA writes to the inactive app slot, so a bad image cannot brick the device.

**Filesystem updates work too.** ElegantOTA's page offers a "firmware" and a
"filesystem" mode; upload `.pio/build/esp32-c6-bzl9/littlefs.bin` in the latter to
push dashboard changes without a cable.

This only works because the filesystem partition declares subtype **`spiffs`**
while keeping the *name* `littlefs`. Nothing is SPIFFS-formatted — it is LittleFS
throughout — but the Arduino `Update` library looks up
`ESP_PARTITION_SUBTYPE_DATA_SPIFFS` (`0x82`) and would never find `0x83`, so with
the obvious subtype the mode silently failed. `LittleFS.begin(..., "littlefs")`
locates the partition by *label*, so both coexist. Don't "correct" that subtype.

---

## Layout and limits

4 MB of flash, partitioned as two 1.875 MB OTA slots plus 192 KB LittleFS:

```
nvs       0x009000   20 KB    settings (survives everything below)
otadata   0x00e000    8 KB
app0      0x010000  1.875 MB
app1      0x1F0000  1.875 MB
littlefs  0x3D0000  192 KB    web assets (~15 KB used)
```

The firmware sits at about **83%** of one OTA slot, leaving ~329 KB.

**App partition offsets must be 64 KB aligned** — `gen_esp32part.py` enforces
`ALIGNMENT[APP_TYPE] = 0x10000` and refuses anything else. That is what fixes the
slot size at `0x1E0000` and leaves 192 KB for the filesystem rather than a round
128 KB. Shrinking the filesystem below that buys **no** extra app space; it just
strands the difference as an unusable gap.

If you ever need more, roughly 36 KB of unused LVGL widget classes are still
linked because the default theme references every enabled widget — disabling the
ones this project never creates is the next win, though note the theme is also
what supplies `arc_rounded` for the mouth's rounded ends.

**No font is compiled in.** `LV_FONT_DEFAULT` is `NULL` and every montserrat font
is disabled, because the display draws nothing but shapes — that is worth ~14 KB.
This is only safe while nothing renders text: LVGL stores the pointer in theme and
draw descriptors but never dereferences it unless a label or a symbol background
image is actually drawn. **Re-enable a font in `include/lv_conf.h` before adding
any `lv_label`.**

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

## License

This project's own code is **MIT** — see [LICENSE](LICENSE). Use it, change it,
build on it, put it in something you sell. The one condition is that the
copyright notice travels with it, so nobody can pass the work off as entirely
their own.

If you build something from this, a link back is appreciated but not required.

### Dependency licenses matter if you ship binaries

The libraries are fetched by PlatformIO rather than vendored here, so this
repository is MIT throughout. A **compiled firmware image** is another matter,
because it links:

| Library | License |
|---|---|
| LVGL | MIT |
| ArduinoJson | MIT |
| LovyanGFX | MIT AND BSD-2-Clause |
| ESPAsyncWebServer | **LGPL-3.0** |
| AsyncTCP | **LGPL-3.0** |
| ElegantOTA 3.x | **AGPL-3.0** |

ElegantOTA in particular is AGPL-3.0, which is strong copyleft and is triggered
by network use — and this project uses it to serve a web page. In practice that
means anyone **distributing built firmware** owes recipients the complete
corresponding source under compatible terms. Publishing your fork's source, as
this repo does, satisfies that; shipping closed binaries would not.

If you want to distribute a closed-source product built on this, ElegantOTA sells
a commercial license, or you can replace it with your own upload endpoint.

*Not legal advice — just the licenses as they actually stand, so nobody is
surprised later.*

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
