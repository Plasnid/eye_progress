#include <SPI.h>
#include <string.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

// Custom Pin Definitions (Locked)
#define TFT_CS     9
#define TFT_DC     5
#define TFT_RST    6

Adafruit_GC9A01A tft = Adafruit_GC9A01A(TFT_CS, TFT_DC, TFT_RST);

// Define 16-bit 565 colors
#define BG_COLOR 0x0000
#define SCLERA   0x0000 // black "eye white" background
#define PUPIL    0x0000

// The iris continuously cycles through the full color spectrum over this
// period (ms), independent of whatever base color it started at.
const unsigned long IRIS_HUE_CYCLE_MS = 30000;

const int CENTER_X = 120;
const int CENTER_Y = 120;
const int SCLERA_RADIUS = 120;
const int SCREEN_DIM = SCLERA_RADIUS * 2; // full display height/width (matches sclera diameter)

const int IRIS_RADIUS = 94; // vertical extent of the iris sprite box (also its bounding radius for movement math)
const int PUPIL_RADIUS = 30; // Base/resting pupil "radius" - now really an animation driver, see PUPIL_SLIT_WIDTH_SCALE

// Cat-eye shaping: the iris is a full circle, but the pupil inside it is a
// vertical slit - widest at the vertical center, tapering to points top and
// bottom - whose WIDTH is what dilates/constricts, while its HEIGHT stays
// close to fixed. This reuses the exact same hippus/light-reflex dynamics
// computed below; only how that value gets turned into a shape has changed.
const int IRIS_RADIUS_X = IRIS_RADIUS;                // round iris - same as Y
const int IRIS_RADIUS_Y = IRIS_RADIUS;                // full extent top-to-bottom (94)
const int PUPIL_SLIT_HALF_HEIGHT = (IRIS_RADIUS_Y * 85) / 100; // slit's fixed long axis (79)
const float PUPIL_SLIT_WIDTH_SCALE = 0.30f;           // converts the old circular "radius" into a slit half-width

// With the enlarged iris, keeping it *fully* inside the round screen at all
// times leaves very little roaming room (SCLERA_RADIUS - IRIS_RADIUS is
// small). Trading strict containment for more movement: the iris is
// allowed to roam far enough that at extreme gaze positions it can extend
// up to IRIS_CLIP_ALLOWANCE px past the round screen's edge - it'll simply
// get cut off there (like a real eye partially occluded by the socket when
// looking hard to one side) rather than staying artificially centered.
const int IRIS_CLIP_ALLOWANCE = 20;
const int MOVE_LIMIT = SCLERA_RADIUS - IRIS_RADIUS + IRIS_CLIP_ALLOWANCE;
const int MIN_ALLOWED_COORD = CENTER_X - MOVE_LIMIT;
const int MAX_ALLOWED_COORD = CENTER_X + MOVE_LIMIT;

// Calculate bounding box size for the moving eye assembly
const int SPRITE_DIM = IRIS_RADIUS * 2;

// Create an in-RAM canvas (Sprite) exactly the size of the Iris
GFXcanvas16 canvas(SPRITE_DIM, SPRITE_DIM);

// The iris pattern's *shape* (fibrous streaks, limbal ring, collarette) is
// baked once at boot into this brightness map, since the per-pixel trig
// involved is too slow to redo often. 0 means "not iris" (leave as
// sclera); 1-255 is a scaled brightness. Color is applied separately at
// canvas-rebuild time by multiplying this brightness against whatever the
// current hue's base RGB is - that recolor step is cheap (no trig), so it
// can run every time the hue advances without bogging things down.
uint8_t irisBrightnessMap[SPRITE_DIM * SPRITE_DIM];

int currentX = CENTER_X;
int currentY = CENTER_Y;
int targetX = CENTER_X;
int targetY = CENTER_Y;

unsigned long lastMoveTime = 0;
unsigned long nextLookTime = 0;
const unsigned long MOVE_INTERVAL = 15; // Speed of the eye glide

// Tracks whether currentX/currentY have advanced since the last time we
// actually drew to the screen, and where the box last drawn to screen
// really is. The move-step timer (120ms) and render timer (30ms) aren't
// synchronized, so a move can happen on a loop() iteration where the
// render block doesn't run - this flag/position survive across iterations
// so no move is ever silently dropped (which was leaving stray lines).
bool moveDirty = false;
int lastDrawnLeft = CENTER_X - IRIS_RADIUS;
int lastDrawnTop = CENTER_Y - IRIS_RADIUS;

// --- Pupil dynamics, modeled after a real pupillary response ---
// Two layers stack together:
//  1. Hippus: a small, continuous, quasi-random flutter that real pupils
//     never stop doing, even with no lighting change. Smoothly drifts
//     toward a new tiny random target every ~1-2s instead of pulsing on
//     a clean sine, so it reads as organic rather than mechanical.
//  2. Light-reflex-style events: every PUPIL_CHECK_INTERVAL there's a
//     PUPIL_TRIGGER_CHANCE% chance of a constriction firing. Real pupils
//     constrict quickly (~150-400ms) and redilate back to rest much more
//     slowly (~1.2-3s) - that asymmetry is the key "human" cue a
//     symmetric sine wave is missing.
unsigned long lastRenderTime = 0;
const unsigned long RENDER_INTERVAL = 30; // ~33fps, smooth without flooding SPI

// Hippus (continuous micro-flutter)
// +/-8.4% of base radius (was +/-6% - variability bumped up 40%)
const float HIPPUS_AMPLITUDE = 0.084;
float hippusCurrent = 0.0f;          // current offset, as a fraction of base radius
float hippusTarget = 0.0f;
unsigned long nextHippusRetarget = 0;

// Light-reflex-style constrict/dilate event
const unsigned long PUPIL_CHECK_INTERVAL = 20000; // how often we roll the dice
const int PUPIL_TRIGGER_CHANCE = 30;              // % chance an event fires on each check
const float PUPIL_MAX_DEPTH = 0.42;               // deepest possible constriction: -42% (was -30%, up 40%)
unsigned long nextPupilCheckTime = 0;
bool pupilEventActive = false;
bool pupilConstricting = false; // true = fast constrict phase, false = slow dilate phase
unsigned long pupilPhaseStart = 0;
unsigned long pupilConstrictDuration = 0;
unsigned long pupilDilateDuration = 0;
float pupilEventDepth = 0.0f; // this event's depth, as a fraction of base radius

int lastDrawnPupilRadius = -1; // forces first canvas build in setup()
int lastDrawnHueDeg = -1;      // forces first canvas build in setup()

// --- Blink ---
// A single eyelid (BG_COLOR band) sweeps down from the top of the screen
// and back up, matching the shape of a real blink: fast, decelerating
// close; a brief full-closed hold; then a slower, smoother reopen. Real
// blinks also occasionally come in quick pairs, which is modeled with
// blinkDoublePending. Eye movement and pupil dynamics are paused for the
// duration since real eyes don't saccade/react mid-blink.
bool blinking = false;
int blinkPhase = 0; // 0 = closing, 1 = closed (hold), 2 = opening
unsigned long blinkPhaseStart = 0;
unsigned long blinkCloseDuration = 0;
unsigned long blinkHoldDuration = 0;
unsigned long blinkOpenDuration = 0;
unsigned long nextBlinkTime = 0;
bool blinkDoublePending = false;
int lidY = 0; // rows currently covered by the eyelid, from the top: 0 = open, SCREEN_DIM = closed

// Smoothstep-style ease: fast at one end, gentle landing at the other.
float easeOutQuad(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
float easeInOutSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Procedurally derives the iris's *shape* into irisBrightnessMap: fine
// radial streaks (like collagen fibers), a darker limbal ring at the outer
// edge, and a subtle darker collarette ring near the pupil - the visual
// cues that read as an "iris image" rather than a flat color fill. Runs
// once at boot since the per-pixel trig is too slow to repeat often; color
// is layered on separately (and cheaply) at recolor time.
//
// The boundary test is expressed in terms of IRIS_RADIUS_X/Y (currently
// equal, i.e. a plain circle) so the iris shape stays independently
// adjustable from the pupil's; streaks, limbal ring, and collarette are
// all driven off the same normalized 0..1 "distance to the edge" so they'd
// automatically follow suit if that boundary ever became elliptical again.
void generateIrisBrightnessMap() {
  const float spokes = 40.0f; // number of fine fiber streaks around the ring

  for (int y = 0; y < SPRITE_DIM; y++) {
    for (int x = 0; x < SPRITE_DIM; x++) {
      int idx = y * SPRITE_DIM + x;

      float dx = (float)x - IRIS_RADIUS;
      float dy = (float)y - IRIS_RADIUS;
      float ndx = dx / (float)IRIS_RADIUS_X;
      float ndy = dy / (float)IRIS_RADIUS_Y;
      float dist = sqrtf(ndx * ndx + ndy * ndy); // 0 at center, 1.0 at the ellipse edge

      if (dist > 1.0f) {
        irisBrightnessMap[idx] = 0; // not iris - stays sclera
        continue;
      }

      float angle = atan2f(ndy, ndx);  // pseudo-angle in normalized ellipse space
      float radialFrac = dist;         // already 0 (center) .. 1 (outer edge)

      // Two layers of radial fiber streaks at different frequencies.
      float streak = sinf(angle * spokes + radialFrac * 6.0f);
      float streak2 = sinf(angle * (spokes * 0.5f) - radialFrac * 3.0f);

      float brightness = 0.65f + 0.20f * streak + 0.15f * streak2;

      // Slightly darker collarette ring near the pupil border.
      if (radialFrac < 0.35f) {
        brightness *= 0.85f + 0.15f * (radialFrac / 0.35f);
      }

      // Smoothly fade the outer edge down to full black (rather than a
      // hard darkened "limbal ring" that still cuts off abruptly against
      // the sclera) so the iris blends seamlessly into the black
      // background with no visible boundary.
      const float FADE_START = 0.65f; // radial fraction where the fade begins
      if (radialFrac > FADE_START) {
        float ft = constrain((radialFrac - FADE_START) / (1.0f - FADE_START), 0.0f, 1.0f);
        float fade = 1.0f - easeInOutSmooth(ft); // 1.0 at FADE_START -> 0.0 at the edge
        brightness *= fade;
      }

      brightness = constrain(brightness, 0.0f, 1.05f);

      // Scale into 1..255 (0 is reserved to mean "not iris").
      int stored = (int)(brightness * 200.0f + 0.5f);
      irisBrightnessMap[idx] = (uint8_t)constrain(stored, 1, 255);
    }
  }
}

// Full-saturation, full-value HSV -> RGB, i.e. a pure spectrum color for a
// given hue angle (0-360 degrees).
void hueToRGB(float hueDeg, uint8_t &r, uint8_t &g, uint8_t &b) {
  float h = hueDeg / 60.0f;
  int hi = ((int)h) % 6;
  float f = h - (int)h;
  uint8_t q = (uint8_t)(255 * (1.0f - f));
  uint8_t t = (uint8_t)(255 * f);
  switch (hi) {
    case 0: r = 255; g = t;   b = 0;   break;
    case 1: r = q;   g = 255; b = 0;   break;
    case 2: r = 0;   g = 255; b = t;   break;
    case 3: r = 0;   g = q;   b = 255; break;
    case 4: r = t;   g = 0;   b = 255; break;
    default: r = 255; g = 0;  b = q;   break; // case 5
  }
}

// Draws the pupil as a vertical cat-eye slit centered in the canvas: an
// ellipse-profile shape that's widest at its vertical midpoint and tapers
// to a point at top and bottom, with a fixed-ish half-height and an
// animated half-width (narrow at rest/bright, widening toward round in
// the "dilated" state - exactly like a real cat pupil).
void fillPupilSlit(int halfWidth) {
  if (halfWidth < 1) halfWidth = 1;

  for (int dy = -PUPIL_SLIT_HALF_HEIGHT; dy <= PUPIL_SLIT_HALF_HEIGHT; dy++) {
    float t = (float)dy / (float)PUPIL_SLIT_HALF_HEIGHT;
    float widthFrac = sqrtf(constrain(1.0f - t * t, 0.0f, 1.0f)); // ellipse cross-section
    int w = (int)(halfWidth * widthFrac + 0.5f);
    if (w < 1) continue; // taper to a point at the very top/bottom - leave those rows untouched

    int y = IRIS_RADIUS + dy;
    canvas.drawFastHLine(IRIS_RADIUS - w, y, w * 2 + 1, PUPIL);
  }
}

void rebuildCanvas(int pupilRadius, uint8_t baseR, uint8_t baseG, uint8_t baseB) {
  uint16_t *buf = canvas.getBuffer();
  for (int i = 0; i < SPRITE_DIM * SPRITE_DIM; i++) {
    uint8_t raw = irisBrightnessMap[i];
    if (raw == 0) {
      buf[i] = SCLERA;
    } else {
      float brightness = raw / 200.0f;
      int r = constrain((int)(baseR * brightness), 0, 255);
      int g = constrain((int)(baseG * brightness), 0, 255);
      int b = constrain((int)(baseB * brightness), 0, 255);
      buf[i] = tft.color565(r, g, b);
    }
  }
  int slitHalfWidth = (int)(pupilRadius * PUPIL_SLIT_WIDTH_SCALE + 0.5f);
  fillPupilSlit(slitHalfWidth);
}

// Redraws exactly one horizontal row of the true eye image (sclera chord,
// plus the iris/pupil sprite row where it overlaps) at screen row y. Used
// to reveal blink-lid rows one at a time as the lid retracts, so rows
// still meant to stay hidden are never touched - and therefore never
// flash - which is what full-circle repaints were doing before.
void revealRow(int y) {
  int dy = y - CENTER_Y;
  long halfChordSq = (long)SCLERA_RADIUS * SCLERA_RADIUS - (long)dy * dy;
  if (halfChordSq < 0) return; // outside the circle - already background

  int halfChord = (int)sqrtf((float)halfChordSq);
  int leftX = CENTER_X - halfChord;
  int width = halfChord * 2 + 1;
  tft.drawFastHLine(leftX, y, width, SCLERA);

  int spriteRow = y - lastDrawnTop;
  if (spriteRow >= 0 && spriteRow < SPRITE_DIM) {
    uint16_t *rowPixels = canvas.getBuffer() + ((size_t)spriteRow * SPRITE_DIM);
    tft.drawRGBBitmap(lastDrawnLeft, y, rowPixels, SPRITE_DIM, 1);
  }
}

void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(BG_COLOR);

  // Paint the permanent black eyeball background once
  tft.fillCircle(CENTER_X, CENTER_Y, SCLERA_RADIUS, SCLERA);

  randomSeed(analogRead(A0));

  // First dice-roll for a pupil event happens after one full check interval
  nextPupilCheckTime = millis() + PUPIL_CHECK_INTERVAL;
  nextHippusRetarget = millis() + random(800, 2000);
  nextBlinkTime = millis() + random(2000, 6000);

  // Bake the iris shape once (per-pixel trig is too slow to repeat every
  // frame), then build the first working canvas at hue 0 (red).
  generateIrisBrightnessMap();
  uint8_t r0, g0, b0;
  hueToRGB(0.0f, r0, g0, b0);
  rebuildCanvas(PUPIL_RADIUS, r0, g0, b0);
  lastDrawnPupilRadius = PUPIL_RADIUS;
  lastDrawnHueDeg = 0;

  // Push the initial center eye graphic from RAM to the physical screen
  tft.drawRGBBitmap(currentX - IRIS_RADIUS, currentY - IRIS_RADIUS, canvas.getBuffer(), SPRITE_DIM, SPRITE_DIM);
}

void loop() {
  unsigned long currentTime = millis();

  // 1. Calculate a random look target
  if (currentTime >= nextLookTime) {
    int offsetX = random(-MOVE_LIMIT, MOVE_LIMIT + 1);
    int offsetY = random(-MOVE_LIMIT, MOVE_LIMIT + 1);

    // Pythagorean circle constraint check: (x² + y² <= r²)
    if (((long)offsetX * offsetX) + ((long)offsetY * offsetY) <= ((long)MOVE_LIMIT * MOVE_LIMIT)) {
      // Calculate targets and instantly clamp them to a hard boundary safety zone
      targetX = constrain(CENTER_X + offsetX, MIN_ALLOWED_COORD, MAX_ALLOWED_COORD);
      targetY = constrain(CENTER_Y + offsetY, MIN_ALLOWED_COORD, MAX_ALLOWED_COORD);

      nextLookTime = currentTime + random(500, 2500);
    }
  }

  // 2. Advance the eye position glide (only steps every MOVE_INTERVAL).
  // Paused while blinking - real eyes don't saccade mid-blink. Just updates
  // currentX/currentY and flags moveDirty; the render tick (step 4) is
  // solely responsible for actually erasing/redrawing, however many
  // loop() iterations later that ends up being.
  if (!blinking && currentTime - lastMoveTime >= MOVE_INTERVAL) {
    lastMoveTime = currentTime;

    if (currentX != targetX || currentY != targetY) {
      // CORRECTED TRACKING MATH: Increments smoothly towards target destinations
      if (currentX < targetX) currentX++;
      else if (currentX > targetX) currentX--;

      if (currentY < targetY) currentY++;
      else if (currentY > targetY) currentY--;

      // Enforce absolute safety constraint on current rendering trackers
      currentX = constrain(currentX, MIN_ALLOWED_COORD, MAX_ALLOWED_COORD);
      currentY = constrain(currentY, MIN_ALLOWED_COORD, MAX_ALLOWED_COORD);

      moveDirty = true;
    }
  }

  // 3. Every PUPIL_CHECK_INTERVAL, roll the dice on whether a constriction
  // event fires (skip if one is already in progress).
  if (currentTime >= nextPupilCheckTime) {
    nextPupilCheckTime = currentTime + PUPIL_CHECK_INTERVAL;

    if (!pupilEventActive && (long)random(100) < PUPIL_TRIGGER_CHANCE) {
      pupilEventActive = true;
      pupilConstricting = true;
      pupilPhaseStart = currentTime;
      pupilConstrictDuration = random(150, 400);   // fast constriction
      pupilDilateDuration = random(1200, 3000);    // slower redilation back to rest
      pupilEventDepth = PUPIL_MAX_DEPTH * (0.5f + (random(0, 101) / 100.0f) * 0.5f); // 15-30%
    }
  }

  // 3b. Roll for a blink starting (skip if one is already in progress).
  if (!blinking && currentTime >= nextBlinkTime) {
    blinking = true;
    blinkPhase = 0; // closing
    blinkPhaseStart = currentTime;
    blinkCloseDuration = random(90, 150);  // fast, real-blink-speed close
    blinkHoldDuration = random(20, 60);    // brief fully-shut hold
    blinkOpenDuration = random(150, 260);  // slower reopen

    // Make sure lastDrawnLeft/Top exactly matches the frozen gaze position
    // before the lid animation starts relying on it for row reveals.
    lastDrawnLeft = currentX - IRIS_RADIUS;
    lastDrawnTop = currentY - IRIS_RADIUS;
    moveDirty = false;
  }

  // 4. Render tick: drives the blink (if any), otherwise hippus + any
  // active constriction event, and the on-screen redraw. Runs faster than
  // the movement step so everything reads as smooth even while the eye is
  // holding still.
  if (currentTime - lastRenderTime >= RENDER_INTERVAL) {
    lastRenderTime = currentTime;

    if (blinking) {
      unsigned long elapsed = currentTime - blinkPhaseStart;

      if (blinkPhase == 0) { // closing: sweep the lid down, incrementally
        int newLidY;
        if (elapsed >= blinkCloseDuration) {
          newLidY = SCREEN_DIM;
          blinkPhase = 1;
          blinkPhaseStart = currentTime;
        } else {
          float t = easeOutQuad((float)elapsed / (float)blinkCloseDuration);
          newLidY = (int)(SCREEN_DIM * t + 0.5f);
        }
        if (newLidY > lidY) {
          tft.fillRect(0, lidY, SCREEN_DIM, newLidY - lidY, BG_COLOR);
          lidY = newLidY;
        }
      } else if (blinkPhase == 1) { // fully closed, brief hold
        if (elapsed >= blinkHoldDuration) {
          blinkPhase = 2;
          blinkPhaseStart = currentTime;
        }
      } else { // opening
        if (elapsed >= blinkOpenDuration) {
          // Blink finished: reveal any rows still covered, one at a time,
          // then resync for normal rendering to resume cleanly.
          if (lidY > 0) {
            for (int y = 0; y < lidY; y++) revealRow(y);
          }
          blinking = false;
          lidY = 0;

          // Real blinks occasionally come in quick pairs.
          if (blinkDoublePending) {
            blinkDoublePending = false;
            nextBlinkTime = currentTime + random(150, 350);
          } else {
            blinkDoublePending = (random(100) < 12); // ~12% chance of a follow-up blink
            nextBlinkTime = currentTime + random(2000, 6000);
          }
        } else {
          float t = (float)elapsed / (float)blinkOpenDuration;
          float openedFrac = easeInOutSmooth(t);
          int newLidY = (int)(SCREEN_DIM * (1.0f - openedFrac) + 0.5f);

          if (newLidY < lidY) {
            // Only reveal the newly-exposed band [newLidY, lidY) with its
            // true content - rows still above newLidY are left completely
            // untouched (still solid black), so there's no flash of the
            // eye becoming briefly visible before being re-covered.
            for (int y = newLidY; y < lidY; y++) revealRow(y);
            lidY = newLidY;
          }
        }
      }
    } else {
      // --- Hippus: continuous small drift toward a new random micro-target ---
      if (currentTime >= nextHippusRetarget) {
        hippusTarget = ((random(0, 201) / 100.0f) - 1.0f) * HIPPUS_AMPLITUDE; // -amp..+amp
        nextHippusRetarget = currentTime + random(800, 2000);
      }
      hippusCurrent += (hippusTarget - hippusCurrent) * 0.06f; // smooth exponential drift

      // --- Constriction/redilation event ---
      float eventOffset = 0.0f;
      if (pupilEventActive) {
        unsigned long elapsed = currentTime - pupilPhaseStart;

        if (pupilConstricting) {
          if (elapsed >= pupilConstrictDuration) {
            // Constriction finished; hand off to the slow redilation phase.
            pupilConstricting = false;
            pupilPhaseStart = currentTime;
            eventOffset = -pupilEventDepth;
          } else {
            float t = easeOutQuad((float)elapsed / (float)pupilConstrictDuration);
            eventOffset = -pupilEventDepth * t;
          }
        } else {
          if (elapsed >= pupilDilateDuration) {
            // Fully back to rest; event over.
            pupilEventActive = false;
            eventOffset = 0.0f;
          } else {
            float t = easeInOutSmooth((float)elapsed / (float)pupilDilateDuration);
            eventOffset = -pupilEventDepth * (1.0f - t);
          }
        }
      }

      float totalScale = 1.0f + hippusCurrent + eventOffset;
      int pupilRadius = (int)(PUPIL_RADIUS * totalScale + 0.5f);
      // Clamp widened to match: deviation from the 1.0x resting point scaled
      // up 40% on both ends (was 0.5x-1.3x) so the larger hippus/event
      // swings above aren't clipped back down.
      pupilRadius = constrain(pupilRadius, (int)(PUPIL_RADIUS * 0.3f), (int)(PUPIL_RADIUS * 1.42f));

      bool pupilChanged = (pupilRadius != lastDrawnPupilRadius);

      // --- Continuous full-spectrum hue rotation, on a fixed period tied
      // directly to millis() so it never drifts. Only compared at whole-
      // degree resolution so the (cheap, but not free) canvas recolor only
      // runs when there's an actual visible change - naturally throttles
      // to roughly every 80ms at a 30s cycle.
      float hueDeg = fmodf((float)(currentTime % IRIS_HUE_CYCLE_MS), (float)IRIS_HUE_CYCLE_MS)
                     / (float)IRIS_HUE_CYCLE_MS * 360.0f;
      int hueDegInt = (int)hueDeg;
      bool hueChanged = (hueDegInt != lastDrawnHueDeg);

      bool canvasDirty = pupilChanged || hueChanged;

      if (moveDirty || canvasDirty) {
        if (canvasDirty) {
          uint8_t baseR, baseG, baseB;
          hueToRGB(hueDeg, baseR, baseG, baseB);
          rebuildCanvas(pupilRadius, baseR, baseG, baseB);
          lastDrawnPupilRadius = pupilRadius;
          lastDrawnHueDeg = hueDegInt;
        }

        int newLeft = currentX - IRIS_RADIUS;
        int newTop = currentY - IRIS_RADIUS;

        if (moveDirty) {
          // Erase relative to lastDrawnLeft/Top - the box actually still on
          // screen - not a locally re-derived "old" position. That's what
          // guarantees every pixel exposed since the last real draw gets
          // cleared, however many (or few) loop() iterations it took to
          // get here.
          int dx = newLeft - lastDrawnLeft;
          int dy = newTop - lastDrawnTop;

          // Only erase the sliver of the OLD box that the NEW box won't
          // immediately cover, instead of wiping the whole sprite to white
          // every tick (that full-box wipe is what caused the original flicker).
          if (dx > 0) {
            tft.fillRect(lastDrawnLeft, lastDrawnTop, dx, SPRITE_DIM, SCLERA);
          } else if (dx < 0) {
            tft.fillRect(lastDrawnLeft + SPRITE_DIM + dx, lastDrawnTop, -dx, SPRITE_DIM, SCLERA);
          }
          if (dy > 0) {
            tft.fillRect(lastDrawnLeft, lastDrawnTop, SPRITE_DIM, dy, SCLERA);
          } else if (dy < 0) {
            tft.fillRect(lastDrawnLeft, lastDrawnTop + SPRITE_DIM + dy, SPRITE_DIM, -dy, SCLERA);
          }

          moveDirty = false;
        }

        // Instantly print the pre-baked round eye graphics from the RAM canvas.
        // When only the pupil changed (no move), this single call overwrites
        // the whole box in place with no separate erase needed.
        tft.drawRGBBitmap(newLeft, newTop, canvas.getBuffer(), SPRITE_DIM, SPRITE_DIM);

        lastDrawnLeft = newLeft;
        lastDrawnTop = newTop;
      }
    }
  }
}