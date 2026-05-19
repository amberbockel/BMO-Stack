#include <Arduino.h>
#include <M5StackChan.h>
#include <M5Unified.h>
#include <math.h>

// Hoisted up here so Arduino's auto-generated forward declarations for
// functions that return these types can see them. If these lived next
// to the face code below, the auto-prototypes above would fail.
enum class MouthShape { SMILE, NEUTRAL, FROWN, OPEN };
enum class EyeState   { BLINK, SLEEPY, NORMAL, WIDE };

// === Mood ===

struct Mood {
  float valence = 0.0f;
  float arousal = 0.3f;
  float energy  = 0.5f;

  static constexpr float V_NEUTRAL = 0.0f;
  static constexpr float A_NEUTRAL = 0.3f;
  static constexpr float E_NEUTRAL = 0.5f;

  static constexpr float V_RATE = 0.01155f;
  static constexpr float A_RATE = 0.02310f;
  static constexpr float E_RATE = 0.00578f;

  void tick(float dt_s) {
    valence = approach(valence, V_NEUTRAL, V_RATE, dt_s);
    arousal = approach(arousal, A_NEUTRAL, A_RATE, dt_s);
    energy  = approach(energy,  E_NEUTRAL, E_RATE, dt_s);
  }

  static float approach(float v, float target, float rate, float dt) {
    return v + (target - v) * (1.0f - expf(-rate * dt));
  }
  static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  static float clamp11(float v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }
};

Mood mood;

// === Events ===

struct Event { const char* name; void (*apply)(Mood&); };

// Lab-test event nudges. Asymmetric on purpose: sad/tired need to be
// strong enough to override a preceding happy/cheer in the tap cycle,
// otherwise consecutive opposite events just cancel and the lab never
// shows the negative-valence face states.
void event_happy(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.50f);
  m.arousal = Mood::clamp01(m.arousal + 0.30f);
  m.energy  = Mood::clamp01(m.energy  + 0.15f);
}
void event_sad(Mood& m) {
  m.valence = Mood::clamp11(m.valence - 0.75f);
  m.arousal = Mood::clamp01(m.arousal - 0.15f);
  m.energy  = Mood::clamp01(m.energy  - 0.20f);
}
void event_curious(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.15f);
  m.arousal = Mood::clamp01(m.arousal + 0.40f);
}
void event_surprised(Mood& m) {
  m.arousal = Mood::clamp01(m.arousal + 0.65f);
}
void event_tired(Mood& m) {
  m.energy  = Mood::clamp01(m.energy  - 0.50f);
  m.arousal = Mood::clamp01(m.arousal - 0.20f);
}
void event_cheer_up(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.85f);
  m.energy  = Mood::clamp01(m.energy  + 0.40f);
}

const Event events[] = {
  {"happy",     event_happy},
  {"sad",       event_sad},
  {"curious",   event_curious},
  {"surprised", event_surprised},
  {"tired",     event_tired},
  {"cheer up",  event_cheer_up},
};
const int event_count = sizeof(events) / sizeof(events[0]);
int next_event_index = 0;

// === Color mapping ===

uint16_t mood_to_color_with_brightness(const Mood& m, float brightness_mult) {
  float r = 55, g = 200, b = 170;  // BMO teal baseline

  if (m.valence > 0) {
    r += m.valence * 150;
    g += m.valence * 30;
    b -= m.valence * 80;
  } else {
    r -= (-m.valence) * 30;
    g -= (-m.valence) * 130;
    b += (-m.valence) * 50;
  }

  float brightness = (0.4f + m.energy * 0.6f) * brightness_mult;
  r *= brightness;
  g *= brightness;
  b *= brightness;

  int ri = (int)(r < 0 ? 0 : (r > 255 ? 255 : r));
  int gi = (int)(g < 0 ? 0 : (g > 255 ? 255 : g));
  int bi = (int)(b < 0 ? 0 : (b > 255 ? 255 : b));

  return M5StackChan.Display().color565(ri, gi, bi);
}

uint16_t mood_to_color(const Mood& m) {
  return mood_to_color_with_brightness(m, 1.0f);
}

// Same hue/saturation as the body, but ~60% as bright. Used as the
// "inside of the mouth" color for the open-mouth surprise shape, so the
// interior reads as "back of mouth showing through" rather than white.
uint16_t darker_mood_color() {
  return mood_to_color_with_brightness(mood, 0.6f);
}

// === Face ===

// Off-screen frame buffer. Lives in PSRAM so the 320*240*2 = 153 KB
// doesn't crowd the small on-chip RAM. Everything draws into here,
// then pushes to the LCD in one SPI transaction -- no visible flicker
// between fillScreen and the feature draws.
LGFX_Sprite face_buffer;

const uint16_t FEATURE_COLOR = TFT_BLACK;  // BMO features are dark on teal

// Eye drawing — BMO eyes are small dots, not rounded rectangles. The eye
// "state" varies less by mood than the mouth does; mood mostly lives in
// the mouth shape per BMO canon.

EyeState compute_eye_state(const Mood& m, bool blinking) {
  if (blinking) return EyeState::BLINK;
  if (m.energy < 0.3f) return EyeState::SLEEPY;
  if (m.arousal > 0.7f) return EyeState::WIDE;
  return EyeState::NORMAL;
}

void draw_eye(int cx, int cy, EyeState s) {
  switch (s) {
    case EyeState::BLINK:
      face_buffer.fillRect(cx - 9, cy - 2, 18, 4, FEATURE_COLOR);
      break;
    case EyeState::SLEEPY:
      face_buffer.fillEllipse(cx, cy + 2, 9, 4, FEATURE_COLOR);
      break;
    case EyeState::NORMAL:
      face_buffer.fillCircle(cx, cy, 8, FEATURE_COLOR);
      break;
    case EyeState::WIDE:
      face_buffer.fillCircle(cx, cy, 11, FEATURE_COLOR);
      break;
  }
}

MouthShape compute_mouth(const Mood& m) {
  if (m.arousal > 0.85f) return MouthShape::OPEN;
  if (m.valence > 0.20f) return MouthShape::SMILE;
  if (m.valence < -0.20f) return MouthShape::FROWN;
  return MouthShape::NEUTRAL;
}

// BMO classic open-smile shape: a dark crescent (lower lip / mouth outline)
// with a white "teeth" band visible across the upper interior. This is the
// signature feature in the reference images -- without the teeth band the
// face reads as generic happy emoji rather than BMO specifically.
void draw_smile(int cx, int cy) {
  // Layer 1: outer dark crescent.
  // Ellipse centered at (cx, cy+4), rx=55, ry=22. Covers y in [cy-18, cy+26].
  // We cover everything above cy with body color, leaving only the lower
  // crescent visible from cy down to cy+26.
  face_buffer.fillEllipse(cx, cy + 4, 55, 22, FEATURE_COLOR);
  face_buffer.fillRect(cx - 60, cy - 22, 120, 22, mood_to_color(mood));

  // Layer 2: white teeth band. Sits inside the dark crescent near its
  // top edge -- the part that reads as "open mouth showing teeth."
  // Geometry: at y=cy+4 the dark ellipse is at its widest (110 px); at
  // y=cy+12 it is still 102 px wide. The 84 px wide white rect sits
  // comfortably inside the dark contour at both top and bottom of the band.
  face_buffer.fillRect(cx - 42, cy + 4, 84, 8, TFT_WHITE);
}

void draw_frown(int cx, int cy) {
  // Plain inverted crescent. BMO's iconic sad faces use eye motion lines
  // and bow-tie mouths (per reference grid), not a teeth-frown -- adding
  // teeth to this shape would look wrong. Composed sad expressions live
  // in Phase 8 gestures, not in the base mouth primitives.
  face_buffer.fillEllipse(cx, cy - 4, 55, 22, FEATURE_COLOR);
  face_buffer.fillRect(cx - 60, cy + 4, 120, 22, mood_to_color(mood));
}

void draw_neutral(int cx, int cy) {
  face_buffer.fillRect(cx - 30, cy - 2, 60, 4, FEATURE_COLOR);
}

// BMO "mild surprise" mouth per the reference: small, contained, darker
// interior (not white teeth). Wider than tall, slight upper-lip emphasis.
// Total footprint ~32x18 -- roughly 1/10 of the face width.
void draw_open_mouth(int cx, int cy) {
  // Outer dark oval — the mouth outline.
  face_buffer.fillEllipse(cx, cy, 16, 9, FEATURE_COLOR);
  // Inner darker-body-color oval, offset 1 px down so the upper rim of
  // the dark outline reads as a slightly thicker upper lip.
  face_buffer.fillEllipse(cx, cy + 1, 13, 5, darker_mood_color());
}

void draw_face(bool blinking) {
  face_buffer.fillScreen(mood_to_color(mood));

  // Eyes: small dots, wide spacing (160 px apart), pulled DOWN from v4
  // (78 -> 90) per the reference. Eyes and mouth-top are now ~28 px apart.
  EyeState eye_s = compute_eye_state(mood, blinking);
  draw_eye(80,  90, eye_s);
  draw_eye(240, 90, eye_s);

  // Mouth: pulled UP from v4 (135 -> 130) so the mouth-eye gap closes.
  int mx = 160, my = 130;
  switch (compute_mouth(mood)) {
    case MouthShape::SMILE:   draw_smile(mx, my); break;
    case MouthShape::FROWN:   draw_frown(mx, my); break;
    case MouthShape::OPEN:    draw_open_mouth(mx, my); break;
    case MouthShape::NEUTRAL: draw_neutral(mx, my); break;
  }

  // Debug strip at the very bottom — hide later
  face_buffer.setTextSize(1);
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setCursor(5, 222);
  face_buffer.printf("V:%+0.2f A:%0.2f E:%0.2f  next:%s",
    mood.valence, mood.arousal, mood.energy, events[next_event_index].name);

  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

// === Loop state ===

unsigned long last_tick_ms   = 0;
unsigned long last_render_ms = 0;
unsigned long next_blink_at  = 0;
unsigned long blink_until    = 0;
bool was_blinking = false;

void setup() {
  M5StackChan.begin();
  M5.Speaker.setVolume(128);
  M5StackChan.Motion.goHome();
  delay(800);
  M5.Speaker.tone(523, 60); delay(65);
  M5.Speaker.tone(784, 80); delay(85);

  // Allocate the offscreen face buffer in PSRAM (153 KB).
  face_buffer.setPsram(true);
  face_buffer.setColorDepth(16);
  face_buffer.createSprite(320, 240);

  randomSeed(esp_random());
  unsigned long now = millis();
  last_tick_ms  = now;
  next_blink_at = now + 2000;
  draw_face(false);
}

void loop() {
  M5StackChan.update();

  unsigned long now = millis();
  float dt = (now - last_tick_ms) / 1000.0f;
  mood.tick(dt);
  last_tick_ms = now;

  // Auto-blink scheduler — fires every 3–7s
  if (now >= next_blink_at) {
    blink_until   = now + 150;
    next_blink_at = now + 3000 + random(0, 4000);
  }
  bool currently_blinking = (now < blink_until);
  bool blink_changed = (currently_blinking != was_blinking);

  if (M5StackChan.TouchSensor.wasPressed()) {
    events[next_event_index].apply(mood);
    next_event_index = (next_event_index + 1) % event_count;
    M5.Speaker.tone(1047, 40);
    draw_face(currently_blinking);
    last_render_ms = now;
    was_blinking = currently_blinking;
  }

  if (blink_changed || (now - last_render_ms > 200)) {
    draw_face(currently_blinking);
    last_render_ms = now;
    was_blinking = currently_blinking;
  }

  delay(20);
}
