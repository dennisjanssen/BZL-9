#include "DisplayEngine.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include <cmath>
#include <cstring>

#include "Config.h"
#include "ConfigStore.h"

namespace {

// --- Panel (moved here from Stage 1's main.cpp, per that file's own note) ---
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;
  lgfx::Light_PWM _light_instance;

public:
  LGFX() {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = LCD_SPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = LCD_SPI_CLOCK_HZ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = LCD_PIN_SCLK;
      cfg.pin_mosi = LCD_PIN_MOSI;
      cfg.pin_miso = LCD_PIN_MISO;
      cfg.pin_dc = LCD_PIN_DC;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs = LCD_PIN_CS;
      cfg.pin_rst = LCD_PIN_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = LCD_PANEL_WIDTH;
      cfg.panel_height = LCD_PANEL_HEIGHT;
      cfg.offset_x = LCD_OFFSET_X;
      cfg.offset_y = LCD_OFFSET_Y;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl = LCD_PIN_BL;
      cfg.invert = false;
      cfg.freq = BACKLIGHT_PWM_FREQ_HZ;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX display;

// --- LVGL plumbing ---
// Two partial buffers, each a quarter of the screen, in internal
// DMA-capable RAM -- per brief section 4, no full-frame buffer.
constexpr int32_t kBufRows = LCD_HEIGHT / 4;
constexpr uint32_t kBufPixels = static_cast<uint32_t>(LCD_WIDTH) * kBufRows;

lv_disp_draw_buf_t drawBuf;
lv_color_t *buf1 = nullptr;
lv_color_t *buf2 = nullptr;
lv_disp_drv_t dispDrv;

void dispFlush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  display.startWrite();
  display.setAddrWindow(area->x1, area->y1, w, h);
  // The `swap` argument is misleadingly named -- per LovyanGFX's own
  // create_pc_fast(uint16_t*, bool swap): swap=true means "treat my
  // source as already in the panel's native rgb565_t byte order, no
  // reordering needed"; swap=false means "assume the source is
  // byte-swapped and correct for it". Our LVGL buffers (LV_COLOR_16_SWAP=0
  // in lv_conf.h) are natively ordered, so this must be true -- passing
  // false was quietly byte-swapping every colored (non-symmetric) pixel
  // on the panel, invisible for white/black but very visible for actual
  // colors (traced: crimson (0xE0C7) byte-swapped becomes 0xC7E0, which
  // decodes to R=192 G=252 B=0 -- bright green. That's the reported bug.)
  display.writePixels(reinterpret_cast<uint16_t *>(&color_p->full), w * h, true);
  display.endWrite();
  lv_disp_flush_ready(drv);
}

// --- Face geometry ---
// ASSUMPTION: pixel values below implement the round-eyes / small-curved-
// mouth direction confirmed against a mockup, on the native 172x320 panel.
constexpr lv_coord_t kFaceCenterX = LCD_WIDTH / 2;  // 86

// Eyes are pill-shaped (a vertical capsule), not circular -- same width,
// taller. Width and height are separate constants because a single
// "diameter" was previously doing duty as both, which is what made the
// eyes necessarily round. LV_RADIUS_CIRCLE clamps to min(w,h)/2, so one
// radius gives a capsule at rest AND still gives a rounded slit when a
// blink squashes the height down to a couple of pixels.
constexpr lv_coord_t kEyeWidth = 40;
constexpr lv_coord_t kEyeHeight = 54;
constexpr lv_coord_t kEyeGap = 84;  // widened for the landscape visor
// Anchored by CENTRE rather than by top edge: the eyes have been resized
// twice now, and deriving the top from a fixed centre keeps the face where
// it was verified to sit on the visor instead of drifting up as the eyes
// shrink. Everything vertical hangs off this.
constexpr lv_coord_t kEyeCenterY = 60;
constexpr lv_coord_t kEyeTop = kEyeCenterY - kEyeHeight / 2;  // 120
constexpr lv_coord_t kEyeLeftX = kFaceCenterX - kEyeGap / 2 - kEyeWidth;  // 30
constexpr lv_coord_t kEyeRightX = kFaceCenterX + kEyeGap / 2;             // 96
constexpr lv_coord_t kEyeBlinkMinHeight = 6;
constexpr lv_coord_t kFaceCenterY = kEyeCenterY;
constexpr lv_coord_t kEyeLeftCenterX = kEyeLeftX + kEyeWidth / 2;
constexpr lv_coord_t kEyeRightCenterX = kEyeRightX + kEyeWidth / 2;

// Head yaw, used by the shock reaction. Features ride a cylinder of
// radius kYawRadius: each one's horizontal offset remaps to R*sin(theta)
// and its width foreshortens by cos(theta). The eye turning away narrows
// and slides toward the edge while the other swings toward centre, and
// the mouth (nearest the axis) travels furthest -- that differential is
// what reads as a head turning in space rather than a flat picture
// sliding sideways.
// kYawRadius has to comfortably exceed the largest feature offset from
// the face centre (the eye centres, now +-62px): the cylinder model takes
// asinf(offset/R), which returns NaN the moment |offset| > R. Sized as a
// plausible head radius for a 320-wide face, keeping offset/R at ~0.48 --
// the same ratio the portrait layout had, so the turn keeps the angular
// character that worked there.
constexpr float kYawRadius = 130.0f;
constexpr float kYawMaxRad = 0.4538f;  // 26 degrees

// Pitch is modelled directly instead of on the cylinder. At the shallow
// angles a 172px-tall screen leaves room for, a true cylinder gives only
// ~2% eye foreshortening, which reads as nothing at all -- so a nod is an
// explicit vertical shift, a much stronger eye squash, and extra travel
// on the mouth, which sits furthest from the axis.
constexpr float kPitchShiftPx = 11.0f;
constexpr float kPitchSpread = 0.35f;   // how much feature spacing opens/closes
constexpr float kPitchSquash = 0.24f;   // peak eye-height foreshortening

constexpr lv_coord_t kMouthRadius = 30;
constexpr lv_coord_t kMouthCenterY = 102;
// Resting span widened from 70 deg (55..125) to 92 deg at the user's
// request -- chord across the curve goes from ~34px to ~43px, which sits
// better under the 124px-wide eye pair without becoming a grin.
constexpr uint16_t kMouthAngleStart = 44;
constexpr uint16_t kMouthAngleEnd = 136;
constexpr lv_coord_t kMouthArcWidth = 8;
// The "open" channel thickens the arc band. lv_arc draws the band centred
// on its radius, so it reaches kMouthArcWidthMax/2 outside kMouthRadius;
// the object box has to be big enough to contain that, and the indicator
// padding pulls the drawn radius back to kMouthRadius. Getting this wrong
// clips the mouth rather than failing loudly.
constexpr lv_coord_t kMouthArcWidthMax = 18;
constexpr lv_coord_t kMouthBoxHalf = kMouthRadius + kMouthArcWidthMax / 2 + 3;  // 42
constexpr lv_coord_t kMouthIndicPad = kMouthBoxHalf - kMouthRadius;             // 12
// Degrees the curve slides around its circle at full skew -- one corner
// lifts while the other drops, which is what reads as a smirk.
constexpr float kMouthSkewDegrees = 14.0f;

lv_obj_t *eyeLeft = nullptr;
lv_obj_t *eyeRight = nullptr;
lv_obj_t *mouth = nullptr;

// The filled "open mouth" shape, swapped in for the arc. Two modes: the
// tall oval the shock reaction uses, and a true circle that opens and
// closes for yawning and snoring. The yawn used to be done by curling the
// arc itself into a near-closed ring, which gave a big 78px donut rather
// than a round mouth -- a filled circle is the only way to actually get
// one, since lv_arc clamps its band width to its radius and so always
// leaves a hole in the middle.
enum class GapeMode : uint8_t { HIDDEN, SHOCK, ROUND };
lv_obj_t *wideOpenMouth = nullptr;
GapeMode gapeMode = GapeMode::HIDDEN;
bool snoring = false;

constexpr lv_coord_t kGapeShockW = 26;
constexpr lv_coord_t kGapeShockH = 34;
constexpr lv_coord_t kGapeRoundMin = 8;
constexpr lv_coord_t kGapeRoundMax = 34;
lv_obj_t *heartCanvas[2] = {nullptr, nullptr};
bool heartEyesVisible = false;

// Eyebrow mimicry, per mood. There is deliberately NO separate eyebrow
// object -- that was proposed early on and the user rejected it in favour
// of changing the eye itself. So this is a black wedge drawn over the top
// of each eye on a transparent canvas: on a black visor the part outside
// the eye is invisible, while the part inside cuts the eye's top edge at
// an angle. The slope is what carries the mood (inner-down reads
// determined, outer-down reads tired).
lv_obj_t *browCanvas[2] = {nullptr, nullptr};
bool browsVisible = false;

// The sustained pose the current mood asked for. applyMoodState() only
// APPLIES a pose when the state changes, so without this an expression had
// nothing to hand the face back to -- it restored hardcoded NEUTRAL values
// and a bored or focused face stayed wrong until the mood next rolled, up
// to ten minutes later.
struct MoodPose {
  lv_coord_t eyeHeight;
  uint16_t mouthStart;
  uint16_t mouthEnd;
  lv_coord_t eyeDriftX;
  float basePitch;
  lv_coord_t browInner;
  lv_coord_t browOuter;
};
MoodPose currentPose = {kEyeHeight, kMouthAngleStart, kMouthAngleEnd, 0, 0.0f, 0, 0};
constexpr lv_coord_t kBrowW = 44;  // slightly wider than the eye, so it always spans it
constexpr lv_coord_t kBrowH = 26;
constexpr lv_coord_t kBrowOverhang = 4;  // wedge starts this far above the eye's top edge

// Sustained mood pitch, composited on top of the animated headPitch
// channel. Kept separate from the channel precisely because
// setIdleLevel() zeroes the channels when a mood takes the face over --
// a sustained droop has to survive that.
float basePitch = 0;

// --- Waving hand ---
// For the "I need you" reaction. Sits in the empty strip to the right of
// the face, above the water bottle -- their y ranges do not overlap, so a
// hydration reminder and a wave can coexist without collision.
lv_obj_t *handCanvas = nullptr;
constexpr lv_coord_t kHandW = 38;
constexpr lv_coord_t kHandH = 50;
constexpr lv_coord_t kHandCx = 286;
constexpr lv_coord_t kHandCy = 52;
// Rotate about the wrist, not the middle, or it pinwheels instead of waving.
constexpr lv_coord_t kHandPivotX = kHandW / 2;
constexpr lv_coord_t kHandPivotY = kHandH - 2;
// How far the face slides left to clear the hand. See the note at the call
// site: most of this is spent cancelling the head turn.
constexpr lv_coord_t kWaveShiftPx = -26;

// --- Whistling ---
// The mouth is the existing round gape held small and wobbling; the note
// is its own canvas that drifts up and away. `whistling` is an OWNERSHIP
// flag in the same sense as `snoring`: several routines release the gape
// when they finish, and they all have to be told that something sustained
// is holding it.
lv_obj_t *noteCanvas[2] = {nullptr, nullptr};
bool whistling = false;
// Set while the mood is FOCUSED. That mood is the "Claude is working"
// signal, so nothing decorative may interrupt it -- sunglasses dropping on
// mid-task is exactly the confusion this avoids.
bool cameosSuppressed = false;
// Defined with the whistle machinery further down. A no-op unless a
// whistle is actually running; clearExpressionDecorations() needs it, and
// sits above the definition.
void restartWhistleMotion();
// Also defined below: the idle flourish scheduler sits above it.
void playWhistle();
// TWO of them, alternating. With one canvas, spawning a note before the
// previous had finished drifting simply restarted it -- the first vanished
// mid-flight. Two lets them overlap, which is what makes a faster stream
// read as whistling rather than as one note stuttering.
bool noteVisible[2] = {false, false};
float noteProgress[2] = {0.0f, 0.0f};
int nextNoteSlot = 0;
constexpr lv_coord_t kNoteW = 16;
constexpr lv_coord_t kNoteH = 24;
// Spawns just off the mouth and drifts up and to the right.
constexpr lv_coord_t kNoteBaseX = kFaceCenterX + 34;
constexpr lv_coord_t kNoteBaseY = kMouthCenterY + kMouthRadius - 4;
constexpr lv_coord_t kWhistleSwayTenths = 38;  // +-3.8 degrees of head sway
constexpr lv_coord_t kWhistleBobPx = 3;
// A whistle is now a brief flourish, not a mood: long enough for four or
// five notes, short enough to stay a moment.
constexpr uint32_t kWhistleDurationMs = 5500;
constexpr float kNoteDriftX = 26.0f;
constexpr float kNoteDriftY = 46.0f;

// --- Snore bubble ---
// Replaces the old "Zzz" text label. Two reasons: text needed a whole
// font compiled in (~13.5KB of flash for three glyphs), and an inflating
// bubble that pops reads as sleeping without asking anyone to read
// anything. Unlike the bottle and the cloud this DOES composite through
// renderFace(), because it belongs to the face -- it comes out of its
// mouth and has to move with it.
lv_obj_t *snoreBubble = nullptr;
bool snoreBubbleVisible = false;
constexpr lv_coord_t kBubbleMin = 5;
constexpr lv_coord_t kBubbleMax = 24;
// Just off the side of the mouth, clear of the right eye above it.
constexpr lv_coord_t kBubbleCx = kFaceCenterX + 40;
constexpr lv_coord_t kBubbleCy = kMouthCenterY + kMouthRadius - 6;

// Defined with the snore machinery further down; clearExpressionDecorations
// needs it before that point.
void hideSnoreBubble();

// Glitch channel-split ghosts. Declared up here because renderFace() has
// to composite them: they ARE the eyes drawn offset, so they must be built
// from the same live geometry or the split drifts off the face. The old
// version positioned them once at a fixed kEyeTop and got away with it
// only because the glitch was brief and completely static.
lv_obj_t *glitchGhostRedL = nullptr;
lv_obj_t *glitchGhostRedR = nullptr;
lv_obj_t *glitchGhostCyanL = nullptr;
lv_obj_t *glitchGhostCyanR = nullptr;
bool glitchActive = false;
float glitchSplitX = 5.0f;
float glitchSplitY = 0.0f;

// Sunglasses (the sunny-weather overlay). Declared up here with the other
// face objects because renderFace() has to composite them -- they sit on
// the eyes, so they have to travel with them.
lv_obj_t *sunglassesL = nullptr;
lv_obj_t *sunglassesR = nullptr;
lv_obj_t *sunglassGlare[2] = {nullptr, nullptr};
lv_obj_t *sunglassBridge = nullptr;
bool sunglassesVisible = false;

// White rounded lenses joined by a bridge, with a black glare streak on
// each. White rather than black because the visor is black: a dark lens
// would read as the eyes being deleted rather than as eyewear. Each lens
// fully covers the eye it sits on (the previous version banded across the
// middle of the eye, which is what showed up as a bar).
constexpr lv_coord_t kLensW = kEyeWidth + 8;   // 54
constexpr lv_coord_t kLensH = kEyeHeight + 8;  // 68
// DERIVED, not a literal: the bridge has to span the gap between the two
// lenses and overlap each of them a little. It was a hardcoded 24 from the
// portrait layout, where the eyes were 20px apart -- once the landscape
// relayout pushed the eye gap to 84 that left a 24px bar floating in a
// 76px void, touching neither lens. Anything that spans between the eyes
// must be computed from the eye positions.
constexpr lv_coord_t kBridgeW = (kEyeRightCenterX - kEyeLeftCenterX) - kLensW + 14;
constexpr lv_coord_t kBridgeH = 6;
constexpr lv_coord_t kBridgeOffsetY = -14;  // sits high, like a real bridge
// Glare is offset up and left inside the lens. These numbers keep the
// streaks within the capsule outline -- at the glare's top edge the lens
// is only ~+-22px wide, not the full +-27, so a centred glare would spill.
constexpr lv_coord_t kGlareW = 26;
constexpr lv_coord_t kGlareH = 30;
constexpr lv_coord_t kGlareOffsetX = -6;
constexpr lv_coord_t kGlareOffsetY = -8;

constexpr lv_coord_t kHeartCanvasW = 60;
constexpr lv_coord_t kHeartCanvasH = 56;

// --- Sustained baseline (mood-driven resting pose) ---
// The "where the face rests" layer. Mood states animate these; the idle
// channels below are applied relative to them.
lv_coord_t baseEyeHeight = kEyeHeight;
uint16_t baseMouthStart = kMouthAngleStart;
uint16_t baseMouthEnd = kMouthAngleEnd;
lv_coord_t baseEyeDriftX = 0;  // sustained horizontal drift, e.g. toward the water bottle

// --- Animation channels ---
// REWORKED for lifelike motion. Previously every animation called
// lv_obj_set_pos/set_size on the eyes directly, which meant only ONE could
// ever run at a time without stomping the others -- so the face was doing
// exactly one discrete thing and was otherwise perfectly frozen. That
// single-track, nothing-in-between quality is what read as mechanical.
//
// Now each animation owns exactly one channel below and nothing else, and
// renderFace() composes all of them into actual object geometry. Because
// the channels are independent, blink / gaze / breathing / tilt / mouth
// can all run CONCURRENTLY at their own rhythms -- which is most of what
// makes something look alive, since real creatures blink mid-glance and
// are never entirely still.
struct Channels {
  float gazeX = 0, gazeY = 0;       // deliberate look direction (saccades)
  float microX = 0, microY = 0;     // involuntary micro-saccade jitter
  float blinkLid = 1.0f;            // 0..1, both lids (blink)
  float squintL = 1.0f, squintR = 1.0f;  // 0..1, per eye (skeptical squint)
  float breathY = 0;                // continuous breathing bob -- never stops
  float tiltTenths = 0;             // head tilt, tenths of a degree
  float faceOffX = 0, faceOffY = 0; // whole-face offset (posture / shake / shiver)
  float mouthBias = 0;              // -1 narrow .. +1 wide, relative to baseline
  float mouthSkew = 0;              // -1 .. +1, slides the curve sideways (smirk)
  float mouthOpen = 0;              // 0 .. 1, thickens the arc band (mouth opening)
  float gapeScale = 0;              // 0 .. 1, size of the round open mouth
  float bubbleScale = 0;            // 0 .. 1, snore bubble inflation
  float headYaw = 0;                // -1 .. +1, pseudo-3D head turn
  float headPitch = 0;              // -1 .. +1, pseudo-3D nod
  float shadesDropY = 0;            // sunglasses slide-in offset, 0 = seated
};
Channels ch;

// Composes every channel + the baseline into final geometry. Cheap enough
// to call from each channel's exec_cb; LVGL coalesces the invalidations.
void renderFace() {
  // Yaw rides a cylinder; pitch is direct (see the constants). Both are
  // guarded on zero so the resting face pays nothing -- renderFace() runs
  // every frame forever because breathing never stops, which makes this
  // the one place that cost would be permanent.
  auto yawOffset = [](float restOff) {
    if (ch.headYaw == 0.0f) return restOff;
    float n = restOff / kYawRadius;
    if (n > 0.95f) n = 0.95f;
    if (n < -0.95f) n = -0.95f;
    return kYawRadius * sinf(asinf(n) + ch.headYaw * kYawMaxRad);
  };
  auto yawScale = [](float restOff) {
    if (ch.headYaw == 0.0f) return 1.0f;
    float n = restOff / kYawRadius;
    if (n > 0.95f) n = 0.95f;
    if (n < -0.95f) n = -0.95f;
    float t0 = asinf(n);
    float sc = cosf(t0 + ch.headYaw * kYawMaxRad) / cosf(t0);
    if (sc < 0.2f) sc = 0.2f;
    if (sc > 1.15f) sc = 1.15f;
    return sc;
  };
  // Animated nod plus whatever sustained droop the current mood holds.
  float pitchT = ch.headPitch + basePitch;
  if (pitchT > 1.0f) pitchT = 1.0f;
  if (pitchT < -1.0f) pitchT = -1.0f;
  auto pitchOffset = [&](float restOff) {
    if (pitchT == 0.0f) return restOff;
    return restOff * (1.0f + pitchT * kPitchSpread) + pitchT * kPitchShiftPx;
  };

  float lidL = ch.blinkLid * ch.squintL;
  float lidR = ch.blinkLid * ch.squintR;
  // The eyes sit on the pitch axis, so they foreshorten without shifting.
  float eyeSquash = 1.0f - fabsf(pitchT) * kPitchSquash;
  lv_coord_t hL = static_cast<lv_coord_t>(baseEyeHeight * lidL * eyeSquash);
  lv_coord_t hR = static_cast<lv_coord_t>(baseEyeHeight * lidR * eyeSquash);
  if (hL < 2) hL = 2;
  if (hR < 2) hR = 2;

  float dx = ch.gazeX + ch.microX + ch.faceOffX + baseEyeDriftX;
  float dy = ch.gazeY + ch.microY + ch.breathY + ch.faceOffY;

  float rad = ch.tiltTenths / 10.0f * (3.14159265f / 180.0f);
  float s = sinf(rad);
  float c = cosf(rad);

  // Rotate each element's centre about the face pivot, then place by
  // top-left. Rotating a circle in place is invisible, so for the eyes it
  // has to be the position that orbits.
  auto placeCentred = [&](lv_obj_t *obj, float cx, float cy, lv_coord_t w, lv_coord_t h) {
    float px = yawOffset(cx - kFaceCenterX) + dx;
    float py = pitchOffset(cy - kFaceCenterY) + dy;
    float rx = kFaceCenterX + px * c - py * s;
    float ry = kFaceCenterY + px * s + py * c;
    lv_obj_set_pos(obj, static_cast<lv_coord_t>(rx - w / 2.0f), static_cast<lv_coord_t>(ry - h / 2.0f));
  };

  float eyeCy = kEyeCenterY;
  // Widths are per-eye, not shared: under yaw they foreshorten by
  // different amounts, and that asymmetry is the whole effect.
  lv_coord_t wL = static_cast<lv_coord_t>(kEyeWidth * yawScale(kEyeLeftCenterX - kFaceCenterX));
  lv_coord_t wR = static_cast<lv_coord_t>(kEyeWidth * yawScale(kEyeRightCenterX - kFaceCenterX));
  if (wL < 4) wL = 4;
  if (wR < 4) wR = 4;
  lv_obj_set_size(eyeLeft, wL, hL);
  lv_obj_set_size(eyeRight, wR, hR);
  placeCentred(eyeLeft, kEyeLeftCenterX, eyeCy, wL, hL);
  placeCentred(eyeRight, kEyeRightCenterX, eyeCy, wR, hR);

  if (glitchActive) {
    // Sized from the real eyes' current width/height, so the split tracks
    // blinks, squints and yaw foreshortening instead of drifting off.
    auto placeGhost = [&](lv_obj_t *o, float cx, lv_coord_t w, lv_coord_t h, float sx, float sy) {
      lv_obj_set_size(o, w, h);
      placeCentred(o, cx + sx, eyeCy + sy, w, h);
    };
    placeGhost(glitchGhostRedL, kEyeLeftCenterX, wL, hL, -glitchSplitX, -glitchSplitY);
    placeGhost(glitchGhostRedR, kEyeRightCenterX, wR, hR, -glitchSplitX, -glitchSplitY);
    placeGhost(glitchGhostCyanL, kEyeLeftCenterX, wL, hL, glitchSplitX, glitchSplitY);
    placeGhost(glitchGhostCyanR, kEyeRightCenterX, wR, hR, glitchSplitX, glitchSplitY);
  }

  if (snoreBubbleVisible) {
    lv_coord_t bd = static_cast<lv_coord_t>(kBubbleMin +
                                            (kBubbleMax - kBubbleMin) * ch.bubbleScale);
    lv_obj_set_size(snoreBubble, bd, bd);
    placeCentred(snoreBubble, kBubbleCx, kBubbleCy, bd, bd);
  }

  // Brows ride each eye's current top edge, so a blink or a squint carries
  // them down with the lid instead of leaving them floating.
  if (browsVisible) {
    placeCentred(browCanvas[0], kEyeLeftCenterX,
                 eyeCy - hL / 2.0f - kBrowOverhang + kBrowH / 2.0f, kBrowW, kBrowH);
    placeCentred(browCanvas[1], kEyeRightCenterX,
                 eyeCy - hR / 2.0f - kBrowOverhang + kBrowH / 2.0f, kBrowW, kBrowH);
  }

  if (heartEyesVisible) {
    placeCentred(heartCanvas[0], kEyeLeftCenterX, eyeCy, kHeartCanvasW, kHeartCanvasH);
    placeCentred(heartCanvas[1], kEyeRightCenterX, eyeCy, kHeartCanvasW, kHeartCanvasH);
  }

  // The sunglasses have to be composited, not placed once at init: they
  // sit on the eyes, so if they don't travel with breathing / gaze /
  // micro-saccades / tilt they visibly detach from the face.
  if (sunglassesVisible) {
    float shadeCy = eyeCy + ch.shadesDropY;
    placeCentred(sunglassesL, kEyeLeftCenterX, shadeCy, kLensW, kLensH);
    placeCentred(sunglassesR, kEyeRightCenterX, shadeCy, kLensW, kLensH);
    placeCentred(sunglassGlare[0], kEyeLeftCenterX + kGlareOffsetX, shadeCy + kGlareOffsetY, kGlareW,
                 kGlareH);
    placeCentred(sunglassGlare[1], kEyeRightCenterX + kGlareOffsetX, shadeCy + kGlareOffsetY,
                 kGlareW, kGlareH);
    placeCentred(sunglassBridge, kFaceCenterX, shadeCy + kBridgeOffsetY, kBridgeW, kBridgeH);
  }

  // Mouth: baseline arc angles, widened/narrowed symmetrically by bias,
  // slid sideways by skew, and thickened by open.
  float half = (static_cast<float>(baseMouthEnd) - baseMouthStart) / 2.0f;
  float mid = (static_cast<float>(baseMouthEnd) + baseMouthStart) / 2.0f +
              ch.mouthSkew * kMouthSkewDegrees;
  float scaled = half * (1.0f + 0.45f * ch.mouthBias);
  if (scaled < 4.0f) scaled = 4.0f;
  if (scaled > 80.0f) scaled = 80.0f;
  // lv_arc_set_start_angle only unwraps a single >360 overflow and takes a
  // uint16_t, so a negative angle would underflow to ~65000. Normalise here.
  auto normAngle = [](float a) {
    while (a < 0.0f) a += 360.0f;
    while (a >= 360.0f) a -= 360.0f;
    return static_cast<uint16_t>(a);
  };
  lv_arc_set_angles(mouth, normAngle(mid - scaled), normAngle(mid + scaled));
  int32_t deg = static_cast<int32_t>(ch.tiltTenths) / 10;
  lv_arc_set_rotation(mouth, static_cast<uint16_t>(deg < 0 ? 360 + deg : deg));

  lv_coord_t arcW = static_cast<lv_coord_t>(
      kMouthArcWidth + (kMouthArcWidthMax - kMouthArcWidth) * ch.mouthOpen);
  lv_obj_set_style_arc_width(mouth, arcW, LV_PART_INDICATOR);

  placeCentred(mouth, kFaceCenterX, kMouthCenterY, kMouthBoxHalf * 2, kMouthBoxHalf * 2);
  if (gapeMode != GapeMode::HIDDEN) {
    lv_coord_t gw = kGapeShockW;
    lv_coord_t gh = kGapeShockH;
    // SHOCK keeps sitting at the arc's centre, which is where it has
    // always been and reads well. The round mouth instead sits on the
    // mouth LINE (the bottom of the arc's circle), because that is where
    // a mouth actually looks like it belongs.
    float gy = kMouthCenterY;
    if (gapeMode == GapeMode::ROUND) {
      gw = gh = static_cast<lv_coord_t>(kGapeRoundMin +
                                        (kGapeRoundMax - kGapeRoundMin) * ch.gapeScale);
      gy = kMouthCenterY + kMouthRadius;
    }
    gw = static_cast<lv_coord_t>(gw * yawScale(0.0f));
    if (gw < 2) gw = 2;
    lv_obj_set_size(wideOpenMouth, gw, gh);
    placeCentred(wideOpenMouth, kFaceCenterX, gy, gw, gh);
  }
}

lv_obj_t *createEye(lv_coord_t x) {
  lv_obj_t *eye = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(eye, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(eye, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_size(eye, kEyeWidth, kEyeHeight);
  lv_obj_set_pos(eye, x, kEyeTop);
  return eye;
}

lv_obj_t *createMouth() {
  lv_obj_t *m = lv_arc_create(lv_scr_act());
  lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_style(m, nullptr, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_arc_set_bg_angles(m, kMouthAngleStart, kMouthAngleEnd);
  lv_arc_set_angles(m, kMouthAngleStart, kMouthAngleEnd);
  lv_obj_set_style_arc_opa(m, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_arc_color(m, lv_color_white(), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(m, kMouthArcWidth, LV_PART_INDICATOR);
  // The box is oversized so a thickened band stays inside it; lv_arc takes
  // its radius from min(w,h)/2 minus the MAIN pad, then subtracts the
  // INDICATOR pad, so pad_all here puts the drawn curve back on
  // kMouthRadius. MAIN pad is pinned to 0 rather than inherited so the
  // radius doesn't depend on the theme.
  lv_obj_set_style_pad_all(m, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(m, kMouthIndicPad, LV_PART_INDICATOR);
  lv_obj_set_size(m, kMouthBoxHalf * 2, kMouthBoxHalf * 2);
  lv_obj_set_pos(m, kFaceCenterX - kMouthBoxHalf, kMouthCenterY - kMouthBoxHalf);
  return m;
}

// --- Idle animation: independent, concurrent behaviours ---
// Each behaviour owns exactly one channel and runs on its own rhythm --
// they deliberately overlap. Every anim sets lv_anim_set_var(&ch) so it
// can be cancelled via lv_anim_del(&ch, cb) and so re-triggering one
// replaces it rather than stacking a duplicate.
enum class IdleLevel { MINIMAL, REDUCED, FULL };
IdleLevel idleLevel = IdleLevel::FULL;

lv_timer_t *blinkSchedTimer = nullptr;
lv_timer_t *gazeSchedTimer = nullptr;
lv_timer_t *flourishSchedTimer = nullptr;
lv_timer_t *microTimer = nullptr;
lv_timer_t *gazeHoldTimer = nullptr;

// -- Breathing: the always-on layer. A perfectly still face reads as
// switched off, so this never stops -- it only slows down while asleep.
void breathExecCb(void *, int32_t centipx) {
  ch.breathY = centipx / 100.0f;
  renderFace();
}

void startBreathing(uint32_t halfPeriodMs, int32_t amplitudeCentipx) {
  lv_anim_del(&ch, breathExecCb);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, breathExecCb);
  lv_anim_set_values(&a, -amplitudeCentipx, amplitudeCentipx);
  lv_anim_set_time(&a, halfPeriodMs);
  lv_anim_set_playback_time(&a, halfPeriodMs);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// -- Micro-saccades: involuntary ~1px jitter. Real eyes never hold
// perfectly steady, and these are instant jumps rather than eased moves --
// easing them would defeat the point.
void microTickCb(lv_timer_t *t) {
  if (idleLevel == IdleLevel::MINIMAL) {
    ch.microX = 0;
    ch.microY = 0;
  } else {
    ch.microX = (static_cast<int32_t>(lv_rand(0, 200)) - 100) / 100.0f;
    ch.microY = (static_cast<int32_t>(lv_rand(0, 140)) - 70) / 100.0f;
  }
  renderFace();
  lv_timer_set_period(t, lv_rand(280, 900));
}

// -- Blink: asymmetric on purpose. Real eyelids snap shut and drift back
// open; the old symmetric 110ms/110ms ease_in_out both ways is exactly
// what made it read as a servo rather than an eye.
int pendingExtraBlinks = 0;

void blinkLidExecCb(void *, int32_t permille) {
  ch.blinkLid = permille / 1000.0f;
  renderFace();
}

void startBlinkClose();

void blinkOpenReadyCb(lv_anim_t *) {
  if (pendingExtraBlinks > 0) {
    pendingExtraBlinks--;
    startBlinkClose();
  }
}

void blinkCloseReadyCb(lv_anim_t *) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, blinkLidExecCb);
  lv_anim_set_values(&a, 60, 1000);
  lv_anim_set_time(&a, 170);  // opens roughly twice as slow as it closes
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_ready_cb(&a, blinkOpenReadyCb);
  lv_anim_start(&a);
}

void startBlinkClose() {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, blinkLidExecCb);
  // Starts from wherever the lid currently is, so a blink interrupting an
  // in-flight one doesn't jump.
  lv_anim_set_values(&a, static_cast<int32_t>(ch.blinkLid * 1000), 60);
  lv_anim_set_time(&a, 80);  // snaps shut
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_ready_cb(&a, blinkCloseReadyCb);
  lv_anim_start(&a);
}

void triggerBlink() {
  if (idleLevel == IdleLevel::MINIMAL) return;
  // ASSUMPTION: ~1 in 5 blinks comes as a quick pair. Roughly how often
  // people double-blink, and it keeps the rhythm off a metronome.
  if (lv_rand(0, 99) < 18) pendingExtraBlinks = 1;
  startBlinkClose();
}

// -- Gaze: no separate pupil in this face design (see face_design memory),
// so "looking" is both eyes shifting together. The key change is the
// *shape* of the movement: a real glance is a fast saccade to a target
// (decelerating into it), a hold of varying length, then another saccade
// -- not a slow symmetric glide out and straight back, which is what this
// used to do and why it looked like a servo sweeping.
float gazeFromX = 0, gazeFromY = 0, gazeToX = 0, gazeToY = 0;
float gazeFromYaw = 0, gazeToYaw = 0, gazeFromPitch = 0, gazeToPitch = 0;
int gazeChainLeft = 0;
bool gazeReturningHome = false;

// Coupling factors from the gaze target to the head. Every glance turns
// the head a little: the eyes lead and the head follows, which is what
// stops a look from reading as the whole face sliding sideways.
constexpr float kGazeYawCoupling = 0.33f;
constexpr float kGazePitchCoupling = 0.30f;

void gazeExecCb(void *, int32_t permille) {
  float t = permille / 1000.0f;
  ch.gazeX = gazeFromX + (gazeToX - gazeFromX) * t;
  ch.gazeY = gazeFromY + (gazeToY - gazeFromY) * t;
  ch.headYaw = gazeFromYaw + (gazeToYaw - gazeFromYaw) * t;
  ch.headPitch = gazeFromPitch + (gazeToPitch - gazeFromPitch) * t;
  renderFace();
}

void gazeArrivedCb(lv_anim_t *);

void saccadeTo(float x, float y, uint32_t timeMs, bool home) {
  gazeFromX = ch.gazeX;
  gazeFromY = ch.gazeY;
  gazeToX = x;
  gazeToY = y;
  gazeFromYaw = ch.headYaw;
  gazeFromPitch = ch.headPitch;
  // Targets are +-13 in x and +-7 in y (see gazePickTarget), so normalise
  // against those. Going home (0,0) unwinds the head to neutral too.
  gazeToYaw = (x / 13.0f) * kGazeYawCoupling;
  gazeToPitch = (y / 7.0f) * kGazePitchCoupling;
  gazeReturningHome = home;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, gazeExecCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, timeMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // decelerates into the target
  lv_anim_set_ready_cb(&a, gazeArrivedCb);
  lv_anim_start(&a);
}

void gazePickTarget() {
  float x = (static_cast<int32_t>(lv_rand(0, 260)) - 130) / 10.0f;
  float y = (static_cast<int32_t>(lv_rand(0, 140)) - 70) / 10.0f;
  saccadeTo(x, y, lv_rand(70, 110), false);
}

void gazeHoldDoneCb(lv_timer_t *) {
  // repeat_count=1: LVGL frees this timer right after we return, so just
  // drop the reference (see hideGlitch() for the double-free this avoids).
  gazeHoldTimer = nullptr;
  if (idleLevel != IdleLevel::FULL) return;
  if (gazeChainLeft > 0) {
    gazeChainLeft--;
    if (lv_rand(0, 99) < 30) triggerBlink();
    gazePickTarget();
  } else {
    saccadeTo(0.0f, 0.0f, 120, true);
  }
}

void gazeArrivedCb(lv_anim_t *) {
  if (gazeReturningHome) return;
  if (gazeHoldTimer != nullptr) lv_timer_del(gazeHoldTimer);
  gazeHoldTimer = lv_timer_create(gazeHoldDoneCb, lv_rand(350, 1700), nullptr);
  lv_timer_set_repeat_count(gazeHoldTimer, 1);
}

void startGazeWander() {
  // ASSUMPTION: ~45% of glances are a multi-point scan rather than a single
  // look-and-return, and ~40% start with a blink -- people blink when they
  // change where they're looking, and borrowing that reads as intent
  // rather than as a mechanism moving.
  gazeChainLeft = (lv_rand(0, 99) < 45) ? static_cast<int>(lv_rand(1, 2)) : 0;
  if (lv_rand(0, 99) < 40) triggerBlink();
  gazePickTarget();
}

// Swaps between the arc mouth and the filled open mouth.
void setGapeMode(GapeMode m) {
  gapeMode = m;
  if (m == GapeMode::HIDDEN) {
    lv_obj_add_flag(wideOpenMouth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(mouth, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(mouth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wideOpenMouth, LV_OBJ_FLAG_HIDDEN);
  }
  renderFace();
}

void gapeScaleExecCb(void *, int32_t permille) {
  ch.gapeScale = permille / 1000.0f;
  renderFace();
}

// Defined further down with the squint flourish; the yawn borrows them so
// the eyes narrow as the mouth opens.
void squintLExecCb(void *, int32_t permille);
void squintRExecCb(void *, int32_t permille);

// -- Mouth: three independent channels on the arc, so the shapes below
// can combine. Width alone only ever gave a wider or narrower smile; skew
// (slides the curve around its circle, lifting one corner) and open
// (thickens the band) are what let the arc do something other than
// stretch. A genuinely round open mouth is not an arc at all -- GapeMode.
void mouthExecCb(void *, int32_t permille) {
  ch.mouthBias = permille / 1000.0f;
  renderFace();
}

void mouthSkewExecCb(void *, int32_t permille) {
  ch.mouthSkew = permille / 1000.0f;
  renderFace();
}

void mouthOpenExecCb(void *, int32_t permille) {
  ch.mouthOpen = permille / 1000.0f;
  renderFace();
}

// Scales both eyes and returns them to rest. Deliberately NOT
// playChannelOutAndBack(): that animates 0 -> target -> 0, whereas the
// squint channels rest at 1000 (fully open), so it would leave the eyes
// shut instead of returning them.
//
// Below 1000 narrows; ABOVE 1000 widens, which is how the surprised look
// gets its saucer eyes. Nothing clamps the squint channels at 1.0 -- they
// multiply straight into the eye height in renderFace(), so values over
// full simply make the eyes taller than the mood baseline.
void playEyeScale(int32_t toPermille, uint32_t outMs, uint32_t backMs, uint32_t holdMs);

// Small helper: one channel, out and back, with the return slower than the
// departure (things relax more slowly than they tense). Not mouth-specific
// -- the head turn uses it too.
void playChannelOutAndBack(lv_anim_exec_xcb_t cb, int32_t target, uint32_t outMs, uint32_t backMs,
                      uint32_t holdMs, lv_anim_path_cb_t path = lv_anim_path_ease_in_out) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, 0, target);
  lv_anim_set_time(&a, outMs);
  lv_anim_set_playback_time(&a, backMs);
  lv_anim_set_playback_delay(&a, holdMs);
  lv_anim_set_path_cb(&a, path);
  lv_anim_start(&a);
}

// A brief pout -- the original flourish, kept.
void playMouthPout() { playChannelOutAndBack(mouthExecCb, -700, 260, 420, 520); }

// A small private smile: widens and parts slightly, which reads warmer
// than widening alone.
void playMouthSmile() {
  playChannelOutAndBack(mouthExecCb, 650, 240, 480, 700);
  playChannelOutAndBack(mouthOpenExecCb, 250, 240, 480, 700);
}

// Smirk: one corner lifts and the mouth tightens a little. Held longer
// than the others because a smirk that snaps back reads as a twitch.
void playMouthSmirk() {
  int32_t dir = lv_rand(0, 1) == 0 ? 1 : -1;
  playChannelOutAndBack(mouthSkewExecCb, dir * 1000, 300, 560, 900);
  playChannelOutAndBack(mouthExecCb, -250, 300, 560, 900);
}

// Chatter: three quick open/close pulses, as if it muttered something.
// repeat_cnt only decrements at the end of a FORWARD leg (verified in
// lv_anim.c anim_ready_handler), so with playback enabled a count of 3 is
// three full out-and-back pulses, ending back at rest.
void playMouthChatter() {
  playChannelOutAndBack(mouthExecCb, -150, 140, 300, 620);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, mouthOpenExecCb);
  lv_anim_set_values(&a, 0, 500);
  lv_anim_set_time(&a, 130);
  lv_anim_set_playback_time(&a, 150);
  lv_anim_set_repeat_count(&a, 3);
  lv_anim_set_repeat_delay(&a, 40);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// Yawn: a round mouth that opens wide and closes again, with the eyes
// narrowing along with it. REVISED: this used to curl the mouth ARC into a
// near-closed ring, which at a 30px radius meant a 78px donut -- read as a
// big ring, not a yawn. It now swaps in the filled circle instead.
// Deliberately uses the squint channels rather than blinkLid: the blink
// scheduler owns blinkLid and fires independently, so borrowing it would
// fight a blink landing mid-yawn.
void yawnDoneCb(lv_anim_t *) {
  // Snoring holds the round mouth open on its own; don't snatch it back.
  if (!snoring && !whistling) setGapeMode(GapeMode::HIDDEN);
}

void playMouthYawn() {
  if (gapeMode == GapeMode::SHOCK) return;  // don't fight a shock reaction

  ch.gapeScale = 0.0f;
  setGapeMode(GapeMode::ROUND);

  lv_anim_t g;
  lv_anim_init(&g);
  lv_anim_set_var(&g, &ch);
  lv_anim_set_exec_cb(&g, gapeScaleExecCb);
  lv_anim_set_values(&g, 0, 1000);
  lv_anim_set_time(&g, 620);
  lv_anim_set_playback_time(&g, 780);
  lv_anim_set_playback_delay(&g, 620);
  lv_anim_set_path_cb(&g, lv_anim_path_ease_in_out);
  lv_anim_set_ready_cb(&g, yawnDoneCb);
  lv_anim_start(&g);

  playEyeScale(300, 620, 700, 620);
}

// Picks one of the mouth shapes. Weighted toward the quieter ones -- a
// yawn or a chatter every time would be exhausting to sit next to.
void playMouthFlourish() {
  int32_t roll = lv_rand(0, 99);
  if (roll < 30) {
    playMouthPout();
  } else if (roll < 55) {
    playMouthSmile();
  } else if (roll < 75) {
    playMouthSmirk();
  } else if (roll < 92) {
    playMouthChatter();
  } else {
    playMouthYawn();
  }
}

// A single small mouth move with no hold, for coupling onto a glance.
void playMouthTwitch() {
  int32_t target = lv_rand(0, 1) == 0 ? -300 : 260;
  playChannelOutAndBack(mouthExecCb, target, 180, 320, 140);
}

void playEyeScale(int32_t toPermille, uint32_t outMs, uint32_t backMs, uint32_t holdMs) {
  for (lv_anim_exec_xcb_t cb : {squintLExecCb, squintRExecCb}) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &ch);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, 1000, toPermille);
    lv_anim_set_time(&a, outMs);
    lv_anim_set_playback_time(&a, backMs);
    lv_anim_set_playback_delay(&a, holdMs);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  }
}

// -- Skeptical squint: one eye partially closes while the other stays
// open, standing in for a raised eyebrow. This now writes its own per-eye
// channel rather than the eye geometry, so a blink landing mid-squint
// composes with it (the lids multiply) instead of wiping it out.
void squintLExecCb(void *, int32_t permille) {
  ch.squintL = permille / 1000.0f;
  renderFace();
}

void squintRExecCb(void *, int32_t permille) {
  ch.squintR = permille / 1000.0f;
  renderFace();
}

void playSquint() {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, lv_rand(0, 1) == 0 ? squintLExecCb : squintRExecCb);
  lv_anim_set_values(&a, 1000, 420);
  lv_anim_set_time(&a, 170);
  lv_anim_set_playback_time(&a, 240);
  lv_anim_set_playback_delay(&a, 600);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_repeat_count(&a, 1);
  lv_anim_start(&a);
}

// -- Head tilt: leans in with a little overshoot, holds, then unwinds.
void tiltExecCb(void *, int32_t tenthsOfDegree) {
  ch.tiltTenths = static_cast<float>(tenthsOfDegree);
  renderFace();
}

void playTilt() {
  int32_t dir = lv_rand(0, 1) == 0 ? 1 : -1;
  int32_t targetTenths = dir * static_cast<int32_t>(lv_rand(40, 75));

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, tiltExecCb);
  lv_anim_set_values(&a, 0, targetTenths);
  lv_anim_set_time(&a, 320);
  lv_anim_set_playback_time(&a, 520);
  lv_anim_set_playback_delay(&a, 700);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_set_repeat_count(&a, 1);
  lv_anim_start(&a);
}

// -- Head turn: the same pseudo-3D transform the shock reaction uses,
// borrowed for idle. A flat translation was always the weakest thing the
// idle layer did; turning instead makes the face feel like an object with
// a back to it.
void headYawExecCb(void *, int32_t permille) {
  ch.headYaw = permille / 1000.0f;
  renderFace();
}

void headPitchExecCb(void *, int32_t permille) {
  ch.headPitch = permille / 1000.0f;
  renderFace();
}

void playHeadTurn() {
  int32_t dir = lv_rand(0, 1) == 0 ? 1 : -1;
  int32_t yaw = dir * static_cast<int32_t>(lv_rand(380, 720));
  // ease_out on the way there: heads accelerate off the mark and settle,
  // they don't glide symmetrically.
  playChannelOutAndBack(headYawExecCb, yaw, 430, 640, 900, lv_anim_path_ease_out);
  // Only sometimes a nod as well, so turns don't all feel identical.
  if (lv_rand(0, 99) < 55) {
    int32_t pitch = (lv_rand(0, 1) == 0 ? 1 : -1) * static_cast<int32_t>(lv_rand(200, 480));
    playChannelOutAndBack(headPitchExecCb, pitch, 470, 660, 860);
  }
  if (lv_rand(0, 99) < 45) triggerBlink();
}

// --- Schedulers ---
// Three independent clocks rather than one picker. Their periods don't
// divide into each other, so behaviours drift in and out of phase and
// occasionally coincide -- which is a large part of why this reads as
// alive rather than as a sequencer stepping through a list.
uint32_t nextBlinkDelay() {
  // ASSUMPTION: mostly 2.5-6.5s, but ~1 in 6 gaps is short, so blinks
  // sometimes cluster instead of arriving on a fixed beat.
  if (lv_rand(0, 99) < 16) return lv_rand(500, 1200);
  return lv_rand(2500, 6500);
}

void blinkSchedCb(lv_timer_t *t) {
  triggerBlink();  // no-ops itself at MINIMAL
  lv_timer_set_period(t, nextBlinkDelay());
}

void gazeSchedCb(lv_timer_t *t) {
  if (idleLevel == IdleLevel::FULL) {
    startGazeWander();
    // Couple a small mouth move onto some glances. Behaviours that
    // co-occur read as one intention; behaviours that only ever happen
    // alone read as a list being worked through.
    if (lv_rand(0, 99) < 22) playMouthTwitch();
  }
  lv_timer_set_period(t, lv_rand(3200, 9000));
}

// Flourish cadence is mood-driven: excited fidgets often, bored barely
// stirs.
uint32_t flourishMinMs = 7000;
uint32_t flourishMaxMs = 19000;

void flourishSchedCb(lv_timer_t *t) {
  if (idleLevel == IdleLevel::FULL) {
    // Weighted toward the mouth now that it has five shapes rather than
    // one; tilt and squint were carrying too much of the idle variety.
    // While whistling, the sway owns the tilt and the whistle owns the
    // mouth -- a one-shot tilt would replace the sway and never restore
    // it, and a mouth flourish (a yawn especially) would steal the gape.
    // Squints and head turns compose fine, so those are all that is left.
    int32_t roll = lv_rand(0, 99);
    if (whistling) {
      if (roll < 50) {
        playSquint();
      } else {
        playHeadTurn();
      }
    } else if (roll < 16) {
      playTilt();
    } else if (roll < 31) {
      playSquint();
    } else if (roll < 51) {
      playHeadTurn();
    } else if (roll < 63) {
      playWhistle();
    } else {
      playMouthFlourish();
    }
  }
  lv_timer_set_period(t, lv_rand(flourishMinMs, flourishMaxMs));
}

void startIdleSystems() {
  startBreathing(2200, 150);
  microTimer = lv_timer_create(microTickCb, 600, nullptr);
  blinkSchedTimer = lv_timer_create(blinkSchedCb, lv_rand(1200, 2500), nullptr);
  gazeSchedTimer = lv_timer_create(gazeSchedCb, lv_rand(2500, 5000), nullptr);
  flourishSchedTimer = lv_timer_create(flourishSchedCb, lv_rand(4000, 9000), nullptr);
}

// Mood states now dial idle behaviour *down* rather than switching it off
// outright: asleep still breathes (slower and deeper), and a held reminder
// pose still blinks. Killing all motion dead was part of what made state
// changes feel like the device had been unplugged.
void setIdleLevel(IdleLevel level) {
  if (level == idleLevel) return;
  idleLevel = level;

  if (level != IdleLevel::FULL) {
    // A held pose owns the face while it lasts: stop wandering and
    // flourishing, and unwind those channels back to rest.
    lv_anim_del(&ch, gazeExecCb);
    lv_anim_del(&ch, headYawExecCb);
    lv_anim_del(&ch, headPitchExecCb);
    lv_anim_del(&ch, tiltExecCb);
    lv_anim_del(&ch, mouthExecCb);
    lv_anim_del(&ch, mouthSkewExecCb);
    lv_anim_del(&ch, mouthOpenExecCb);
    lv_anim_del(&ch, gapeScaleExecCb);
    lv_anim_del(&ch, squintLExecCb);
    lv_anim_del(&ch, squintRExecCb);
    if (gazeHoldTimer != nullptr) {
      lv_timer_del(gazeHoldTimer);
      gazeHoldTimer = nullptr;
    }
    gazeChainLeft = 0;
    ch.gazeX = 0;
    ch.gazeY = 0;
    ch.headYaw = 0;
    ch.headPitch = 0;
    ch.tiltTenths = 0;
    ch.mouthBias = 0;
    ch.mouthSkew = 0;
    ch.mouthOpen = 0;
    ch.squintL = 1.0f;
    ch.squintR = 1.0f;
  }

  if (level == IdleLevel::MINIMAL) {
    lv_anim_del(&ch, blinkLidExecCb);
    pendingExtraBlinks = 0;
    ch.blinkLid = 1.0f;  // the sustained baseline already has the eyes nearly shut
  }
  // Breathing rate is NOT set here any more -- applyMoodState() owns it,
  // because it varies per mood and not just per idle level. Two callers
  // restarting the same infinite animation fought each other.
  renderFace();
}

// --- Baseline transition (mood-state-driven) ---
// One small anim eases eye height / mouth angles / eye drift from
// whatever they currently are to a new sustained target. Infrequent
// (mood states persist for a while) so the small chance of overlapping
// with an in-flight idle animation is an accepted, low-impact tradeoff.
struct BaselineTarget {
  lv_coord_t eyeHeight;
  uint16_t mouthStart;
  uint16_t mouthEnd;
  lv_coord_t eyeDriftX;
};

BaselineTarget baselineFrom, baselineTo;

void baselineExecCb(void *, int32_t progressPermille) {
  baseEyeHeight = baselineFrom.eyeHeight +
                  (baselineTo.eyeHeight - baselineFrom.eyeHeight) * progressPermille / 1000;
  baseMouthStart = static_cast<uint16_t>(
      baselineFrom.mouthStart + (static_cast<int32_t>(baselineTo.mouthStart) - baselineFrom.mouthStart) *
                                     progressPermille / 1000);
  baseMouthEnd = static_cast<uint16_t>(
      baselineFrom.mouthEnd +
      (static_cast<int32_t>(baselineTo.mouthEnd) - baselineFrom.mouthEnd) * progressPermille / 1000);
  baseEyeDriftX = baselineFrom.eyeDriftX +
                  (baselineTo.eyeDriftX - baselineFrom.eyeDriftX) * progressPermille / 1000;
  renderFace();
}

void animateBaselineTo(lv_coord_t eyeHeight, uint16_t mouthStart, uint16_t mouthEnd, lv_coord_t eyeDriftX) {
  baselineFrom = {baseEyeHeight, baseMouthStart, baseMouthEnd, baseEyeDriftX};
  baselineTo = {eyeHeight, mouthStart, mouthEnd, eyeDriftX};

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_exec_cb(&a, baselineExecCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 450);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

// Snappier variant for *entering* a manual expression: faster, with a
// little overshoot so it reads as a reaction/pop rather than a slow
// settle. Releasing back to neutral still goes through animateBaselineTo
// above -- quick reaction, gentle recovery.
void animateExpressionTo(lv_coord_t eyeHeight, uint16_t mouthStart, uint16_t mouthEnd, lv_coord_t eyeDriftX) {
  baselineFrom = {baseEyeHeight, baseMouthStart, baseMouthEnd, baseEyeDriftX};
  baselineTo = {eyeHeight, mouthStart, mouthEnd, eyeDriftX};

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_exec_cb(&a, baselineExecCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 180);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
  lv_anim_start(&a);
}

// --- Brief whole-face shake (rage) ---
// Writes the faceOff channel, so it composes with everything else and no
// longer needs to capture/restore object positions by hand (renderFace()
// owns those now).
lv_coord_t shakeAmplitude = 3;
lv_timer_t *shakeTickTimer = nullptr;
lv_timer_t *shakeEndTimer = nullptr;

void shakeTickCb(lv_timer_t *) {
  ch.faceOffX = static_cast<float>(lv_rand(0, 2 * shakeAmplitude)) - shakeAmplitude;
  ch.faceOffY = static_cast<float>(lv_rand(0, 2 * shakeAmplitude)) - shakeAmplitude;
  renderFace();
}

void stopShake(lv_timer_t *) {
  if (shakeTickTimer != nullptr) {
    lv_timer_del(shakeTickTimer);
    shakeTickTimer = nullptr;
  }
  ch.faceOffX = 0;
  ch.faceOffY = 0;
  renderFace();
  // repeat_count=1 auto-deletes this timer after return -- see hideGlitch().
  shakeEndTimer = nullptr;
}

void playShake(uint32_t durationMs, lv_coord_t amplitude) {
  shakeAmplitude = amplitude;

  if (shakeTickTimer != nullptr) lv_timer_del(shakeTickTimer);
  shakeTickTimer = lv_timer_create(shakeTickCb, 45, nullptr);

  if (shakeEndTimer != nullptr) lv_timer_del(shakeEndTimer);
  shakeEndTimer = lv_timer_create(stopShake, durationMs, nullptr);
  lv_timer_set_repeat_count(shakeEndTimer, 1);
}

// --- Shock: the head turns to look left and right, mouth swapped for a
// wide-open filled shape. REVISED: this used to drive gazeX, which slid
// the whole face sideways as one rigid unit and read as a flat image
// panning. It now drives headYaw, so the eyes foreshorten by different
// amounts and the mouth parallaxes past them -- the face turns instead of
// translating. Blinks and breathing keep running underneath either way.
// Deliberately reuses headYawExecCb rather than owning a second callback
// on the same channel: lv_anim_start() replaces an existing animation
// with the same var+exec_cb, so an idle head turn landing mid-shock
// supersedes it cleanly instead of the two writing headYaw in turn and
// leaving it stuck wherever the loser stopped.

// REVISED AGAIN: this used to be a single infinite animation swinging
// headYaw between -1.0 and +1.0 on ease_in_out, i.e. a full-amplitude
// metronome with no pause at either end. That is what read as violent
// shaking rather than looking around. Two changes fix it: the amplitude
// drops to a fraction of full yaw, and -- the important one -- each
// glance now HOLDS before the next, so it scans instead of oscillating.
// Same principle as the idle saccades: movement, then stillness.
lv_timer_t *shockGlanceTimer = nullptr;
int shockGlanceDir = 1;

void shockGlanceCb(lv_timer_t *t) {
  // Alternate sides, but vary how far each way, so it does not settle
  // into a rhythm between two fixed extremes.
  shockGlanceDir = -shockGlanceDir;
  int32_t target = shockGlanceDir * static_cast<int32_t>(lv_rand(300, 580));
  uint32_t moveMs = lv_rand(190, 280);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, headYawExecCb);
  lv_anim_set_values(&a, static_cast<int32_t>(ch.headYaw * 1000.0f), target);
  lv_anim_set_time(&a, moveMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // decelerate in, like a saccade
  lv_anim_start(&a);

  // Sometimes a little vertical with it, so the scan is not perfectly flat.
  if (lv_rand(0, 99) < 45) {
    int32_t pitch = (lv_rand(0, 1) == 0 ? 1 : -1) * static_cast<int32_t>(lv_rand(70, 180));
    lv_anim_t pa;
    lv_anim_init(&pa);
    lv_anim_set_var(&pa, &ch);
    lv_anim_set_exec_cb(&pa, headPitchExecCb);
    lv_anim_set_values(&pa, static_cast<int32_t>(ch.headPitch * 1000.0f), pitch);
    lv_anim_set_time(&pa, moveMs);
    lv_anim_set_path_cb(&pa, lv_anim_path_ease_out);
    lv_anim_start(&pa);
  }

  // Move, then sit still for a beat. The hold is the whole difference
  // between "looking around" and "shaking".
  lv_timer_set_period(t, moveMs + lv_rand(260, 520));
}

void startShockLook() {
  // Gaze also writes headYaw now (see gazeExecCb), so it has to be stood
  // down or a saccade mid-shock would yank the turn back.
  lv_anim_del(&ch, gazeExecCb);
  if (gazeHoldTimer != nullptr) {
    lv_timer_del(gazeHoldTimer);
    gazeHoldTimer = nullptr;
  }
  gazeChainLeft = 0;
  ch.gazeX = 0;
  ch.gazeY = 0;

  // Flinch first: the head snaps back, then a beat before it starts
  // looking around. Scheduling the first glance after this rather than
  // alongside it matters -- both drive headPitchExecCb, and
  // lv_anim_start() replaces an animation with the same var+exec_cb.
  playChannelOutAndBack(headPitchExecCb, -380, 140, 420, 120, lv_anim_path_ease_out);

  shockGlanceDir = (lv_rand(0, 1) == 0) ? 1 : -1;
  if (shockGlanceTimer != nullptr) lv_timer_del(shockGlanceTimer);
  shockGlanceTimer = lv_timer_create(shockGlanceCb, 320, nullptr);
}

void stopShockLook() {
  if (shockGlanceTimer != nullptr) {
    lv_timer_del(shockGlanceTimer);
    shockGlanceTimer = nullptr;
  }
  // Ease back to centre rather than snapping: after a 2.5s reaction a
  // hard reset reads as a cut.
  auto easeHome = [](lv_anim_exec_xcb_t cb, float from) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &ch);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, static_cast<int32_t>(from * 1000.0f), 0);
    lv_anim_set_time(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
  };
  easeHome(headYawExecCb, ch.headYaw);
  easeHome(headPitchExecCb, ch.headPitch);
}

void setShockMouthVisible(bool visible) {
  setGapeMode(visible ? GapeMode::SHOCK : GapeMode::HIDDEN);
}

// --- Hydration reminder: a blue bottle that actually empties ---
// Three plain objects rather than one: a white-outlined body, a blue fill
// whose height is the remaining water, and a small white cap. The fill is
// bottom-anchored inside the body and only ever shrinks, so it needs no
// clipping.
constexpr lv_coord_t kBottleW = 22;
constexpr lv_coord_t kBottleH = 58;
constexpr lv_coord_t kBottleInset = 3;  // glass thickness the water sits inside
constexpr lv_coord_t kBottleCapW = 10;
constexpr lv_coord_t kBottleCapH = 8;
constexpr lv_coord_t kBottleX = kEyeRightX + kEyeWidth + 16;
// Sits on the mouth LINE (the bottom of the mouth arc's circle), not the
// arc's centre -- it is being drunk from, so it belongs level with the
// mouth.
constexpr lv_coord_t kBottleY = kMouthCenterY + kMouthRadius - kBottleH / 2;
constexpr lv_coord_t kBottleFillMax = kBottleH - 2 * kBottleInset;
constexpr lv_coord_t kHydrationDriftX = 14;  // toward the bottle, on the right

lv_obj_t *bottleBody = nullptr;
lv_obj_t *bottleFill = nullptr;
lv_obj_t *bottleCap = nullptr;
// 1.0 = full, 0.0 = drunk empty. Not an animation channel: the bottle is
// an object in the scene, not part of the face, so it deliberately does
// NOT ride renderFace()'s compositing: it is a prop in the scene, not
// part of the face.
// It must not breathe or lean with the face -- the face leans toward IT.
float bottleLevel = 1.0f;

void updateBottleFill() {
  lv_coord_t h = static_cast<lv_coord_t>(kBottleFillMax * bottleLevel);
  if (h < 0) h = 0;
  lv_obj_set_size(bottleFill, kBottleW - 2 * kBottleInset, h);
  // Bottom-anchored, so the surface falls as it empties.
  lv_obj_set_pos(bottleFill, kBottleX + kBottleInset,
                 kBottleY + kBottleH - kBottleInset - h);
}

void bottleLevelExecCb(void *, int32_t permille) {
  bottleLevel = permille / 1000.0f;
  updateBottleFill();
}

void setBottleVisible(bool visible) {
  lv_obj_t *parts[] = {bottleBody, bottleFill, bottleCap};
  for (lv_obj_t *o : parts) {
    if (visible) {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

// --- Rain-forecast cameo: a cloud drifts in and rains a little ---
// Scenery, not face: like the bottle, these are positioned
// directly rather than composited through renderFace(), because they are
// objects in the world that the face looks AT. They sit in the empty strip
// to the left of the face (the face spans x 78..242), where the bottle on
// the right has its counterpart.
constexpr lv_coord_t kCloudW = 56;
constexpr lv_coord_t kCloudH = 28;
constexpr lv_coord_t kCloudCx = 46;
constexpr lv_coord_t kCloudCy = 30;
constexpr int kRainDropCount = 3;
constexpr lv_coord_t kDropW = 4;
constexpr lv_coord_t kDropH = 9;
constexpr lv_coord_t kDropTop = kCloudCy + kCloudH / 2 - 2;  // just under the cloud
constexpr lv_coord_t kDropFall = 40;                          // how far they fall

lv_obj_t *cloudCanvas = nullptr;
lv_obj_t *rainDrops[kRainDropCount] = {nullptr, nullptr, nullptr};
// 0 = parked off the top of the screen, 1 = fully drifted in.
float cloudIn = 0.0f;
bool rainCameoActive = false;


// --- Express-only decorations ---

// "heart" -- replaces the two round eyes with heart shapes. Earlier
// approaches didn't hold up: a rotated-square point (transform_angle
// didn't reliably render once hidden-then-shown on real hardware, despite
// checking out from LVGL's source) and three plain overlapping circles
// (read as "three balls", not a heart). This draws the actual heart as
// pixels on a small lv_canvas instead: two filled circles for the lobes
// plus a filled triangle for the point, via LVGL's own polygon/rect fill
// routines rather than composing separately-transformed objects.
//
// Color: lv_palette_main(LV_PALETTE_RED) rendered green on the real
// panel -- unclear exactly why (a canvas/TRUE_COLOR_ALPHA-specific
// interpretation issue is the leading guess, not confirmed), but
// lv_color_white()/lv_color_black() elsewhere on this face render
// correctly and both resolve through the same lv_color_make() primitive,
// so this bypasses the palette lookup and uses lv_color_make() directly
// to sidestep whatever the palette-specific issue was.
//
// Sized larger than the eye it replaces (was smaller before, per
// feedback) and pulses via lv_img_set_zoom() -- lv_canvas is built on
// lv_img (confirmed: lv_canvas_class.base_class = &lv_img_class), and
// image zoom is a far more heavily-used/battle-tested LVGL feature than
// the transform_angle rotation that failed earlier, so it's a lower-risk
// way to bring animation back.
uint8_t heartCanvasBuf[2][LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kHeartCanvasW, kHeartCanvasH)];

void drawHeartShape(lv_obj_t *canvas) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t rectDsc;
  lv_draw_rect_dsc_init(&rectDsc);
  rectDsc.bg_color = lv_color_make(224, 24, 60);  // crimson
  rectDsc.bg_opa = LV_OPA_COVER;
  rectDsc.radius = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(canvas, 2, 2, 30, 30, &rectDsc);
  lv_canvas_draw_rect(canvas, 28, 2, 30, 30, &rectDsc);

  rectDsc.radius = 0;
  static const lv_point_t trianglePts[3] = {{2, 19}, {58, 19}, {30, 55}};
  lv_canvas_draw_polygon(canvas, trianglePts, 3, &rectDsc);
}

uint8_t noteCanvasBuf[2][LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kNoteW, kNoteH)];

// A quaver: oval head, stem, flag.
void drawNoteShape(lv_obj_t *canvas) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_white();
  d.bg_opa = LV_OPA_COVER;

  d.radius = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(canvas, 0, 15, 11, 8, &d);  // head
  d.radius = 0;
  lv_canvas_draw_rect(canvas, 9, 2, 2, 15, &d);   // stem
  static const lv_point_t kFlag[4] = {{11, 2}, {15, 6}, {15, 11}, {11, 7}};
  lv_canvas_draw_polygon(canvas, kFlag, 4, &d);
}

// Positioned directly rather than composited: notes leave the face rather
// than riding it, same as the cloud and the bottle. The second slot is
// nudged sideways so overlapping notes do not trace the same line.
void updateNotePos(int i) {
  float t = noteProgress[i];
  lv_coord_t lane = (i == 0) ? 0 : 9;
  lv_obj_set_pos(noteCanvas[i],
                 kNoteBaseX + lane + static_cast<lv_coord_t>(t * kNoteDriftX) - kNoteW / 2,
                 kNoteBaseY - static_cast<lv_coord_t>(t * kNoteDriftY) - kNoteH / 2);
  // In fast, hold, then out. lv_img draws with img_opa (NOT the generic
  // opa style), so that is the one to set on a canvas.
  float a = (t < 0.15f) ? (t / 0.15f) : ((t > 0.60f) ? (1.0f - t) / 0.40f : 1.0f);
  if (a < 0.0f) a = 0.0f;
  lv_obj_set_style_img_opa(noteCanvas[i], static_cast<lv_opa_t>(a * 255.0f), LV_PART_MAIN);
}

// One callback per slot: each animation needs a distinct var+exec_cb pair
// or starting the second would delete the first.
void noteAExecCb(void *, int32_t permille) {
  noteProgress[0] = permille / 1000.0f;
  updateNotePos(0);
}
void noteBExecCb(void *, int32_t permille) {
  noteProgress[1] = permille / 1000.0f;
  updateNotePos(1);
}
void noteADoneCb(lv_anim_t *) {
  noteVisible[0] = false;
  lv_obj_add_flag(noteCanvas[0], LV_OBJ_FLAG_HIDDEN);
}
void noteBDoneCb(lv_anim_t *) {
  noteVisible[1] = false;
  lv_obj_add_flag(noteCanvas[1], LV_OBJ_FLAG_HIDDEN);
}

void spawnNote() {
  int i = nextNoteSlot;
  nextNoteSlot ^= 1;

  noteVisible[i] = true;
  noteProgress[i] = 0.0f;
  updateNotePos(i);
  lv_obj_clear_flag(noteCanvas[i], LV_OBJ_FLAG_HIDDEN);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, noteCanvas[i]);
  lv_anim_set_exec_cb(&a, (i == 0) ? noteAExecCb : noteBExecCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, 1900);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // drifts off, slowing
  lv_anim_set_ready_cb(&a, (i == 0) ? noteADoneCb : noteBDoneCb);
  lv_anim_start(&a);
}

void hideNotes() {
  lv_anim_del(noteCanvas[0], noteAExecCb);
  lv_anim_del(noteCanvas[1], noteBExecCb);
  for (int i = 0; i < 2; i++) {
    noteVisible[i] = false;
    noteProgress[i] = 0.0f;
    lv_obj_add_flag(noteCanvas[i], LV_OBJ_FLAG_HIDDEN);
  }
}

uint8_t handCanvasBuf[LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kHandW, kHandH)];

// Drawn on a canvas like the heart and the cloud, because loose
// overlapping objects read as loose overlapping objects.
//
// Three things do the work of making it look like a hand rather than a
// mitten: the fingers are DIFFERENT LENGTHS (middle longest, then ring,
// index, pinky) instead of a uniform comb; they use LV_RADIUS_CIRCLE so
// each is a capsule with a properly domed tip; and the thumb is a slanted
// polygon with a round cap rather than another axis-aligned box. All four
// fingers end at the same y, tucked under the palm, so the joins vanish.
void drawHandShape(lv_obj_t *canvas) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = lv_color_white();
  d.bg_opa = LV_OPA_COVER;

  // Fingers: {x, y, height}. Width 5, all ending at y=33 under the palm.
  static const lv_coord_t kFingers[4][3] = {
      {9, 11, 22},   // index
      {15, 7, 26},   // middle -- longest
      {21, 10, 23},  // ring
      {27, 16, 17},  // pinky -- shortest
  };
  d.radius = LV_RADIUS_CIRCLE;
  for (const auto &f : kFingers) {
    lv_canvas_draw_rect(canvas, f[0], f[1], 5, f[2], &d);
  }

  // Thumb tip, then the slanted shaft joining it to the palm.
  lv_canvas_draw_rect(canvas, 1, 22, 8, 8, &d);
  d.radius = 0;
  static const lv_point_t kThumb[4] = {{11, 38}, {2, 29}, {6, 24}, {15, 33}};
  lv_canvas_draw_polygon(canvas, kThumb, 4, &d);

  d.radius = 9;
  lv_canvas_draw_rect(canvas, 8, 27, 25, 18, &d);  // palm
  d.radius = 4;
  lv_canvas_draw_rect(canvas, 13, 41, 15, 8, &d);  // wrist
}

// LVGL's angle space is 0..3600 tenths of a degree, so animating straight
// from 3400 to 200 would sweep 340 degrees the long way round. The
// animation therefore runs over a SIGNED range and normalises here, at the
// one point where it matters.
void waveExecCb(void *, int32_t tenths) {
  int32_t a = tenths % 3600;
  if (a < 0) a += 3600;
  lv_img_set_angle(handCanvas, static_cast<int16_t>(a));
}

void setHandVisible(bool visible) {
  if (visible) {
    lv_obj_clear_flag(handCanvas, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_anim_del(handCanvas, waveExecCb);
    lv_img_set_angle(handCanvas, 0);
    lv_obj_add_flag(handCanvas, LV_OBJ_FLAG_HIDDEN);
  }
}

uint8_t cloudCanvasBuf[LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kCloudW, kCloudH)];

// A flat-bottomed, lumpy-topped cloud: one rounded base plus three
// circles. Drawn on a canvas rather than assembled from overlapping
// lv_objs for the same reason the heart is -- three loose circles read as
// three circles (the user said exactly that of the first heart), whereas
// one rasterised silhouette reads as the thing it is.
void drawCloudShape(lv_obj_t *canvas) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = lv_color_white();
  dsc.bg_opa = LV_OPA_COVER;

  dsc.radius = 6;
  lv_canvas_draw_rect(canvas, 4, 14, 48, 13, &dsc);
  dsc.radius = LV_RADIUS_CIRCLE;
  lv_canvas_draw_rect(canvas, 8, 7, 20, 20, &dsc);
  lv_canvas_draw_rect(canvas, 20, 2, 24, 24, &dsc);
  lv_canvas_draw_rect(canvas, 36, 9, 17, 17, &dsc);
}

// Cloud and drops share one slide-in offset. Drops carry their own fall
// progress on top of it.
float dropFall[kRainDropCount] = {0.0f, 0.0f, 0.0f};

void updateCloudPos() {
  lv_coord_t slide = static_cast<lv_coord_t>((1.0f - cloudIn) * -(kCloudCy + kCloudH));
  lv_obj_set_pos(cloudCanvas, kCloudCx - kCloudW / 2, kCloudCy - kCloudH / 2 + slide);
  for (int i = 0; i < kRainDropCount; i++) {
    lv_coord_t x = kCloudCx - 16 + i * 15;
    lv_obj_set_pos(rainDrops[i], x,
                   kDropTop + slide + static_cast<lv_coord_t>(dropFall[i] * kDropFall));
  }
}

void cloudInExecCb(void *, int32_t permille) {
  cloudIn = permille / 1000.0f;
  updateCloudPos();
}

uint8_t browCanvasBuf[2][LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kBrowW, kBrowH)];

// A black quad filling the canvas from its top down to a slanted edge.
// "Inner" is the nose side, which is the right edge of the left eye's
// canvas and the left edge of the right eye's -- so the two are mirrored
// and a single (inner, outer) pair describes a symmetric pair of brows.
void drawBrowShape(lv_obj_t *canvas, lv_coord_t innerDepth, lv_coord_t outerDepth,
                   bool isLeftEye) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = lv_color_black();
  dsc.bg_opa = LV_OPA_COVER;
  dsc.radius = 0;

  lv_coord_t leftEdge = isLeftEye ? outerDepth : innerDepth;
  lv_coord_t rightEdge = isLeftEye ? innerDepth : outerDepth;
  const lv_point_t pts[4] = {{0, 0}, {kBrowW, 0}, {kBrowW, rightEdge}, {0, leftEdge}};
  lv_canvas_draw_polygon(canvas, pts, 4, &dsc);
}

// Redrawn only on a mood change (a few times an hour), so the cost of
// re-rasterising both canvases doesn't matter.
void setBrows(lv_coord_t innerDepth, lv_coord_t outerDepth) {
  if (innerDepth <= 0 && outerDepth <= 0) {
    browsVisible = false;
    lv_obj_add_flag(browCanvas[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(browCanvas[1], LV_OBJ_FLAG_HIDDEN);
    return;
  }
  drawBrowShape(browCanvas[0], innerDepth, outerDepth, true);
  drawBrowShape(browCanvas[1], innerDepth, outerDepth, false);
  browsVisible = true;
  lv_obj_clear_flag(browCanvas[0], LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(browCanvas[1], LV_OBJ_FLAG_HIDDEN);
}

// Sustained mood droop. One-way (no playback) -- it holds until the next
// mood replaces it.
void basePitchExecCb(void *, int32_t permille) {
  basePitch = permille / 1000.0f;
  renderFace();
}

void animateBasePitchTo(float target, uint32_t ms) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &basePitch);
  lv_anim_set_exec_cb(&a, basePitchExecCb);
  lv_anim_set_values(&a, static_cast<int32_t>(basePitch * 1000.0f),
                     static_cast<int32_t>(target * 1000.0f));
  lv_anim_set_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

uint8_t sunglassGlareBuf[2][LV_IMG_BUF_SIZE_TRUE_COLOR_ALPHA(kGlareW, kGlareH)];

// Two parallel streaks leaning right as they rise -- the classic glass
// highlight. Canvas-local coordinates in a kGlareW x kGlareH box; a canvas
// polygon is the only reliable way to get a diagonal here (see
// drawHeartShape()'s note on transform_angle).
void drawGlareShape(lv_obj_t *canvas) {
  lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);

  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = lv_color_black();
  dsc.bg_opa = LV_OPA_COVER;
  dsc.radius = 0;

  static const lv_point_t wideStreak[4] = {{2, 26}, {10, 26}, {17, 3}, {9, 3}};
  static const lv_point_t narrowStreak[4] = {{13, 26}, {17, 26}, {24, 3}, {20, 3}};
  lv_canvas_draw_polygon(canvas, wideStreak, 4, &dsc);
  lv_canvas_draw_polygon(canvas, narrowStreak, 4, &dsc);
}

void heartPulseExecCb(void *, int32_t zoom) {
  lv_img_set_zoom(heartCanvas[0], static_cast<uint16_t>(zoom));
  lv_img_set_zoom(heartCanvas[1], static_cast<uint16_t>(zoom));
}

void setHeartVisible(bool visible) {
  if (visible) {
    lv_obj_add_flag(eyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
    // renderFace() places these (and keeps them breathing/gazing with the
    // rest of the face) once the flag is set.
    heartEyesVisible = true;
    lv_obj_clear_flag(heartCanvas[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(heartCanvas[1], LV_OBJ_FLAG_HIDDEN);
    renderFace();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, heartCanvas[0]);  // identity token for later lv_anim_del()
    lv_anim_set_exec_cb(&a, heartPulseExecCb);
    lv_anim_set_values(&a, LV_IMG_ZOOM_NONE, LV_IMG_ZOOM_NONE + LV_IMG_ZOOM_NONE / 6);  // ~117% at peak
    lv_anim_set_time(&a, 280);
    lv_anim_set_playback_time(&a, 280);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
  } else {
    lv_anim_del(heartCanvas[0], heartPulseExecCb);
    heartEyesVisible = false;
    lv_obj_add_flag(heartCanvas[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(heartCanvas[1], LV_OBJ_FLAG_HIDDEN);
    lv_img_set_zoom(heartCanvas[0], LV_IMG_ZOOM_NONE);
    lv_img_set_zoom(heartCanvas[1], LV_IMG_ZOOM_NONE);
    lv_obj_clear_flag(eyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
    renderFace();
  }
}

lv_timer_t *expressionTimer = nullptr;  // clears one-shot express decorations after a delay

void clearExpressionDecorations(lv_timer_t *) {
  // Back to the MOOD's pose, not to neutral. Restoring hardcoded defaults
  // here is what left a bored or focused face sitting at neutral eye
  // height and mouth after every expression.
  animateBaselineTo(currentPose.eyeHeight, currentPose.mouthStart, currentPose.mouthEnd,
                    currentPose.eyeDriftX);
  animateBasePitchTo(currentPose.basePitch, 320);
  setBrows(currentPose.browInner, currentPose.browOuter);
  // An expression can land mid-yawn or mid-grin; drop those channels so
  // the mouth can't be left stuck open or half-gaped.
  lv_anim_del(&ch, mouthOpenExecCb);
  lv_anim_del(&ch, gapeScaleExecCb);
  ch.mouthOpen = 0;
  setHeartVisible(false);
  stopShockLook();
  setShockMouthVisible(false);
  // The gape teardown above (and setShockMouthVisible's HIDDEN) would
  // otherwise leave a whistling mood with no mouth and a frozen wobble;
  // the `wave` expression also drives tiltExecCb, which would have
  // replaced the sway and left the head still.
  restartWhistleMotion();
  setHandVisible(false);
  hideSnoreBubble();
  setBottleVisible(false);
  // Same reasoning as hideGlitch(): repeat_count=1 means LVGL deletes this
  // timer right after this callback returns -- must not delete it again
  // here, just drop the reference.
  expressionTimer = nullptr;
}

// --- Glitch: a stuttering channel-split, not a static overlay ---
// REVISED: the first version placed its bands and ghosts once and held
// that single frame for the whole 200-800ms, which read as a coloured
// sticker laid over the face rather than a fault. It now re-randomises on
// a fast stutter timer, so the face tears, the split breathes, the bands
// jump around, and the real eyes occasionally drop out entirely.
constexpr int kGlitchBandCount = 6;
lv_obj_t *glitchBands[kGlitchBandCount];
lv_obj_t *glitchFlash = nullptr;
lv_timer_t *glitchEndTimer = nullptr;
lv_timer_t *glitchStutterTimer = nullptr;
bool glitchEyesDropped = false;

// 55ms against LVGL's 30ms refresh period: fast enough to read as a
// stutter, slow enough that every frame actually gets drawn.
constexpr uint32_t kGlitchStutterMs = 55;

void setGlitchEyesDropped(bool drop) {
  if (drop == glitchEyesDropped) return;
  glitchEyesDropped = drop;
  if (drop) {
    lv_obj_add_flag(eyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
  } else if (!heartEyesVisible) {
    // Only give the eyes back if the hearts have not taken them over in
    // the meantime -- setHeartVisible() owns the same hidden flag, and
    // clearing it blindly would show eyes through the hearts.
    lv_obj_clear_flag(eyeLeft, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(eyeRight, LV_OBJ_FLAG_HIDDEN);
  }
}

void glitchStutterCb(lv_timer_t *) {
  // lv_color_make rather than lv_palette_main: the palette path is the one
  // that rendered the hearts green, and these are the only other coloured
  // pixels on the panel.
  static const lv_color_t kBandColours[] = {
      lv_color_make(255, 60, 60),
      lv_color_make(60, 230, 255),
      lv_color_make(150, 255, 80),
      lv_color_make(255, 255, 255),
  };
  constexpr uint32_t kBandColourCount = sizeof(kBandColours) / sizeof(kBandColours[0]);

  // The split amount breathes instead of sitting at a fixed 5px.
  glitchSplitX = static_cast<float>(lv_rand(3, 9));
  glitchSplitY = (lv_rand(0, 99) < 40) ? static_cast<float>(lv_rand(0, 4)) - 2.0f : 0.0f;

  // Horizontal tearing of the whole face -- the thing that most reads as a
  // broken signal rather than a decorated face.
  ch.faceOffX = (lv_rand(0, 99) < 70) ? static_cast<float>(lv_rand(0, 12)) - 6.0f : 0.0f;
  ch.faceOffY = (lv_rand(0, 99) < 25) ? static_cast<float>(lv_rand(0, 6)) - 3.0f : 0.0f;

  // Dropout: the real white eyes vanish for a tick, leaving only the
  // colour ghosts. Skipped while the hearts are up -- those already own
  // the eyes' hidden flag, and fighting over it would leave eyes showing
  // through the hearts.
  setGlitchEyesDropped(!heartEyesVisible && lv_rand(0, 99) < 18);

  for (auto *band : glitchBands) {
    if (lv_rand(0, 99) < 25) {
      lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(band, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(band, LCD_WIDTH, static_cast<lv_coord_t>(lv_rand(2, 7)));
    // lv_rand takes uint32_t, so a negative minimum has to be built by
    // subtracting rather than passed in directly.
    lv_obj_set_pos(band, static_cast<lv_coord_t>(lv_rand(0, 20)) - 10,
                   static_cast<lv_coord_t>(lv_rand(0, LCD_HEIGHT - 8)));
    lv_obj_set_style_bg_color(band, kBandColours[lv_rand(0, kBandColourCount - 1)], LV_PART_MAIN);
  }

  // Rare whole-screen wash, like a frame of lost sync.
  if (lv_rand(0, 99) < 8) {
    lv_obj_clear_flag(glitchFlash, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(glitchFlash, LV_OBJ_FLAG_HIDDEN);
  }

  renderFace();
}

void hideGlitch(lv_timer_t *) {
  if (glitchStutterTimer != nullptr) {
    lv_timer_del(glitchStutterTimer);
    glitchStutterTimer = nullptr;
  }
  glitchActive = false;
  setGlitchEyesDropped(false);
  for (auto *band : glitchBands) lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(glitchFlash, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(glitchGhostRedL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(glitchGhostRedR, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(glitchGhostCyanL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(glitchGhostCyanR, LV_OBJ_FLAG_HIDDEN);
  ch.faceOffX = 0;
  ch.faceOffY = 0;
  renderFace();
  // Do NOT call lv_timer_del() on ourselves here: this callback runs
  // because repeat_count hit 0, and LVGL's own timer handler deletes the
  // timer right after this returns (confirmed in lv_timer.c). Deleting it
  // again would be a double-free -- just drop our reference.
  glitchEndTimer = nullptr;
}

void playGlitchEffect() {
  glitchActive = true;
  lv_obj_clear_flag(glitchGhostRedL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(glitchGhostRedR, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(glitchGhostCyanL, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(glitchGhostCyanR, LV_OBJ_FLAG_HIDDEN);

  if (glitchStutterTimer != nullptr) lv_timer_del(glitchStutterTimer);
  glitchStutterTimer = lv_timer_create(glitchStutterCb, kGlitchStutterMs, nullptr);
  glitchStutterCb(nullptr);  // first frame now, not 55ms from now

  uint32_t durationMs = lv_rand(200, 800);  // brief section 5: 200-800ms
  if (glitchEndTimer != nullptr) lv_timer_del(glitchEndTimer);
  glitchEndTimer = lv_timer_create(hideGlitch, durationMs, nullptr);
  lv_timer_set_repeat_count(glitchEndTimer, 1);
}

// --- Posture reminder: whole face stretches upward, then settles ---
void postureExecCb(void *, int32_t offsetY) {
  ch.faceOffY = static_cast<float>(offsetY);
  renderFace();
}

// Horizontal counterpart. Takes raw pixels, not permille -- faceOff* is a
// pixel channel, unlike the 0..1 normalised ones.
void faceOffXExecCb(void *, int32_t offsetX) {
  ch.faceOffX = static_cast<float>(offsetX);
  renderFace();
}

// --- Hydration: lean over to the bottle and drink it empty ---
// The sustained pose already drifts the eyes toward the bottle; this is
// the part that actually reads as drinking. It repeats for as long as the
// reminder is up rather than firing once on entry -- the reminder lasts
// 20s, and a single motion at the start left 19 of them looking inert.
constexpr lv_coord_t kHydrationLeanPx = 9;
// Four mouthfuls empties it, which at a 4.6s cycle lands just inside the
// 20s reminder.
constexpr float kBottleSipAmount = 0.25f;
lv_timer_t *hydrationTimer = nullptr;

void hydrationSipCb(lv_timer_t *t) {
  if (bottleLevel <= 0.005f) {
    // Drunk empty: one satisfied smile, then stop sipping at thin air for
    // the rest of the reminder. Paused rather than deleted -- deleting a
    // recurring timer from inside its own callback is the pattern that
    // caused the double-free crash once already; stopHydrationRoutine()
    // still owns it and frees it properly.
    playChannelOutAndBack(mouthExecCb, 560, 320, 640, 760);
    if (t != nullptr) lv_timer_pause(t);
    return;
  }

  // Re-assert visibility every cycle: a one-shot `hydrate` express landing
  // during a reminder clears its own decorations after 2.5s, which would
  // otherwise leave the reminder drinking from an invisible bottle.
  setBottleVisible(true);

  // Lean in toward the bottle, turning the head with it so the lean has
  // some depth instead of being a flat slide.
  playChannelOutAndBack(faceOffXExecCb, kHydrationLeanPx, 420, 520, 820, lv_anim_path_ease_out);
  playChannelOutAndBack(headYawExecCb, 300, 420, 560, 820, lv_anim_path_ease_out);

  // Three quick sips on the round mouth, starting once the lean lands.
  ch.gapeScale = 0.0f;
  setGapeMode(GapeMode::ROUND);
  lv_anim_t sip;
  lv_anim_init(&sip);
  lv_anim_set_var(&sip, &ch);
  lv_anim_set_exec_cb(&sip, gapeScaleExecCb);
  lv_anim_set_values(&sip, 0, 400);
  lv_anim_set_time(&sip, 190);
  lv_anim_set_playback_time(&sip, 210);
  lv_anim_set_repeat_count(&sip, 3);
  lv_anim_set_repeat_delay(&sip, 70);
  lv_anim_set_delay(&sip, 380);
  lv_anim_set_path_cb(&sip, lv_anim_path_ease_in_out);
  // Same completion handling as the yawn: put the arc mouth back, unless
  // sleep is holding the round one open.
  lv_anim_set_ready_cb(&sip, yawnDoneCb);
  lv_anim_start(&sip);

  // The water goes down over the same window the sips happen in.
  float target = bottleLevel - kBottleSipAmount;
  if (target < 0.0f) target = 0.0f;
  lv_anim_t drain;
  lv_anim_init(&drain);
  lv_anim_set_var(&drain, &bottleLevel);
  lv_anim_set_exec_cb(&drain, bottleLevelExecCb);
  lv_anim_set_values(&drain, static_cast<int32_t>(bottleLevel * 1000.0f),
                     static_cast<int32_t>(target * 1000.0f));
  lv_anim_set_time(&drain, 1150);
  lv_anim_set_delay(&drain, 380);
  lv_anim_set_path_cb(&drain, lv_anim_path_ease_in_out);
  lv_anim_start(&drain);
}

void startHydrationRoutine() {
  if (hydrationTimer != nullptr) return;
  bottleLevel = 1.0f;  // a fresh full bottle each reminder
  updateBottleFill();
  setBottleVisible(true);
  hydrationTimer = lv_timer_create(hydrationSipCb, 4600, nullptr);
  hydrationSipCb(hydrationTimer);  // first sip immediately, not 4.6s in
}

void stopHydrationRoutine() {
  if (hydrationTimer != nullptr) {
    lv_timer_del(hydrationTimer);
    hydrationTimer = nullptr;
  }
  lv_anim_del(&ch, faceOffXExecCb);
  lv_anim_del(&ch, headYawExecCb);
  lv_anim_del(&ch, gapeScaleExecCb);
  lv_anim_del(&bottleLevel, bottleLevelExecCb);
  ch.faceOffX = 0;
  ch.headYaw = 0;
  ch.gapeScale = 0;
  setBottleVisible(false);
  if (gapeMode != GapeMode::SHOCK && !whistling) setGapeMode(GapeMode::HIDDEN);
  renderFace();
}

// --- Posture: stretch, then a few steps on the spot ---
// Two halves, because the reminder is really saying two things: sit up,
// and get up. The stretch reaches, holds and settles; then the face bobs
// vertically a few times, which reads as footsteps.
lv_timer_t *postureTimer = nullptr;
lv_timer_t *postureWalkTimer = nullptr;

void postureWalkCb(lv_timer_t *) {
  // repeat_count=1 auto-frees this timer on return -- only drop the
  // reference, never delete it from in here.
  postureWalkTimer = nullptr;

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, postureExecCb);
  lv_anim_set_values(&a, 0, -5);
  lv_anim_set_time(&a, 190);
  lv_anim_set_playback_time(&a, 190);
  lv_anim_set_repeat_count(&a, 4);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

void postureRoutineCb(lv_timer_t *) {
  // Reach up and hold. The eyes squeeze and the mouth opens with the effort.
  playChannelOutAndBack(postureExecCb, -16, 500, 620, 700, lv_anim_path_ease_out);
  playEyeScale(260, 500, 620, 700);
  playChannelOutAndBack(mouthOpenExecCb, 520, 500, 620, 700);

  // The steps need their own timer rather than a delayed animation:
  // lv_anim_start() deletes any existing animation with the same
  // var+exec_cb, so starting the walk now would cancel the very stretch
  // it is meant to follow (both drive postureExecCb).
  if (postureWalkTimer != nullptr) lv_timer_del(postureWalkTimer);
  postureWalkTimer = lv_timer_create(postureWalkCb, 2000, nullptr);
  lv_timer_set_repeat_count(postureWalkTimer, 1);
}

void startPostureRoutine() {
  if (postureTimer != nullptr) return;
  postureTimer = lv_timer_create(postureRoutineCb, 7000, nullptr);
  postureRoutineCb(nullptr);
}

void stopPostureRoutine() {
  if (postureTimer != nullptr) {
    lv_timer_del(postureTimer);
    postureTimer = nullptr;
  }
  if (postureWalkTimer != nullptr) {
    // Still pending, so we own it (unlike from inside its own callback).
    lv_timer_del(postureWalkTimer);
    postureWalkTimer = nullptr;
  }
  lv_anim_del(&ch, postureExecCb);
  lv_anim_del(&ch, squintLExecCb);
  lv_anim_del(&ch, squintRExecCb);
  lv_anim_del(&ch, mouthOpenExecCb);
  ch.faceOffY = 0;
  ch.squintL = 1.0f;
  ch.squintR = 1.0f;
  ch.mouthOpen = 0;
  renderFace();
}

// --- Weather ambient overlays (rain / sunglasses / shiver) ---
// Lower priority than mood overlays: suppressed while a transient one
// (glitch/hydration/posture) is showing, resumes once it clears.
constexpr int kRainStreakCount = 5;
lv_obj_t *rainStreaks[kRainStreakCount];
lv_timer_t *rainTimer = nullptr;

void rainTickCb(lv_timer_t *) {
  for (auto *streak : rainStreaks) {
    lv_coord_t y = lv_obj_get_y(streak) + 6;
    if (y > LCD_HEIGHT) y = -20;
    lv_obj_set_y(streak, y);
  }
}

// Writes the faceOff channel like playShake() does, so it composes with
// blinking/breathing and needs no manual capture/restore.
lv_timer_t *shiverTimer = nullptr;

void shiverTickCb(lv_timer_t *) {
  ch.faceOffX = static_cast<float>(lv_rand(0, 4)) - 2.0f;
  ch.faceOffY = static_cast<float>(lv_rand(0, 4)) - 2.0f;
  renderFace();
}

// Sunglasses are a brief cameo, not a costume. While the weather is
// clear they drop onto the face every minute or two, sit for a couple of
// seconds, then lift away again -- worn permanently they just became the
// face, and the animation is the joke.
constexpr int32_t kShadesDropFrom = -96;  // starts above the visor
constexpr uint32_t kShadesInMs = 380;
// Held much longer than the first attempt's 2.6s, which was over almost
// before it registered.
constexpr uint32_t kShadesHoldMs = 7000;
constexpr uint32_t kShadesOutMs = 320;

lv_timer_t *shadesCameoTimer = nullptr;  // recurring: schedules cameos
lv_timer_t *shadesHideTimer = nullptr;   // one-shot: ends the current cameo

void shadesDropExecCb(void *, int32_t v) {
  ch.shadesDropY = static_cast<float>(v);
  renderFace();
}

void setShadesHidden(bool hidden) {
  lv_obj_t *parts[] = {sunglassesL, sunglassesR, sunglassGlare[0], sunglassGlare[1],
                       sunglassBridge};
  for (lv_obj_t *o : parts) {
    if (hidden) {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void endShadesCameo() {
  // repeat_count=1 means LVGL frees this timer as soon as the callback
  // returns, so this must only drop the reference -- deleting it here is
  // the double-free that crashed the express handler once already.
  shadesHideTimer = nullptr;
  sunglassesVisible = false;
  setShadesHidden(true);
  ch.shadesDropY = 0;
  renderFace();
}

void shadesHideCb(lv_timer_t *) { endShadesCameo(); }

void playShadesCameo() {
  if (cameosSuppressed) return;
  if (shadesHideTimer != nullptr) return;  // already wearing them
  // Only one piece of scenery at a time. playRainCameo() holds the
  // mirror-image guard, so whichever fires first keeps the stage.
  if (rainCameoActive) return;

  sunglassesVisible = true;
  setShadesHidden(false);
  ch.shadesDropY = static_cast<float>(kShadesDropFrom);
  renderFace();

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &ch);
  lv_anim_set_exec_cb(&a, shadesDropExecCb);
  lv_anim_set_values(&a, kShadesDropFrom, 0);
  lv_anim_set_time(&a, kShadesInMs);
  lv_anim_set_playback_time(&a, kShadesOutMs);
  lv_anim_set_playback_delay(&a, kShadesHoldMs);
  lv_anim_set_path_cb(&a, lv_anim_path_overshoot);  // lands with a small bounce
  lv_anim_start(&a);

  shadesHideTimer =
      lv_timer_create(shadesHideCb, kShadesInMs + kShadesHoldMs + kShadesOutMs + 40, nullptr);
  lv_timer_set_repeat_count(shadesHideTimer, 1);
}

void shadesCameoSchedCb(lv_timer_t *t) {
  playShadesCameo();
  lv_timer_set_period(t, lv_rand(48000, 115000));
}

void stopWeatherEffects() {
  if (rainTimer != nullptr) {
    lv_timer_del(rainTimer);
    rainTimer = nullptr;
    for (auto *streak : rainStreaks) lv_obj_add_flag(streak, LV_OBJ_FLAG_HIDDEN);
  }
  if (shadesCameoTimer != nullptr) {
    lv_timer_del(shadesCameoTimer);
    shadesCameoTimer = nullptr;
  }
  if (shadesHideTimer != nullptr) {
    // Still pending, so we own it and must free it (unlike from inside
    // its own callback, where repeat_count=1 has already freed it).
    lv_timer_del(shadesHideTimer);
    shadesHideTimer = nullptr;
  }
  lv_anim_del(&ch, shadesDropExecCb);
  sunglassesVisible = false;
  setShadesHidden(true);
  ch.shadesDropY = 0;
  if (shiverTimer != nullptr) {
    lv_timer_del(shiverTimer);
    shiverTimer = nullptr;
    ch.faceOffX = 0;
    ch.faceOffY = 0;
    renderFace();
  }
}

// --- Whistling ---
lv_timer_t *whistleNoteTimer = nullptr;

void whistleNoteCb(lv_timer_t *t) {
  spawnNote();
  // Roughly one note per second. Each drifts for 1.9s, so two are usually
  // in the air at once -- which is the point of having two canvases.
  lv_timer_set_period(t, lv_rand(850, 1400));
}

// Everything a whistle continuously owns: the pursed mouth, a side-to-side
// sway, and a smaller vertical bob. Separate from startWhistling() because
// expressions tear these channels down on their way out -- the gape in
// clearExpressionDecorations(), and the tilt in the `wave` expression --
// and the mood underneath has to get them back.
void restartWhistleMotion() {
  if (!whistling) return;

  // A small circle that keeps changing size: a whistle is a pitch, and a
  // fixed hole reads as a gasp instead.
  setGapeMode(GapeMode::ROUND);
  lv_anim_t mouth;
  lv_anim_init(&mouth);
  lv_anim_set_var(&mouth, &ch);
  lv_anim_set_exec_cb(&mouth, gapeScaleExecCb);
  lv_anim_set_values(&mouth, 120, 300);
  lv_anim_set_time(&mouth, 900);
  lv_anim_set_playback_time(&mouth, 1100);  // uneven, so it does not tick
  lv_anim_set_repeat_count(&mouth, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&mouth, lv_anim_path_ease_in_out);
  lv_anim_start(&mouth);

  // Head sway. 1700ms per full cycle against the mouth's 2000ms, so the
  // two drift in and out of phase instead of locking into one beat.
  lv_anim_t sway;
  lv_anim_init(&sway);
  lv_anim_set_var(&sway, &ch);
  lv_anim_set_exec_cb(&sway, tiltExecCb);
  lv_anim_set_values(&sway, -kWhistleSwayTenths, kWhistleSwayTenths);
  lv_anim_set_time(&sway, 850);
  lv_anim_set_playback_time(&sway, 850);
  lv_anim_set_repeat_count(&sway, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&sway, lv_anim_path_ease_in_out);
  lv_anim_start(&sway);

  // Vertical bob at roughly twice the sway rate, so it reads as nodding
  // along rather than just leaning. Uses postureExecCb because that is
  // already the faceOffY writer -- a second callback on the same channel
  // would not replace it, it would fight it.
  lv_anim_t bob;
  lv_anim_init(&bob);
  lv_anim_set_var(&bob, &ch);
  lv_anim_set_exec_cb(&bob, postureExecCb);
  lv_anim_set_values(&bob, 0, -kWhistleBobPx);
  lv_anim_set_time(&bob, 410);
  lv_anim_set_playback_time(&bob, 430);
  lv_anim_set_repeat_count(&bob, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&bob, lv_anim_path_ease_in_out);
  lv_anim_start(&bob);
}

lv_timer_t *whistleEndTimer = nullptr;
void stopWhistling();

void whistleEndCb(lv_timer_t *) {
  // repeat_count=1: LVGL frees this on return, so only drop the reference.
  whistleEndTimer = nullptr;
  stopWhistling();
}

// A short burst, started either by the idle flourish scheduler or by
// /api/express. It owns its own lifetime rather than going through the
// shared expression timer, the same way the glitch effect does.
void playWhistle() {
  if (whistling) return;
  whistling = true;
  ch.gapeScale = 0.15f;
  restartWhistleMotion();

  spawnNote();  // one straight away, so the burst starts with a note
  whistleNoteTimer = lv_timer_create(whistleNoteCb, lv_rand(850, 1400), nullptr);

  whistleEndTimer = lv_timer_create(whistleEndCb, kWhistleDurationMs, nullptr);
  lv_timer_set_repeat_count(whistleEndTimer, 1);
}

void stopWhistling() {
  if (!whistling) return;
  whistling = false;
  if (whistleEndTimer != nullptr) {
    // Still pending, so we own it (unlike from inside its own callback).
    lv_timer_del(whistleEndTimer);
    whistleEndTimer = nullptr;
  }
  if (whistleNoteTimer != nullptr) {
    lv_timer_del(whistleNoteTimer);
    whistleNoteTimer = nullptr;
  }
  lv_anim_del(&ch, gapeScaleExecCb);
  lv_anim_del(&ch, tiltExecCb);
  lv_anim_del(&ch, postureExecCb);
  ch.gapeScale = 0;
  ch.tiltTenths = 0;
  ch.faceOffY = 0;
  hideNotes();
  // A shock reaction may have taken the gape mid-whistle; leave it alone.
  if (gapeMode != GapeMode::SHOCK) setGapeMode(GapeMode::HIDDEN);
  renderFace();
}

// --- Sleepy: periodic yawns ---
lv_timer_t *sleepyYawnTimer = nullptr;

void sleepyYawnCb(lv_timer_t *t) {
  playMouthYawn();
  lv_timer_set_period(t, lv_rand(16000, 38000));
}

void startSleepyYawns() {
  if (sleepyYawnTimer != nullptr) return;
  // First yawn soon after the wind-down starts, so the state announces
  // itself rather than looking like nothing happened.
  sleepyYawnTimer = lv_timer_create(sleepyYawnCb, 2500, nullptr);
}

void stopSleepyYawns() {
  if (sleepyYawnTimer == nullptr) return;
  lv_timer_del(sleepyYawnTimer);
  sleepyYawnTimer = nullptr;
}

// --- Sleeping: snoring ---
// One cycle is a long inhale with the mouth falling open into a small O,
// then a slower exhale, with a bubble swelling and popping over the top.
constexpr uint32_t kSnoreCycleMs = 4200;
lv_timer_t *snoreTimer = nullptr;

void bubbleScaleExecCb(void *, int32_t permille) {
  ch.bubbleScale = permille / 1000.0f;
  renderFace();
}

// The bubble vanishing at full size IS the pop -- no shrink, no fade. An
// abrupt disappearance is what reads as bursting; easing it out just looks
// like it deflated.
void bubblePopCb(lv_anim_t *) {
  snoreBubbleVisible = false;
  ch.bubbleScale = 0.0f;
  lv_obj_add_flag(snoreBubble, LV_OBJ_FLAG_HIDDEN);
  renderFace();
}

// One breath: the mouth falls open and a bubble inflates from it, then the
// bubble pops. Shared by the sleeping snore loop and the one-shot `sleepy`
// expression.
void playSnoreBeat() {
  playChannelOutAndBack(gapeScaleExecCb, 520, 1500, 1400, 250);

  snoreBubbleVisible = true;
  ch.bubbleScale = 0.0f;
  lv_obj_clear_flag(snoreBubble, LV_OBJ_FLAG_HIDDEN);

  lv_anim_t b;
  lv_anim_init(&b);
  lv_anim_set_var(&b, &ch);
  lv_anim_set_exec_cb(&b, bubbleScaleExecCb);
  lv_anim_set_values(&b, 0, 1000);
  lv_anim_set_time(&b, 2100);
  lv_anim_set_path_cb(&b, lv_anim_path_ease_out);  // swells fast, then strains
  lv_anim_set_ready_cb(&b, bubblePopCb);
  lv_anim_start(&b);
}

void hideSnoreBubble() {
  lv_anim_del(&ch, bubbleScaleExecCb);
  snoreBubbleVisible = false;
  ch.bubbleScale = 0.0f;
  lv_obj_add_flag(snoreBubble, LV_OBJ_FLAG_HIDDEN);
}

void snoreTickCb(lv_timer_t *) { playSnoreBeat(); }

void startSnoring() {
  if (snoreTimer != nullptr) return;
  snoring = true;
  ch.gapeScale = 0.0f;
  setGapeMode(GapeMode::ROUND);
  snoreTimer = lv_timer_create(snoreTickCb, kSnoreCycleMs, nullptr);
  snoreTickCb(nullptr);  // snore straight away, not a full cycle from now
}

void stopSnoring() {
  if (snoreTimer != nullptr) {
    lv_timer_del(snoreTimer);
    snoreTimer = nullptr;
  }
  snoring = false;
  lv_anim_del(&ch, gapeScaleExecCb);
  ch.gapeScale = 0;
  // A shock reaction owns the gape for its 2.5s, and a whistle owns it for
  // the whole mood; a mood change landing in the middle of either must not
  // snatch the open mouth back off it.
  if (gapeMode != GapeMode::SHOCK && !whistling) setGapeMode(GapeMode::HIDDEN);
  hideSnoreBubble();
  renderFace();
}

// --- Rain-forecast cameo ---
// A heads-up, not a weather report: the cloud drifts in, sheds a few
// drops, the face glances up at it, and it leaves again. Long random
// period so it stays a moment rather than becoming furniture.
constexpr uint32_t kRainCameoInMs = 520;
constexpr uint32_t kRainCameoHoldMs = 3400;
constexpr uint32_t kRainCameoOutMs = 460;
// The face slides this far right, away from the cloud arriving on its
// left. Bigger than it looks: turning to face the cloud ALREADY shifts
// the features ~15px left (the yaw cylinder swings them that way), so the
// first ~15px of recoil only cancels the turn out and produces no visible
// movement at all. 26 nets about +10px of actual travel. The ceiling is
// the water bottle -- face right edge sits at 254 with this, against the
// bottle's 258, so a hydration reminder overlapping the cameo still
// cannot collide.
constexpr lv_coord_t kRainRecoilPx = 26;

bool rainForecastExpected = false;
// Whether this cameo is the thing currently holding the open mouth. Only
// what took it may give it back -- a shock landing mid-cameo owns it
// instead, and must be left alone.
bool rainCameoTookGape = false;
lv_timer_t *rainCameoTimer = nullptr;   // recurring: schedules cameos
lv_timer_t *rainCameoEndTimer = nullptr;  // one-shot: ends the current one
lv_timer_t *rainDropTimer = nullptr;      // drives the falling drops

void setRainCameoHidden(bool hidden) {
  lv_obj_t *parts[] = {cloudCanvas, rainDrops[0], rainDrops[1], rainDrops[2]};
  for (lv_obj_t *o : parts) {
    if (hidden) {
      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void rainDropTickCb(lv_timer_t *) {
  for (int i = 0; i < kRainDropCount; i++) {
    dropFall[i] += 0.085f;
    if (dropFall[i] >= 1.0f) dropFall[i] = 0.0f;  // back up into the cloud
  }
  updateCloudPos();
}

// Hands the open mouth back, if this cameo was the one holding it.
void releaseRainCameoGape() {
  if (!rainCameoTookGape) return;
  rainCameoTookGape = false;
  if (snoring || whistling || gapeMode == GapeMode::SHOCK) return;
  lv_anim_del(&ch, gapeScaleExecCb);
  ch.gapeScale = 0;
  setGapeMode(GapeMode::HIDDEN);
}

void endRainCameo() {
  // repeat_count=1 auto-frees this timer on return -- only drop the
  // reference, never delete it from in here.
  rainCameoEndTimer = nullptr;
  if (rainDropTimer != nullptr) {
    lv_timer_del(rainDropTimer);
    rainDropTimer = nullptr;
  }
  rainCameoActive = false;
  releaseRainCameoGape();
  cloudIn = 0.0f;
  for (int i = 0; i < kRainDropCount; i++) dropFall[i] = 0.0f;
  updateCloudPos();
  setRainCameoHidden(true);
}

void rainCameoEndCb(lv_timer_t *) { endRainCameo(); }

void playRainCameo() {
  if (cameosSuppressed) return;
  if (rainCameoActive) return;
  // Don't stack scenery: the shades own the face's attention while they
  // are down.
  if (sunglassesVisible) return;

  rainCameoActive = true;
  cloudIn = 0.0f;
  for (int i = 0; i < kRainDropCount; i++) dropFall[i] = static_cast<float>(i) * 0.33f;
  updateCloudPos();
  setRainCameoHidden(false);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, &cloudIn);
  lv_anim_set_exec_cb(&a, cloudInExecCb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_time(&a, kRainCameoInMs);
  lv_anim_set_playback_time(&a, kRainCameoOutMs);
  lv_anim_set_playback_delay(&a, kRainCameoHoldMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);

  if (rainDropTimer == nullptr) rainDropTimer = lv_timer_create(rainDropTickCb, 55, nullptr);

  // The face backs away from it: the cloud comes in on the left, so the
  // whole face slides right, which also clears the space the cloud is
  // arriving into.
  playChannelOutAndBack(faceOffXExecCb, kRainRecoilPx, 480, 640, kRainCameoHoldMs - 300,
                        lv_anim_path_ease_out);

  // ...and looks up at it, surprised. The head turn stays modest because
  // the idle gaze writes the same channels and may cut across it -- a
  // subtle look survives that interruption where a big one would snap.
  // The surprise is carried by the eyes and mouth instead, which nothing
  // else is competing for.
  playChannelOutAndBack(headYawExecCb, -300, 520, 620, kRainCameoHoldMs - 400,
                        lv_anim_path_ease_out);
  playChannelOutAndBack(headPitchExecCb, -220, 520, 620, kRainCameoHoldMs - 400,
                        lv_anim_path_ease_out);

  // Eyes go wide (over 1000 = wider than the mood baseline).
  playEyeScale(1220, 380, 560, kRainCameoHoldMs - 200);

  // Open mouth -- but only if nothing else already owns it. A shock
  // reaction holds the gape for its own 2.5s and must not be interrupted.
  if (gapeMode == GapeMode::HIDDEN) {
    rainCameoTookGape = true;
    ch.gapeScale = 0.0f;
    setGapeMode(GapeMode::ROUND);
    lv_anim_t g;
    lv_anim_init(&g);
    lv_anim_set_var(&g, &ch);
    lv_anim_set_exec_cb(&g, gapeScaleExecCb);
    lv_anim_set_values(&g, 0, 560);
    lv_anim_set_time(&g, 380);
    lv_anim_set_playback_time(&g, 520);
    lv_anim_set_playback_delay(&g, kRainCameoHoldMs - 500);
    lv_anim_set_path_cb(&g, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&g, yawnDoneCb);  // puts the arc mouth back
    lv_anim_start(&g);
  }

  if (rainCameoEndTimer != nullptr) lv_timer_del(rainCameoEndTimer);
  rainCameoEndTimer = lv_timer_create(
      rainCameoEndCb, kRainCameoInMs + kRainCameoHoldMs + kRainCameoOutMs + 40, nullptr);
  lv_timer_set_repeat_count(rainCameoEndTimer, 1);
}

void rainCameoSchedCb(lv_timer_t *t) {
  playRainCameo();
  lv_timer_set_period(t, lv_rand(70000, 160000));
}

void stopRainForecast() {
  if (rainCameoTimer != nullptr) {
    lv_timer_del(rainCameoTimer);
    rainCameoTimer = nullptr;
  }
  if (rainCameoEndTimer != nullptr) {
    lv_timer_del(rainCameoEndTimer);
    rainCameoEndTimer = nullptr;
  }
  if (rainDropTimer != nullptr) {
    lv_timer_del(rainDropTimer);
    rainDropTimer = nullptr;
  }
  lv_anim_del(&cloudIn, cloudInExecCb);
  lv_anim_del(&ch, faceOffXExecCb);
  ch.faceOffX = 0;
  rainCameoActive = false;
  releaseRainCameoGape();
  cloudIn = 0.0f;
  for (int i = 0; i < kRainDropCount; i++) dropFall[i] = 0.0f;
  updateCloudPos();
  setRainCameoHidden(true);
}

WeatherService::Overlay activeWeatherOverlay = WeatherService::Overlay::NONE;
bool weatherSuppressed = false;

void startWeatherEffect(WeatherService::Overlay overlay) {
  switch (overlay) {
    case WeatherService::Overlay::RAIN:
      for (auto *streak : rainStreaks) {
        lv_obj_set_y(streak, lv_rand(-LCD_HEIGHT, LCD_HEIGHT));
        lv_obj_clear_flag(streak, LV_OBJ_FLAG_HIDDEN);
      }
      rainTimer = lv_timer_create(rainTickCb, 50, nullptr);
      break;
    case WeatherService::Overlay::SUNGLASSES:
      // Not immediately on connect -- the first weather fetch lands a few
      // seconds after boot, and shades appearing right then looks like a
      // startup glitch rather than a joke.
      shadesCameoTimer = lv_timer_create(shadesCameoSchedCb, 25000, nullptr);
      break;
    case WeatherService::Overlay::SHIVER:
      shiverTimer = lv_timer_create(shiverTickCb, 70, nullptr);
      break;
    default:
      break;
  }
}

void refreshWeatherEffect() {
  stopWeatherEffects();
  if (!weatherSuppressed) startWeatherEffect(activeWeatherOverlay);
}

}  // namespace

namespace DisplayEngine {

void init() {
  display.init();
  display.setRotation(ConfigStore::get().displayFlipped ? LCD_ROTATION_FLIPPED : LCD_ROTATION);
  display.setBrightness(0);

  lv_init();

  buf1 = static_cast<lv_color_t *>(
      heap_caps_malloc(kBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  buf2 = static_cast<lv_color_t *>(
      heap_caps_malloc(kBufPixels * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  lv_disp_draw_buf_init(&drawBuf, buf1, buf2, kBufPixels);

  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = LCD_WIDTH;
  dispDrv.ver_res = LCD_HEIGHT;
  dispDrv.flush_cb = dispFlush;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);

  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

  // Nothing on this screen scrolls, and leaving scrolling enabled cost us a
  // permanent grey bar along the bottom of the panel.
  //
  // The default theme puts a grey scrollbar style (LV_PALETTE_GREY at
  // LV_OPA_40) on every parentless object, and a screen's default scrollbar
  // mode is LV_SCROLLBAR_MODE_AUTO -- "draw whenever a child sticks out".
  // The glitch bands are LCD_WIDTH wide and get torn sideways by up to
  // +10px, so their right edge lands past 319 and AUTO starts drawing a
  // horizontal scrollbar at the bottom edge. Hiding the bands again
  // retracts the scroll extent, so LVGL simply stops drawing the scrollbar
  // -- it does NOT invalidate the strip it was living in, and renderFace()
  // never touches y=171, so the grey pixels stayed on the panel forever.
  //
  // The cloud, the wave hand and the drifting notes can all leave the frame
  // too, so this is fixed at the screen rather than per-animation.
  lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(lv_scr_act(), LV_SCROLLBAR_MODE_OFF);

  eyeLeft = createEye(kEyeLeftX);
  eyeRight = createEye(kEyeRightX);
  mouth = createMouth();

  // Hydration bottle: cap, then body outline, then the water on top of it
  // (creation order is paint order).
  bottleCap = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(bottleCap, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(bottleCap, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(bottleCap, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bottleCap, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bottleCap, 2, LV_PART_MAIN);
  lv_obj_set_size(bottleCap, kBottleCapW, kBottleCapH);
  lv_obj_set_pos(bottleCap, kBottleX + (kBottleW - kBottleCapW) / 2, kBottleY - kBottleCapH);
  lv_obj_add_flag(bottleCap, LV_OBJ_FLAG_HIDDEN);

  bottleBody = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(bottleBody, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(bottleBody, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(bottleBody, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(bottleBody, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_width(bottleBody, 2, LV_PART_MAIN);
  lv_obj_set_style_border_opa(bottleBody, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bottleBody, 6, LV_PART_MAIN);
  lv_obj_set_size(bottleBody, kBottleW, kBottleH);
  lv_obj_set_pos(bottleBody, kBottleX, kBottleY);
  lv_obj_add_flag(bottleBody, LV_OBJ_FLAG_HIDDEN);

  bottleFill = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(bottleFill, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(bottleFill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  // lv_color_make, not lv_palette_main -- the palette path is what
  // rendered the hearts green once (see the dispFlush byte-order note).
  lv_obj_set_style_bg_color(bottleFill, lv_color_make(48, 130, 235), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bottleFill, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bottleFill, 3, LV_PART_MAIN);
  lv_obj_add_flag(bottleFill, LV_OBJ_FLAG_HIDDEN);
  updateBottleFill();

  // Snore bubble: an outline, not a blob -- a filled circle reads as a
  // ball, whereas a ring reads as something inflated.
  snoreBubble = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(snoreBubble, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(snoreBubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(snoreBubble, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_color(snoreBubble, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_width(snoreBubble, 2, LV_PART_MAIN);
  lv_obj_set_style_border_opa(snoreBubble, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(snoreBubble, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_size(snoreBubble, kBubbleMin, kBubbleMin);
  lv_obj_add_flag(snoreBubble, LV_OBJ_FLAG_HIDDEN);

  // Wide-open "gasp" mouth for shock -- a filled oval swapped in for the
  // usual thin arc, rather than trying to stretch the arc into looking
  // open.
  wideOpenMouth = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(wideOpenMouth, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(wideOpenMouth, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(wideOpenMouth, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(wideOpenMouth, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(wideOpenMouth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_size(wideOpenMouth, kGapeShockW, kGapeShockH);
  lv_obj_add_flag(wideOpenMouth, LV_OBJ_FLAG_HIDDEN);
  // Position comes from renderFace() -- it tracks the face's gaze/breath
  // offsets rather than sitting at a fixed spot.

  // Heart eyes -- drawn once onto small canvases (two filled circles +
  // a filled triangle), see drawHeartShape()'s comment for why this
  // replaced two earlier per-object approaches.
  for (int i = 0; i < 2; i++) {
    heartCanvas[i] = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(heartCanvas[i], heartCanvasBuf[i], kHeartCanvasW, kHeartCanvasH, LV_IMG_CF_TRUE_COLOR_ALPHA);
    drawHeartShape(heartCanvas[i]);
    lv_obj_add_flag(heartCanvas[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Brow wedges. Created after the eyes so they paint on top of them, and
  // before the sunglasses so shades still cover everything.
  for (int i = 0; i < 2; i++) {
    browCanvas[i] = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(browCanvas[i], browCanvasBuf[i], kBrowW, kBrowH,
                         LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_add_flag(browCanvas[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Glitch scanline bands + red/cyan channel-split ghosts of the eyes.
  for (int i = 0; i < kGlitchBandCount; i++) {
    lv_obj_t *band = lv_obj_create(lv_scr_act());
    lv_obj_remove_style(band, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_color_t colors[] = {lv_palette_main(LV_PALETTE_RED), lv_palette_main(LV_PALETTE_CYAN),
                            lv_palette_main(LV_PALETTE_LIME), lv_color_white()};
    lv_obj_set_style_bg_color(band, colors[i % 4], LV_PART_MAIN);
    lv_obj_set_style_bg_opa(band, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_size(band, LCD_WIDTH, 3);
    lv_obj_add_flag(band, LV_OBJ_FLAG_HIDDEN);
    glitchBands[i] = band;
  }
  auto makeGhost = [](lv_color_t colour) {
    lv_obj_t *g = lv_obj_create(lv_scr_act());
    lv_obj_remove_style(g, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g, colour, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_radius(g, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_size(g, kEyeWidth, kEyeHeight);
    lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
    return g;
  };
  glitchGhostRedL = makeGhost(lv_color_make(255, 40, 40));
  glitchGhostRedR = makeGhost(lv_color_make(255, 40, 40));
  glitchGhostCyanL = makeGhost(lv_color_make(40, 240, 255));
  glitchGhostCyanR = makeGhost(lv_color_make(40, 240, 255));

  // Whole-screen wash for the rare lost-sync frame. Created last of the
  // glitch parts so it paints over them.
  glitchFlash = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(glitchFlash, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(glitchFlash, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(glitchFlash, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(glitchFlash, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_radius(glitchFlash, 0, LV_PART_MAIN);
  lv_obj_set_size(glitchFlash, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_pos(glitchFlash, 0, 0);
  lv_obj_add_flag(glitchFlash, LV_OBJ_FLAG_HIDDEN);

  // Music note for the whistling mood.
  for (int i = 0; i < 2; i++) {
    noteCanvas[i] = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(noteCanvas[i], noteCanvasBuf[i], kNoteW, kNoteH,
                         LV_IMG_CF_TRUE_COLOR_ALPHA);
    drawNoteShape(noteCanvas[i]);
    lv_obj_add_flag(noteCanvas[i], LV_OBJ_FLAG_HIDDEN);
  }

  // Waving hand.
  handCanvas = lv_canvas_create(lv_scr_act());
  lv_canvas_set_buffer(handCanvas, handCanvasBuf, kHandW, kHandH, LV_IMG_CF_TRUE_COLOR_ALPHA);
  drawHandShape(handCanvas);
  lv_img_set_pivot(handCanvas, kHandPivotX, kHandPivotY);
  lv_obj_set_pos(handCanvas, kHandCx - kHandW / 2, kHandCy - kHandH / 2);
  lv_obj_add_flag(handCanvas, LV_OBJ_FLAG_HIDDEN);

  // Rain-forecast cloud + its drops.
  cloudCanvas = lv_canvas_create(lv_scr_act());
  lv_canvas_set_buffer(cloudCanvas, cloudCanvasBuf, kCloudW, kCloudH, LV_IMG_CF_TRUE_COLOR_ALPHA);
  drawCloudShape(cloudCanvas);
  lv_obj_add_flag(cloudCanvas, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < kRainDropCount; i++) {
    lv_obj_t *d = lv_obj_create(lv_scr_act());
    lv_obj_remove_style(d, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(d, lv_color_make(48, 130, 235), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(d, kDropW / 2, LV_PART_MAIN);
    lv_obj_set_size(d, kDropW, kDropH);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    rainDrops[i] = d;
  }
  updateCloudPos();

  // Weather: rain streaks + sunglasses, both hidden until active.
  for (int i = 0; i < kRainStreakCount; i++) {
    lv_obj_t *streak = lv_obj_create(lv_scr_act());
    lv_obj_remove_style(streak, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_clear_flag(streak, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(streak, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(streak, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_radius(streak, 2, LV_PART_MAIN);
    lv_obj_set_size(streak, 3, 16);
    lv_obj_set_x(streak, (LCD_WIDTH / (kRainStreakCount + 1)) * (i + 1));
    lv_obj_add_flag(streak, LV_OBJ_FLAG_HIDDEN);
    rainStreaks[i] = streak;
  }
  // White capsule lenses. Created AFTER the eyes so they draw on top of
  // them (LVGL paints children in creation order), and sized to cover the
  // eye completely -- you shouldn't see eyes through shades.
  auto makeSunglassLens = []() {
    lv_obj_t *lens = lv_obj_create(lv_scr_act());
    lv_obj_remove_style(lens, nullptr, LV_PART_MAIN | LV_STATE_ANY);
    lv_obj_clear_flag(lens, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(lens, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lens, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(lens, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_size(lens, kLensW, kLensH);
    lv_obj_add_flag(lens, LV_OBJ_FLAG_HIDDEN);
    return lens;
  };
  sunglassesL = makeSunglassLens();
  sunglassesR = makeSunglassLens();

  // Glare: two slanted black streaks per lens, on a transparent canvas.
  // A canvas polygon is used for the same reason the hearts needed one --
  // a diagonal can't be had from a plain object, since rotation via
  // transform_angle is not honoured for non-image widgets here.
  for (int i = 0; i < 2; i++) {
    sunglassGlare[i] = lv_canvas_create(lv_scr_act());
    lv_canvas_set_buffer(sunglassGlare[i], sunglassGlareBuf[i], kGlareW, kGlareH,
                         LV_IMG_CF_TRUE_COLOR_ALPHA);
    drawGlareShape(sunglassGlare[i]);
    lv_obj_add_flag(sunglassGlare[i], LV_OBJ_FLAG_HIDDEN);
  }

  sunglassBridge = lv_obj_create(lv_scr_act());
  lv_obj_remove_style(sunglassBridge, nullptr, LV_PART_MAIN | LV_STATE_ANY);
  lv_obj_clear_flag(sunglassBridge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(sunglassBridge, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(sunglassBridge, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(sunglassBridge, 2, LV_PART_MAIN);
  lv_obj_set_size(sunglassBridge, kBridgeW, kBridgeH);
  lv_obj_add_flag(sunglassBridge, LV_OBJ_FLAG_HIDDEN);

  renderFace();
  startIdleSystems();
}

void loop() { lv_timer_handler(); }

// Both rotations are the same landscape shape, so hor_res/ver_res and the
// draw buffers are unchanged -- only the panel's scan mapping flips. That
// means this is safe to call at runtime; a full invalidate repaints.
void setDisplayFlipped(bool flipped) {
  display.setRotation(flipped ? LCD_ROTATION_FLIPPED : LCD_ROTATION);
  lv_obj_invalidate(lv_scr_act());
}

void setBrightnessPercent(uint8_t percent) {
  if (percent > BACKLIGHT_MAX_SUSTAINED_PERCENT) {
    percent = BACKLIGHT_MAX_SUSTAINED_PERCENT;
  }
  // Perceptual (gamma) curve rather than linear duty, per brief section 5.
  float normalized = percent / 100.0f;
  uint8_t duty = static_cast<uint8_t>(powf(normalized, 2.2f) * 255.0f + 0.5f);
  display.setBrightness(duty);
}

namespace {
void brightnessFadeExecCb(void *, int32_t duty) { display.setBrightness(static_cast<uint8_t>(duty)); }
}  // namespace

void fadeBrightnessPercent(uint8_t percent, uint32_t durationMs) {
  if (percent > BACKLIGHT_MAX_SUSTAINED_PERCENT) percent = BACKLIGHT_MAX_SUSTAINED_PERCENT;
  float normalized = percent / 100.0f;
  uint8_t targetDuty = static_cast<uint8_t>(powf(normalized, 2.2f) * 255.0f + 0.5f);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_exec_cb(&a, brightnessFadeExecCb);
  lv_anim_set_values(&a, display.getBrightness(), targetDuty);
  lv_anim_set_time(&a, durationMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_start(&a);
}

void applyMoodState(MoodEngine::State state) {
  static MoodEngine::State lastState = MoodEngine::State::NEUTRAL;
  bool stateChanged = (state != lastState);
  lastState = state;

  // The full sustained pose for this state. Everything here is a resting
  // value; the idle channels animate relative to it.
  lv_coord_t targetEyeHeight = kEyeHeight;
  uint16_t targetMouthStart = kMouthAngleStart;
  uint16_t targetMouthEnd = kMouthAngleEnd;
  lv_coord_t targetDrift = 0;
  float targetPitch = 0.0f;
  lv_coord_t browInner = 0;
  lv_coord_t browOuter = 0;
  uint32_t breathHalfMs = 2200;
  int32_t breathAmp = 150;
  uint32_t flourishMin = 7000;
  uint32_t flourishMax = 19000;
  bool showBottle = false;
  bool wantSnore = false;
  bool wantYawns = false;
  bool wantSips = false;
  bool wantStretch = false;
  // Sustained states own the face outright -- idle blink/look/pout/squint/
  // tilt would otherwise fight the sustained pose for the same objects.
  // Dialled down rather than switched off: asleep still breathes, a held
  // reminder pose still blinks. See setIdleLevel().
  IdleLevel level = IdleLevel::FULL;

  switch (state) {
    case MoodEngine::State::SLEEPING:
      // Eyes shut, head down, snoring. Breathing goes slow and deep.
      targetEyeHeight = static_cast<lv_coord_t>(kEyeHeight * 0.07f);
      targetPitch = 0.30f;
      // Phase-locked to the snore, and inverted. Half a snore cycle, so
      // one rise-and-fall per breath; NEGATIVE amplitude so the face
      // rises while the mouth is opening rather than sinking into it --
      // startBreathing() animates -amp -> +amp, so a negative value
      // starts it at the top. Both animations are kicked off in the same
      // applyMoodState() call, which is what keeps them in step.
      breathHalfMs = kSnoreCycleMs / 2;
      breathAmp = -340;
      wantSnore = true;
      level = IdleLevel::MINIMAL;
      break;
    case MoodEngine::State::SLEEPY:
      // Heavy lids and a drooping head, yawning every 15-40s. No Zzz --
      // that belongs to actually being asleep; this is still awake.
      targetEyeHeight = static_cast<lv_coord_t>(kEyeHeight * 0.42f);
      targetMouthStart = 70;
      targetMouthEnd = 110;
      targetPitch = 0.22f;
      browInner = 6;
      browOuter = 13;  // outer ends sagging: tired, not cross
      breathHalfMs = 3000;
      breathAmp = 210;
      wantYawns = true;
      // REDUCED rather than MINIMAL so it still blinks and can yawn --
      // MINIMAL kills the blink and made the wind-down look switched off.
      level = IdleLevel::REDUCED;
      break;
    case MoodEngine::State::BORED:
      targetEyeHeight = static_cast<lv_coord_t>(kEyeHeight * 0.62f);
      targetMouthStart = 76;
      targetMouthEnd = 104;  // flat, unbothered
      targetPitch = 0.10f;
      browInner = 8;
      browOuter = 13;
      breathHalfMs = 3000;
      breathAmp = 180;
      flourishMin = 11000;
      flourishMax = 26000;  // barely stirs
      break;
    case MoodEngine::State::EXCITED:
      targetEyeHeight = static_cast<lv_coord_t>(kEyeHeight * 1.12f);  // eyes wide
      targetMouthStart = 24;
      targetMouthEnd = 156;
      targetPitch = -0.08f;  // chin up
      breathHalfMs = 1700;
      breathAmp = 190;
      flourishMin = 4000;
      flourishMax = 10000;  // fidgety
      break;
    case MoodEngine::State::FOCUSED:
      targetEyeHeight = static_cast<lv_coord_t>(kEyeHeight * 0.82f);
      targetMouthStart = 80;
      targetMouthEnd = 100;
      browInner = 15;
      browOuter = 3;  // inner ends down: the determined brow
      breathHalfMs = 2400;
      breathAmp = 120;
      flourishMin = 9000;
      flourishMax = 20000;
      break;
    case MoodEngine::State::HYDRATION_REMINDER:
      targetDrift = kHydrationDriftX;
      showBottle = true;
      wantSips = true;
      level = IdleLevel::REDUCED;  // holds the gaze on the bottle, but keeps blinking
      break;
    case MoodEngine::State::POSTURE_REMINDER:
      // A 20s reminder is long enough to own the face, so unlike GLITCHED
      // this does get a pose. REDUCED keeps it blinking while stopping the
      // idle flourishes from fighting the stretch for the same channels.
      wantStretch = true;
      level = IdleLevel::REDUCED;
      break;
    default:
      break;
  }

  // GLITCHED is an effect layered over whatever mood is running, not a
  // pose of its own -- it lands in `default:` above with every pose value
  // at its neutral, and applying that would animate the entire mood pose
  // out and back for the sake of 200-800ms. So the pose block is skipped
  // for it and the mood underneath keeps holding the face. POSTURE lasts
  // 20s and does get its own pose, but still suppresses weather.
  bool isPoseless = state == MoodEngine::State::GLITCHED;
  bool isTransientOverlay =
      isPoseless || state == MoodEngine::State::POSTURE_REMINDER;

  // Recorded on EVERY call, not just when the state changes, so it is
  // always current for an expression to restore. Skipped for poseless
  // states, whose locals are all neutral defaults and would otherwise
  // overwrite the real mood a glitch happened to interrupt.
  if (!isPoseless) {
    currentPose = {targetEyeHeight, targetMouthStart, targetMouthEnd,
                   targetDrift,     targetPitch,      browInner,
                   browOuter};
    // Recomputed every call, not just on change, so it stays right even if
    // the state is re-entered.
    cameosSuppressed = (state == MoodEngine::State::FOCUSED);
  }

  // applyMoodState() is called once a SECOND regardless of whether the
  // state actually changed (main.cpp's tick() contract). animateBaselineTo()
  // starts a real animation, so (re)starting it every second -- even toward
  // the same target -- restarted the baseline mid-flight and read as a
  // twitch. Only touch any of this when the state actually changed.
  if (stateChanged && !isPoseless) {
    // A whistle in flight has to end here. setIdleLevel() below tears
    // down the very channels it is driving, which would otherwise leave
    // `whistling` true with dead animations -- the mouth stuck open and
    // never released.
    stopWhistling();

    // Idle level first: it zeroes the animation channels, and the calls
    // below then install the new sustained pose on top.
    setIdleLevel(level);

    // An expression owns the face's SHAPE while it runs, so a mood rolling
    // mid-expression must not reach in and repaint the brows or baseline
    // over it. Nothing is lost by waiting: currentPose was updated above,
    // and clearExpressionDecorations() installs it when the expression
    // releases. Everything else below applies immediately -- falling
    // asleep during an expression should still start the snore.
    if (expressionTimer == nullptr) {
      animateBaselineTo(targetEyeHeight, targetMouthStart, targetMouthEnd, targetDrift);
      animateBasePitchTo(targetPitch, 600);
      setBrows(browInner, browOuter);
    }
    startBreathing(breathHalfMs, breathAmp);
    flourishMinMs = flourishMin;
    flourishMaxMs = flourishMax;

    if (wantSnore) {
      startSnoring();
    } else {
      stopSnoring();
    }
    if (wantYawns) {
      startSleepyYawns();
    } else {
      stopSleepyYawns();
    }
    if (wantSips) {
      startHydrationRoutine();
    } else {
      stopHydrationRoutine();
    }
    if (wantStretch) {
      startPostureRoutine();
    } else {
      stopPostureRoutine();
    }


    // The routine itself shows the bottle (and fills it); this only has to
    // take it away when the reminder is over.
    if (!showBottle) setBottleVisible(false);
  }

  // Weather is suppressed for the duration of a transient overlay
  // (glitch/posture) so it doesn't visually compete with it.
  if (isTransientOverlay != weatherSuppressed) {
    weatherSuppressed = isTransientOverlay;
    refreshWeatherEffect();
  }

  if (stateChanged && state == MoodEngine::State::GLITCHED) playGlitchEffect();
}

void applyRainForecast(bool expected) {
  if (expected == rainForecastExpected) return;
  rainForecastExpected = expected;
  if (expected) {
    // First cameo a little way in, then on a long random period.
    rainCameoTimer = lv_timer_create(rainCameoSchedCb, 30000, nullptr);
  } else {
    stopRainForecast();
  }
}

void applyWeatherOverlay(WeatherService::Overlay overlay) {
  if (overlay == activeWeatherOverlay) return;
  activeWeatherOverlay = overlay;
  refreshWeatherEffect();
}

constexpr uint32_t kExpressionHoldMs = 2500;

// An expression is a whole face, not a modifier on the current one. The
// mood's eye-shape treatments have to come off first or they show through:
// the brow wedges in particular are drawn ON TOP of the heart canvases
// (created later, so painted later), which is what made heart eyes look
// broken in `bored` and `focused`. clearExpressionDecorations() puts the
// mood's pose back afterwards.
void neutraliseMoodShaping() {
  setBrows(0, 0);
  animateBasePitchTo(0.0f, 220);
}

void triggerExpression(const char *expression) {
  // Glitch returns early below and is deliberately poseless, so it does
  // NOT get this treatment -- it is an effect layered over the mood.
  if (strcmp(expression, "glitch") != 0) neutraliseMoodShaping();

  if (strcmp(expression, "shock") == 0) {
    startShockLook();
    setShockMouthVisible(true);
  } else if (strcmp(expression, "heart") == 0) {
    // Pushed out to 128 deg: the old 40/140 was only 8 deg wider than the
    // widened resting mouth, so the smile no longer read as a change.
    animateExpressionTo(kEyeHeight, 26, 154, 0);
    setHeartVisible(true);
  } else if (strcmp(expression, "rage") == 0) {
    animateExpressionTo(static_cast<lv_coord_t>(kEyeHeight * 0.35f), 75, 105, 0);
    playShake(kExpressionHoldMs, 3);  // sustained tremor for as long as rage is shown
  } else if (strcmp(expression, "sleepy") == 0) {
    animateExpressionTo(static_cast<lv_coord_t>(kEyeHeight * 0.15f), kMouthAngleStart, kMouthAngleEnd, 0);
    // One snore beat rather than the old Zzz -- it is a 2.5s one-shot, so
    // a single bubble is the whole gag.
    if (gapeMode == GapeMode::HIDDEN) playSnoreBeat();
  } else if (strcmp(expression, "unimpressed") == 0) {
    // Half-lidded eyes over a short flat mouth -- the flat, unbothered
    // look from the reference sheet.
    animateExpressionTo(static_cast<lv_coord_t>(kEyeHeight * 0.38f), 78, 102, 0);
  } else if (strcmp(expression, "grin") == 0) {
    // Wide eyes and a big thick smile. The extra arc thickness is what
    // separates a grin from merely a wider version of the resting mouth.
    animateExpressionTo(kEyeHeight, 20, 160, 0);
    playChannelOutAndBack(mouthOpenExecCb, 700, 220, 380, kExpressionHoldMs - 640);
  } else if (strcmp(expression, "wave") == 0) {
    // "I need you." A hand comes up beside the face and waves, and the
    // face turns toward you with a small smile rather than the alarmed
    // look `shock` gives -- this is a request for attention, not a fright.
    setHandVisible(true);
    lv_anim_t w;
    lv_anim_init(&w);
    lv_anim_set_var(&w, handCanvas);
    lv_anim_set_exec_cb(&w, waveExecCb);
    lv_anim_set_values(&w, -200, 200);  // +-20 degrees about the wrist
    lv_anim_set_time(&w, 330);
    lv_anim_set_playback_time(&w, 330);
    lv_anim_set_repeat_count(&w, 3);
    lv_anim_set_path_cb(&w, lv_anim_path_ease_in_out);
    lv_anim_start(&w);

    // Shift left to make room for the hand, and lean with it so it is not
    // a flat slide. kWaveShiftPx is larger than the movement it produces:
    // turning to look at the hand already carries the features ~13px
    // RIGHT via the yaw cylinder, so the first 13px of shift only cancels
    // that out. -26 nets about -12px of real travel.
    playChannelOutAndBack(faceOffXExecCb, kWaveShiftPx, 440, 600, kExpressionHoldMs - 1050,
                          lv_anim_path_ease_out);
    // Negative tilt is counter-clockwise on screen (the rotation runs in a
    // y-down frame), so the top of the head goes away from the hand.
    playChannelOutAndBack(tiltExecCb, -45, 460, 620, kExpressionHoldMs - 1080);

    playChannelOutAndBack(headYawExecCb, 260, 420, 560, kExpressionHoldMs - 1000,
                          lv_anim_path_ease_out);
    playChannelOutAndBack(mouthExecCb, 420, 320, 520, kExpressionHoldMs - 900);
  } else if (strcmp(expression, "whistle") == 0) {
    // Owns its own lifetime, so it skips the shared expression timer
    // entirely -- same arrangement as glitch below.
    playWhistle();
    return;
  } else if (strcmp(expression, "glitch") == 0) {
    playGlitchEffect();
    return;  // glitch clears itself via its own timer, not the shared one below
  } else if (strcmp(expression, "posture") == 0) {
    // One complete cycle of the real reminder: stretch, then the footstep
    // bobs two seconds later. Matches how `hydrate` gives you one sip
    // rather than the whole 20s reminder.
    //
    // Returns early like whistle and glitch: postureRoutineCb's animations
    // all terminate on their own and outlast the 2.5s expression hold, so
    // routing it through the shared expression timer would cut the walk
    // off halfway.
    postureRoutineCb(nullptr);
    return;
  } else if (strcmp(expression, "hydrate") == 0) {
    animateExpressionTo(kEyeHeight, kMouthAngleStart, kMouthAngleEnd, kHydrationDriftX);
    bottleLevel = 1.0f;
    updateBottleFill();
    setBottleVisible(true);
    hydrationSipCb(nullptr);  // one sip; the 2.5s hold then clears it
  } else {
    return;
  }

  // All the (non-glitch) express reactions above are one-shot: hold
  // briefly, then return to whatever MoodEngine's actual state implies.
  // ASSUMPTION: 2.5s hold -- the brief doesn't give a duration for manual
  // expressions specifically.
  if (expressionTimer != nullptr) lv_timer_del(expressionTimer);
  expressionTimer = lv_timer_create(clearExpressionDecorations, kExpressionHoldMs, nullptr);
  lv_timer_set_repeat_count(expressionTimer, 1);
}

}  // namespace DisplayEngine
