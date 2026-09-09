#pragma once

// =============================================================================
// HARDWARE IDENTITY -- CORRECTED 2026-09-02
//
// The brief was written assuming an ESP32-S3 board (Waveshare
// ESP32-S3-LCD-1.47 family). On first flash, esptool reported the actual
// chip as ESP32-C6 -- a different architecture entirely (single RISC-V
// core, no PSRAM support, no Xtensa toolchain). The original Stage 1 files
// (sourced from the S3 demo) were wrong for this hardware and have been
// replaced.
//
// The marketplace listing text for this board reads: "ESP32 C6 Development
// Board 262K Color ST7789 1.47 inch LCD Display Screen For Arduino
// WiFi6/BT/LVGL/HMI Onborad SD-Port/RGB-LED" -- which matches Waveshare's
// ESP32-C6-LCD-1.47 (the NON-touch variant -- note there is also a
// visually-similar ESP32-C6-Touch-LCD-1.47 with a different display driver
// chip (JD9853) and no RGB LED; that is NOT this board).
//
// SOURCE: Waveshare's own official demo package for the ESP32-C6-LCD-1.47,
// downloaded 2026-09-02 from
// https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip
// (linked from https://www.waveshare.com/wiki/ESP32-C6-LCD-1.47), reproduced
// from Arduino/examples/LVGL_Arduino/Display_ST7789.h and Display_ST7789.cpp,
// cross-checked against the wiki's own "Interface Introduction" table and
// (for the RGB LED pin) the ESP-IDF demo's main/RGB/RGB.h. The ST7789 init
// command sequence in that .cpp is byte-for-byte identical to the S3 board's
// -- this really is a plain ST7789, not a vendor-specific driver IC, so
// LovyanGFX's stock Panel_ST7789 applies unmodified.
//
// CAVEAT: this project's board was bought through a third-party marketplace
// listing, not directly from Waveshare -- it may be a clone reusing this
// reference design, which is what the marketplace title's phrasing suggests.
// This pin map has NOT been checked against the physical board yet (that's
// exactly what happened last time, and it turned out to be the wrong chip
// family entirely). Stage 1's test pattern is the verification step -- a
// wrong CS/DC pin shows as a blank screen, a wrong offset shows as a missing
// edge or a band of noise. Do not trust this file until that test pattern
// renders correctly on the real hardware.
// =============================================================================

// --- Display (ST7789, 172x320) ---
#define LCD_PIN_SCLK   7
#define LCD_PIN_MOSI   6
#define LCD_PIN_MISO   5   // shared with the TF card's MISO, see below
#define LCD_PIN_CS     14
#define LCD_PIN_DC     15
#define LCD_PIN_RST    21
#define LCD_PIN_BL     22   // backlight, LEDC PWM

// ESP32-C6 has exactly one general-purpose SPI peripheral (GPSPI2), so
// unlike the S3 there is no FSPI/HSPI ambiguity to guess at -- the vendor
// demo just uses the default global `SPI` object, which maps to SPI2_HOST.
#define LCD_SPI_HOST      SPI2_HOST
#define LCD_SPI_CLOCK_HZ  80000000

// Native panel geometry, in the panel's OWN (portrait) orientation. These
// go straight to the LovyanGFX panel config and must never be swapped for
// rotation: LovyanGFX derives both the rotated size and the rotated GRAM
// offsets itself, from these plus memory_width/memory_height (see
// Panel_LCD::setRotation).
#define LCD_PANEL_WIDTH   172
#define LCD_PANEL_HEIGHT  320

// Logical (post-rotation) screen geometry -- what LVGL and every bit of
// face layout works in. Landscape, because the screen is the visor of the
// helmet and a visor is wider than it is tall.
#define LCD_WIDTH   320
#define LCD_HEIGHT  172

// Panel offset -- same reasoning and same values as the S3 board's ST7789
// (172-wide panel inside a 240-wide GRAM), confirmed from this board's own
// Display_ST7789.h. The offsets swap between x and y when rotation changes.
#define LCD_OFFSET_X   34
#define LCD_OFFSET_Y   0

// Landscape mounting. Rotation 1 and 3 are the same landscape shape, 180
// degrees apart, so which one is correct depends only on which way up the
// board ended up in the helmet -- hence the runtime `displayFlipped`
// config field rather than a compile-time choice.
//
// Both are safe with the offsets above: LovyanGFX computes colstart from
// (memory_width - (panel_width + offset)) for the mirrored rotations, so
// for this 172x320 panel in a 240x320 GRAM both landscape rotations land
// on colstart 0 / rowstart 34. Verified against Panel_LCD::setRotation --
// not assumed.
#define LCD_ROTATION           1   // 90 deg: landscape
#define LCD_ROTATION_FLIPPED   3   // 270 deg: landscape, mounted the other way up

// --- Onboard addressable RGB LED (WS2812) ---
// Confirmed three ways: this board's own ESP-IDF demo (main/RGB/RGB.h,
// `#define BLINK_GPIO 8`), the wiki's RGB LED table, and the Arduino C6
// variant's `#define PIN_RGB_LED 8`.
// NOW IN USE (see StatusLed) -- colour and on/off are portal-configurable.
// Driven with the core's rgbLedWrite(), which owns its own RMT setup;
// nothing else in this project uses RMT.
#define RGB_LED_PIN    8

// --- mDNS ---
// The device answers to "<MDNS_HOSTNAME>.local" once it joins a network, so
// scripts and hooks do not have to chase a DHCP-assigned IP. Keep it short
// and lowercase: mDNS labels are case-insensitive and some resolvers are
// fussy about anything exotic.
#define MDNS_HOSTNAME  "bzl9"

// --- BOOT button ---
// UNCONFIRMED. Neither the wiki's onboard-resources text nor the Arduino/
// ESP-IDF demo source (checked) states the GPIO for this board's BOOT
// button -- unlike RGB_LED_PIN above, I could not source this one, so it is
// deliberately left undefined rather than guessed (many ESP32-C-series
// boards use GPIO9, but that is a pattern, not a confirmed fact for this
// board). Not used in Stage 1 regardless; fill in from the schematic
// (files.waveshare.com/wiki/ESP32-C6-LCD-1.47/...) before any stage that
// needs it.

// --- microSD / TF card ---
// UNLIKE the S3 board, this bus is SHARED with the LCD: SCLK/MOSI/MISO
// (GPIO7/6/5) are the same physical SPI lines as the display, only CS
// differs. Per the brief ("If the microSD slot shares the SPI bus with the
// panel... the project must either arbitrate the bus or leave the card
// unused. Leave it unused.") -- so SD_PIN_CS is recorded for completeness
// but the card is not initialized anywhere in this project.
#define SD_PIN_MISO  5
#define SD_PIN_MOSI  6
#define SD_PIN_SCLK  7
#define SD_PIN_CS    4

// --- Backlight PWM ---
// Vendor demo: ledcAttach(pin, 1000 Hz, 10-bit resolution), duty scaled
// from a 0-100 "Light" percentage as Light*10 (i.e. linear, not perceptual
// -- section 5 of the brief calls for a perceptual curve, applied on top of
// this raw PWM layer in DisplayEngine).
#define BACKLIGHT_PWM_FREQ_HZ    1000
#define BACKLIGHT_PWM_RESOLUTION 10  // bits -> duty range 0-1023

// The vendor wiki explicitly warns: keep brightness at 50% or lower for
// sustained use -- running the panel at full brightness for extended
// periods can overheat it and cause permanent dark shadows. DisplayEngine's
// perceptual brightness curve (Stage 2+) must respect this, not just the
// raw PWM range.
#define BACKLIGHT_MAX_SUSTAINED_PERCENT 50
