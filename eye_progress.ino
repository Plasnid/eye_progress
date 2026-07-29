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
#define SCLERA   0xFFFF
#define PUPIL    0x0000

// Base color of the procedural iris texture (a natural iris blue), each
// channel gets scaled per-pixel by the generated brightness pattern below.
const uint8_t IRIS_BASE_R = 40;
const uint8_t IRIS_BASE_G = 95;
const uint8_t IRIS_BASE_B = 200;

const int CENTER_X = 120;
const int CENTER_Y = 120;
const int SCLERA_RADIUS = 120;

const int IRIS_RADIUS = 60;
const int PUPIL_RADIUS = 30; // Base/resting pupil radius

// STRICT HARD LIMITS: Prevents the sprite box from ever bleeding off the edge
const int MOVE_LIMIT = SCLERA_RADIUS - IRIS_RADIUS - 5;
const int MIN_ALLOWED_COORD = CENTER_X - MOVE_LIMIT;
const int MAX_ALLOWED_COORD = CENTER_X + MOVE_LIMIT;

// Calculate bounding box size for the moving eye assembly
const int SPRITE_DIM = IRIS_RADIUS * 2;

// Create an in-RAM canvas (Sprite) exactly the size of the Iris
GFXcanvas16 canvas(SPRITE_DIM, SPRITE_DIM);

// A second RAM buffer holds the procedurally-generated iris "image":
// sclera background + a fibrous radial iris pattern, with no pupil punched
// in. It's computed once in setup() (per-pixel trig is too slow to redo on
// every pupil-radius change) and then cheaply memcpy'd into canvas each
// time the pupil resizes, which just punches a fresh hole into the copy.
GFXcanvas16 irisTexture(SPRITE_DIM, SPRITE_DIM);

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
const float HIPPUS_AMPLITUDE = 0.06; // +/-6% of base radius
float hippusCurrent = 0.0f;          // current offset, as a fraction of base radius
float hippusTarget = 0.0f;
unsigned long nextHippusRetarget = 0;

// Light-reflex-style constrict/dilate event
const unsigned long PUPIL_CHECK_INTERVAL = 20000; // how often we roll the dice
const int PUPIL_TRIGGER_CHANCE = 30;              // % chance an event fires on each check
const float PUPIL_MAX_DEPTH = 0.30;               // deepest possible constriction: -30%
unsigned long nextPupilCheckTime = 0;
bool pupilEventActive = false;
bool pupilConstricting = false; // true = fast constrict phase, false = slow dilate phase
unsigned long pupilPhaseStart = 0;
unsigned long pupilConstrictDuration = 0;
unsigned long pupilDilateDuration = 0;
float pupilEventDepth = 0.0f; // this event's depth, as a fraction of base radius

int lastDrawnPupilRadius = -1; // forces first canvas build in setup()

// Smoothstep-style ease: fast at one end, gentle landing at the other.
float easeOutQuad(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
float easeInOutSmooth(float t) { return t * t * (3.0f - 2.0f * t); }

// Shades the iris base color by a brightness factor and packs it to RGB565.
uint16_t shadeOfIris(float brightness) {
  int r = constrain((int)(IRIS_BASE_R * brightness), 0, 255);
  int g = constrain((int)(IRIS_BASE_G * brightness), 0, 255);
  int b = constrain((int)(IRIS_BASE_B * brightness), 0, 255);
  return tft.color565(r, g, b);
}

// Procedurally paints a fibrous iris texture into irisTexture: fine radial
// streaks (like collagen fibers), a darker limbal ring at the outer edge,
// and a subtle darker collarette ring near the pupil - the visual cues
// that read as an "iris image" rather than a flat color fill. Runs once
// at boot since the per-pixel trig is too slow to repeat every frame.
void generateIrisTexture() {
  irisTexture.fillScreen(SCLERA);

  const float spokes = 40.0f; // number of fine fiber streaks around the ring

  for (int y = 0; y < SPRITE_DIM; y++) {
    for (int x = 0; x < SPRITE_DIM; x++) {
      float dx = (float)x - IRIS_RADIUS;
      float dy = (float)y - IRIS_RADIUS;
      float dist = sqrtf(dx * dx + dy * dy);

      if (dist > IRIS_RADIUS) continue; // leave sclera background as-is

      float angle = atan2f(dy, dx);            // -PI..PI
      float radialFrac = dist / (float)IRIS_RADIUS; // 0 (pupil edge) .. 1 (outer edge)

      // Two layers of radial fiber streaks at different frequencies.
      float streak = sinf(angle * spokes + radialFrac * 6.0f);
      float streak2 = sinf(angle * (spokes * 0.5f) - radialFrac * 3.0f);

      float brightness = 0.65f + 0.20f * streak + 0.15f * streak2;

      // Darker limbal ring where the iris meets the sclera.
      brightness *= 1.0f - 0.35f * powf(radialFrac, 6.0f);

      // Slightly darker collarette ring near the pupil border.
      if (radialFrac < 0.35f) {
        brightness *= 0.85f + 0.15f * (radialFrac / 0.35f);
      }

      brightness = constrain(brightness, 0.30f, 1.05f);

      irisTexture.drawPixel(x, y, shadeOfIris(brightness));
    }
  }
}

void rebuildCanvas(int pupilRadius) {
  // Start from the pre-baked iris texture, then punch the pupil hole.
  memcpy(canvas.getBuffer(), irisTexture.getBuffer(), (size_t)SPRITE_DIM * SPRITE_DIM * sizeof(uint16_t));
  canvas.fillCircle(IRIS_RADIUS, IRIS_RADIUS, pupilRadius, PUPIL);
}

void setup() {
  tft.begin();
  tft.setRotation(0);
  tft.fillScreen(BG_COLOR);

  // Paint the permanent white eyeball background once
  tft.fillCircle(CENTER_X, CENTER_Y, SCLERA_RADIUS, SCLERA);

  randomSeed(analogRead(A0));

  // First dice-roll for a pupil event happens after one full check interval
  nextPupilCheckTime = millis() + PUPIL_CHECK_INTERVAL;
  nextHippusRetarget = millis() + random(800, 2000);

  // Bake the procedural iris texture once (per-pixel trig is too slow to
  // repeat every frame), then build the first working canvas from it.
  generateIrisTexture();
  rebuildCanvas(PUPIL_RADIUS);
  lastDrawnPupilRadius = PUPIL_RADIUS;

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
  // Just updates currentX/currentY and flags moveDirty; the render tick
  // (step 4) is solely responsible for actually erasing/redrawing, however
  // many loop() iterations later that ends up being.
  if (currentTime - lastMoveTime >= MOVE_INTERVAL) {
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

  // 4. Render tick: drives hippus + any active constriction event, and the
  // on-screen redraw. Runs faster than the movement step so everything
  // reads as smooth even while the eye is holding still.
  if (currentTime - lastRenderTime >= RENDER_INTERVAL) {
    lastRenderTime = currentTime;

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
    pupilRadius = constrain(pupilRadius, (int)(PUPIL_RADIUS * 0.5f), (int)(PUPIL_RADIUS * 1.3f));

    bool pupilChanged = (pupilRadius != lastDrawnPupilRadius);

    if (moveDirty || pupilChanged) {
      if (pupilChanged) {
        rebuildCanvas(pupilRadius);
        lastDrawnPupilRadius = pupilRadius;
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
