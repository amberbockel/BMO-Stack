// IdleLab -- Phase 8 of the StackChan BMO project.
//
// Carries forward MoodLab v6 verbatim (Mood + sprite-buffered face + auto-blink)
// and adds the idle behavior loop on top. When no touch interaction has happened
// for a mood-modulated quiet window (15-60 s), a weighted-random idle behavior
// fires. The weight table is biased by mood so the robot picks something that
// fits how it's currently feeling.
//
// Touch still cycles through the six mood-nudge events from MoodLab so we can
// push the mood around and watch the idle pool's behavior shift accordingly.
// Every touch also resets the quiet timer.

#include <Arduino.h>
#include <M5StackChan.h>
#include <M5Unified.h>
#include <math.h>

// === Hoisted types (Arduino auto-prototype workaround) ===
// Any type referenced in a function signature must be defined here, not
// next to the section that uses it, because Arduino auto-inserts forward
// declarations for all functions at the top of the file.
enum class MouthShape { SMILE, NEUTRAL, FROWN, OPEN, GRIN };
enum class EyeState   { BLINK, SLEEPY, NORMAL, WIDE, CONTENT, ASLEEP };
struct IdleBehavior {
  const char* name;
  void (*fn)();
  float base_weight;
  // Weight bias by mood -- determines how often this behavior is chosen
  float v_bias, a_bias, e_bias;
  // Mood NUDGE when fired -- drives the COLOR change (color reads from mood)
  float v_nudge, a_nudge, e_nudge;
  // Face OVERRIDE during behavior -- guarantees a visual signature regardless
  // of starting mood. If has_override is false, face stays mood-driven.
  bool has_override;
  EyeState override_eye;
  MouthShape override_mouth;
};

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

  void tick(float dt) {
    valence = approach(valence, V_NEUTRAL, V_RATE, dt);
    arousal = approach(arousal, A_NEUTRAL, A_RATE, dt);
    energy  = approach(energy,  E_NEUTRAL, E_RATE, dt);
  }
  static float approach(float v, float t, float r, float dt) {
    return v + (t - v) * (1.0f - expf(-r * dt));
  }
  static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  static float clamp11(float v) { return v < -1 ? -1 : (v > 1 ? 1 : v); }
};

Mood mood;

// === Mood-nudge events (touch cycles through these) ===

struct Event { const char* name; void (*apply)(Mood&); };

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
  {"happy",     event_happy},     {"sad",       event_sad},
  {"curious",   event_curious},   {"surprised", event_surprised},
  {"tired",     event_tired},     {"cheer up",  event_cheer_up},
};
const int event_count = sizeof(events) / sizeof(events[0]);
int next_event_index = 0;

// === Color mapping (same as MoodLab v6) ===

uint16_t mood_to_color_with_brightness(const Mood& m, float bm) {
  float r = 55, g = 200, b = 170;
  if (m.valence > 0) {
    r += m.valence * 150; g += m.valence * 30; b -= m.valence * 80;
  } else {
    r -= (-m.valence) * 30; g -= (-m.valence) * 130; b += (-m.valence) * 50;
  }
  float br = (0.4f + m.energy * 0.6f) * bm;
  r *= br; g *= br; b *= br;
  int ri = (int)(r < 0 ? 0 : (r > 255 ? 255 : r));
  int gi = (int)(g < 0 ? 0 : (g > 255 ? 255 : g));
  int bi = (int)(b < 0 ? 0 : (b > 255 ? 255 : b));
  return M5StackChan.Display().color565(ri, gi, bi);
}
uint16_t mood_to_color(const Mood& m) { return mood_to_color_with_brightness(m, 1.0f); }
uint16_t darker_mood_color() { return mood_to_color_with_brightness(mood, 0.6f); }

// === Face rendering (same as MoodLab v6) ===

LGFX_Sprite face_buffer;
const uint16_t FEATURE_COLOR = TFT_BLACK;

EyeState compute_eye_state(const Mood& m, bool blinking) {
  if (blinking) return EyeState::BLINK;
  if (m.energy < 0.15f) return EyeState::ASLEEP;                       // nodding off
  if (m.energy < 0.30f) return EyeState::SLEEPY;                       // droopy
  if (m.valence > 0.50f && m.arousal < 0.50f) return EyeState::CONTENT; // calm-happy
  if (m.arousal > 0.70f) return EyeState::WIDE;                        // alert / surprised
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
    case EyeState::CONTENT:
      // Closed happy eye -- "^" shape, peak up in middle, like ⌃
      for (int dy = 0; dy < 3; dy++) {
        face_buffer.drawLine(cx - 8, cy + dy,     cx - 3, cy - 4 + dy, FEATURE_COLOR);
        face_buffer.drawLine(cx - 3, cy - 4 + dy, cx + 3, cy - 4 + dy, FEATURE_COLOR);
        face_buffer.drawLine(cx + 3, cy - 4 + dy, cx + 8, cy + dy,     FEATURE_COLOR);
      }
      break;
    case EyeState::ASLEEP:
      // Closed sleepy eye -- "u" shape, corners up, middle down, like ⌣
      for (int dy = 0; dy < 3; dy++) {
        face_buffer.drawLine(cx - 8, cy - 3 + dy, cx - 3, cy + 1 + dy, FEATURE_COLOR);
        face_buffer.drawLine(cx - 3, cy + 1 + dy, cx + 3, cy + 1 + dy, FEATURE_COLOR);
        face_buffer.drawLine(cx + 3, cy + 1 + dy, cx + 8, cy - 3 + dy, FEATURE_COLOR);
      }
      break;
  }
}

MouthShape compute_mouth(const Mood& m) {
  if (m.arousal > 0.85f) return MouthShape::OPEN;
  if (m.valence > 0.60f) return MouthShape::GRIN;     // delighted
  if (m.valence > 0.20f) return MouthShape::SMILE;
  if (m.valence < -0.20f) return MouthShape::FROWN;
  return MouthShape::NEUTRAL;
}

void draw_smile(int cx, int cy) {
  face_buffer.fillEllipse(cx, cy + 4, 55, 22, FEATURE_COLOR);
  face_buffer.fillRect(cx - 60, cy - 22, 120, 22, mood_to_color(mood));
  face_buffer.fillRect(cx - 42, cy + 4, 84, 8, TFT_WHITE);
}

void draw_grin(int cx, int cy) {
  // Wider, deeper variant of the smile -- the "delighted" mouth.
  // Same construction as smile but bigger ellipse and thicker teeth band.
  face_buffer.fillEllipse(cx, cy + 6, 65, 28, FEATURE_COLOR);
  face_buffer.fillRect(cx - 70, cy - 22, 140, 22, mood_to_color(mood));
  face_buffer.fillRect(cx - 52, cy + 6, 104, 12, TFT_WHITE);
}
void draw_frown(int cx, int cy) {
  face_buffer.fillEllipse(cx, cy - 4, 55, 22, FEATURE_COLOR);
  face_buffer.fillRect(cx - 60, cy + 4, 120, 22, mood_to_color(mood));
}
void draw_neutral(int cx, int cy) {
  face_buffer.fillRect(cx - 30, cy - 2, 60, 4, FEATURE_COLOR);
}
void draw_open_mouth(int cx, int cy) {
  face_buffer.fillEllipse(cx, cy, 16, 9, FEATURE_COLOR);
  face_buffer.fillEllipse(cx, cy + 1, 13, 5, darker_mood_color());
}

// Forward declarations for the idle layer (defined further down)
extern const char* current_idle_label;
extern unsigned long idle_label_until;
extern unsigned long last_interaction_ms;
extern unsigned long last_render_ms;
// Face override state -- when an idle behavior fires with has_override,
// the face shows these specific values instead of mood-derived ones.
extern bool face_override_active;
extern EyeState face_override_eye;
extern MouthShape face_override_mouth;
extern unsigned long face_override_until;
float quiet_timeout_seconds();

void draw_face(bool blinking) {
  face_buffer.fillScreen(mood_to_color(mood));

  // Decide eye + mouth: face override during idle behavior, otherwise mood.
  unsigned long now_ms = millis();
  EyeState eye_s;
  MouthShape mouth_s;
  if (face_override_active && now_ms < face_override_until) {
    eye_s   = blinking ? EyeState::BLINK : face_override_eye;
    mouth_s = face_override_mouth;
  } else {
    if (face_override_active) face_override_active = false;
    eye_s   = compute_eye_state(mood, blinking);
    mouth_s = compute_mouth(mood);
  }

  draw_eye(80,  90, eye_s);
  draw_eye(240, 90, eye_s);

  int mx = 160, my = 130;
  switch (mouth_s) {
    case MouthShape::SMILE:   draw_smile(mx, my); break;
    case MouthShape::GRIN:    draw_grin(mx, my); break;
    case MouthShape::FROWN:   draw_frown(mx, my); break;
    case MouthShape::OPEN:    draw_open_mouth(mx, my); break;
    case MouthShape::NEUTRAL: draw_neutral(mx, my); break;
  }

  // Idle-label overlay -- transient, shows for ~2 s after each idle fire
  unsigned long now = millis();
  if (current_idle_label != nullptr && now < idle_label_until) {
    face_buffer.setTextSize(2);
    face_buffer.setTextColor(TFT_WHITE);
    face_buffer.setCursor(5, 5);
    face_buffer.printf("idle: %s", current_idle_label);
  }

  // Debug strip -- mood, quiet timer status, next mood event
  face_buffer.setTextSize(1);
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setCursor(5, 218);
  face_buffer.printf("V:%+0.2f A:%0.2f E:%0.2f", mood.valence, mood.arousal, mood.energy);
  face_buffer.setCursor(5, 228);
  float quiet_s = (millis() - last_interaction_ms) / 1000.0f;
  float timeout = quiet_timeout_seconds();
  face_buffer.printf("quiet:%4.1fs / %4.1fs   next:%s",
    quiet_s, timeout, events[next_event_index].name);

  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

// === Existing gestures, copied inline from GestureLab ===
// Reused by the idle pool. Copied rather than extracted because a single
// sketch is easier to iterate on; a real library lives in Phase 8 v2.

void excited_wiggle() {
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(   0, 1000); delay(220);
}
void sigh() {
  M5StackChan.Motion.moveY(500, 200); delay(700);
  M5StackChan.Motion.moveY(120, 150);
  M5.Speaker.tone(523, 200); delay(210);
  M5.Speaker.tone(440, 200); delay(210);
  M5.Speaker.tone(330, 300); delay(310);
  delay(300);
}
void happy_bounce() {
  M5.Speaker.tone(523,  55); delay(60);
  M5.Speaker.tone(659,  55); delay(60);
  M5.Speaker.tone(784,  55); delay(60);
  M5.Speaker.tone(1047,100); delay(105);
  M5StackChan.Motion.moveY(550, 900); delay(180);
  M5StackChan.Motion.moveY(300, 900); delay(180);
  M5StackChan.Motion.moveY(550, 900); delay(180);
  M5StackChan.Motion.moveY(350, 700); delay(280);
}
void curious_tilt() {
  M5StackChan.Motion.moveX(300, 300);
  M5StackChan.Motion.moveY(650, 300);
  M5.Speaker.tone(784, 90); delay(95);
  M5.Speaker.tone(1047, 120); delay(125);
  delay(1300);
}

// === New idle-only behaviors ===

// === Hum melodies (4 notes each, random pick) ===
// All in C major, all sing-song. Last note held longer than the others
// to give each phrase a sense of resolution rather than just trailing off.
const int HUM_COUNT = 5;
const int hum_freqs[HUM_COUNT][4] = {
  {523, 659, 587, 523},   // C-E-D-C   (cheerful, returns to tonic)
  {784, 659, 784, 1047},  // G-E-G-C   (skipping)
  {659, 784, 880, 784},   // E-G-A-G   (gentle peak)
  {523, 587, 659, 784},   // C-D-E-G   (rising)
  {880, 784, 659, 523},   // A-G-E-C   (descending, sigh-like)
};

void idle_hum() {
  int m = random(0, HUM_COUNT);
  for (int i = 0; i < 4; i++) {
    int dur = (i == 3) ? 200 : 100;
    M5.Speaker.tone(hum_freqs[m][i], dur);
    delay(dur + 10);
  }
}

void idle_look_around() {
  M5StackChan.Motion.moveX(-400, 300); delay(900);
  M5StackChan.Motion.moveX( 400, 300); delay(900);
  M5StackChan.Motion.goHome();         delay(500);
}

// === Babble patterns (5 tones each, random pick) ===
// Different emotional contours: question (rising), dismissive (falling),
// excited (high cluster), stuttery (repeating), original varied.
const int BABBLE_COUNT = 5;
const int babble_freqs[BABBLE_COUNT][5] = {
  {660,  523,  784,  659,  523},   // varied, original
  {440,  523,  659,  784,  988},   // rising question (ends up)
  {880,  784,  659,  523,  440},   // falling dismissive (ends down)
  {880,  988, 1047,  988,  880},   // excited cluster (high range)
  {523,  523,  587,  659,  523},   // stuttery / talking to self
};
const int babble_durs[BABBLE_COUNT][5] = {
  { 80,  60, 100,  80, 120},
  { 80,  80, 100, 100, 140},
  { 80, 100,  80,  80, 130},
  { 60,  60, 100,  60, 100},
  { 60,  60, 100, 100, 120},
};

void idle_talk_to_corner() {
  // Pick a side to turn to (left or right corner) and a babble pattern
  int side = (random(0, 2) == 0) ? -700 : 700;
  int p    = random(0, BABBLE_COUNT);
  M5StackChan.Motion.moveX(side, 500); delay(700);
  for (int i = 0; i < 5; i++) {
    M5.Speaker.tone(babble_freqs[p][i], babble_durs[p][i]);
    delay(babble_durs[p][i] + 10);
  }
  delay(300);
  M5StackChan.Motion.goHome(); delay(500);
}

// === Practice-speech phrases (3 notes, repeated twice with head bob) ===
const int SPEECH_COUNT = 4;
const int speech_freqs[SPEECH_COUNT][3] = {
  {440, 523, 587},  // rising 3
  {587, 659, 784},  // higher rising
  {523, 587, 523},  // return-to-tonic ("hello? hello?")
  {659, 587, 440},  // descending phrase
};

void idle_practice_speech() {
  int p = random(0, SPEECH_COUNT);
  M5StackChan.Motion.moveY(500, 400); delay(400);
  for (int i = 0; i < 3; i++) {
    M5.Speaker.tone(speech_freqs[p][i], 100); delay(110);
  }
  delay(150);
  M5StackChan.Motion.moveY(380, 400); delay(300);
  for (int i = 0; i < 3; i++) {
    M5.Speaker.tone(speech_freqs[p][i], 100); delay(110);
  }
  delay(200);
  M5StackChan.Motion.goHome(); delay(400);
}

extern unsigned long blink_until;
void idle_blink_solo() {
  blink_until = millis() + 250;
  delay(280);
}

void idle_stretch() {
  M5StackChan.Motion.moveY(800, 250); delay(1100);
  M5StackChan.Motion.goHome();         delay(500);
}

// === Idle behavior pool with mood-biased weights ===
// (IdleBehavior struct itself is hoisted up at the top of the file.)

const IdleBehavior idle_pool[] = {
  // name             fn                    base  v_b   a_b   e_b   v_n    a_n    e_n    ovr   eye                mouth
  {"hum_short",       idle_hum,             1.0f, 0.30f, 0.0f,  0.30f, 0.30f, 0.15f, 0.15f, true, EyeState::NORMAL,  MouthShape::SMILE},
  {"look_around",     idle_look_around,     1.5f, 0.0f,  0.50f, 0.0f,  0.05f, 0.40f, 0.0f,  true, EyeState::WIDE,    MouthShape::NEUTRAL},
  {"talk_to_corner",  idle_talk_to_corner,  0.7f, 0.0f, -0.30f, 0.0f,  0.10f,-0.20f, 0.0f,  false,EyeState::NORMAL,  MouthShape::NEUTRAL},
  {"practice_speech", idle_practice_speech, 0.5f, 0.0f, -0.40f, 0.0f,  0.10f,-0.15f, 0.0f,  false,EyeState::NORMAL,  MouthShape::NEUTRAL},
  {"idle_blink_solo", idle_blink_solo,      2.0f, 0.0f,  0.0f, -0.30f, 0.0f,  0.0f,  0.0f,  false,EyeState::NORMAL,  MouthShape::NEUTRAL},
  {"idle_stretch",    idle_stretch,         0.8f, 0.0f,  0.30f, 0.0f,  0.10f, 0.35f, 0.15f, true, EyeState::WIDE,    MouthShape::NEUTRAL},
  {"excited_wiggle",  excited_wiggle,       0.4f, 0.50f, 0.30f, 0.50f, 0.55f, 0.50f, 0.40f, true, EyeState::WIDE,    MouthShape::SMILE},
  {"sigh",            sigh,                 0.4f,-0.30f,-0.30f,-0.50f,-0.40f,-0.25f,-0.40f, true, EyeState::ASLEEP,  MouthShape::FROWN},
  {"happy_bounce",    happy_bounce,         0.4f, 0.60f, 0.30f, 0.40f, 0.70f, 0.40f, 0.30f, true, EyeState::WIDE,    MouthShape::GRIN},
  {"curious_tilt",    curious_tilt,         0.5f, 0.20f, 0.40f, 0.0f,  0.30f, 0.50f, 0.0f,  true, EyeState::WIDE,    MouthShape::SMILE},
};
const int idle_pool_size = sizeof(idle_pool) / sizeof(idle_pool[0]);

float compute_weight(const IdleBehavior& b) {
  float v = mood.valence;
  float a = mood.arousal - 0.5f;
  float e = mood.energy  - 0.5f;
  float modifier = 1.0f + b.v_bias * v + b.a_bias * a + b.e_bias * e;
  float w = b.base_weight * modifier;
  return w < 0.1f ? 0.1f : w;
}

const IdleBehavior* pick_idle_behavior() {
  float total = 0;
  for (int i = 0; i < idle_pool_size; i++) total += compute_weight(idle_pool[i]);
  float roll = (random(0, 10000) / 10000.0f) * total;
  for (int i = 0; i < idle_pool_size; i++) {
    roll -= compute_weight(idle_pool[i]);
    if (roll <= 0) return &idle_pool[i];
  }
  return &idle_pool[0];
}

float quiet_timeout_seconds() {
  // Cadence tuning, v4: base 10 s, clamps [5, 20] s. Brief said 15-60 s
  // but lifelike feel needs faster firings than that range allows.
  // Micro-fidget layer (see maybe_fire_micro) fills the gaps between
  // these full behaviors with tiny twitches every 2.5-5.5 s.
  float base = 10.0f;
  base *= (1.5f - mood.arousal);
  base *= (1.7f - mood.energy);
  if (base <  5.0f) base =  5.0f;
  if (base > 20.0f) base = 20.0f;
  return base;
}

// === Micro-fidget layer ===
// Fires very small movements every 2.5-5.5 s between full behaviors.
// ~60% chance of an actual twitch; 40% no-op so it doesn't feel
// mechanical/metronomic. Skipped during a behavior's override window
// so it doesn't interrupt the expression hold.

unsigned long next_micro_at = 0;

void maybe_fire_micro() {
  unsigned long now = millis();
  if (now < next_micro_at) return;
  // Don't fidget while a real behavior is showing its override face
  if (face_override_active && now < face_override_until + 800) {
    next_micro_at = now + 1500;
    return;
  }

  int kind = random(0, 5);  // 0..4
  switch (kind) {
    case 0: {  // tiny pan twitch
      int dir = (random(0, 2) == 0) ? -50 : 50;
      M5StackChan.Motion.moveX(dir, 400); delay(250);
      M5StackChan.Motion.moveX(0,   400); delay(180);
      break;
    }
    case 1: {  // tiny tilt down
      M5StackChan.Motion.moveY(380, 400); delay(250);
      M5StackChan.Motion.goHome();        delay(180);
      break;
    }
    case 2: {  // tiny tilt up
      M5StackChan.Motion.moveY(520, 400); delay(250);
      M5StackChan.Motion.goHome();        delay(180);
      break;
    }
    case 3:
    case 4:
      // No-op -- the silence keeps it from feeling mechanical
      break;
  }
  next_micro_at = now + 2500 + random(0, 3000);  // 2.5-5.5 s
}

void maybe_fire_idle() {
  unsigned long now = millis();
  float quiet_s = (now - last_interaction_ms) / 1000.0f;
  if (quiet_s >= quiet_timeout_seconds()) {
    const IdleBehavior* b = pick_idle_behavior();

    // 1. Big mood nudge -> color shifts noticeably (color reads from mood)
    mood.valence = Mood::clamp11(mood.valence + b->v_nudge);
    mood.arousal = Mood::clamp01(mood.arousal + b->a_nudge);
    mood.energy  = Mood::clamp01(mood.energy  + b->e_nudge);

    // 2. Face override -> face features get a guaranteed look matching
    //    this specific behavior, regardless of starting mood. Held for
    //    ~4s so the user clearly sees the expression during the behavior
    //    and for ~2s after.
    if (b->has_override) {
      face_override_active = true;
      face_override_eye    = b->override_eye;
      face_override_mouth  = b->override_mouth;
      face_override_until  = millis() + 4000;
    }

    current_idle_label = b->name;
    idle_label_until   = millis() + 2500;

    // 3. Force a face redraw NOW so the expression + color show BEFORE
    //    the (blocking) behavior starts moving the head.
    draw_face(false);
    last_render_ms = millis();

    b->fn();
    last_interaction_ms = millis();
  }
}

// === Setup and loop ===

const char* current_idle_label = nullptr;
unsigned long idle_label_until = 0;
unsigned long last_interaction_ms = 0;

// Face override state -- set by maybe_fire_idle when a behavior has has_override.
bool face_override_active = false;
EyeState face_override_eye = EyeState::NORMAL;
MouthShape face_override_mouth = MouthShape::NEUTRAL;
unsigned long face_override_until = 0;

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
  // Quiet boot sound
  M5.Speaker.tone(523, 60); delay(65);
  M5.Speaker.tone(784, 80); delay(85);

  face_buffer.setPsram(true);
  face_buffer.setColorDepth(16);
  face_buffer.createSprite(320, 240);

  randomSeed(esp_random());
  unsigned long now = millis();
  last_tick_ms        = now;
  last_interaction_ms = now;
  next_blink_at       = now + 2000;
  draw_face(false);
}

void loop() {
  M5StackChan.update();

  unsigned long now = millis();
  float dt = (now - last_tick_ms) / 1000.0f;
  mood.tick(dt);
  last_tick_ms = now;

  // Auto-blink scheduler
  if (now >= next_blink_at) {
    blink_until   = now + 150;
    next_blink_at = now + 3000 + random(0, 4000);
  }
  bool currently_blinking = (now < blink_until);
  bool blink_changed = (currently_blinking != was_blinking);

  // Touch = mood nudge + quiet-timer reset
  if (M5StackChan.TouchSensor.wasPressed()) {
    events[next_event_index].apply(mood);
    next_event_index = (next_event_index + 1) % event_count;
    M5.Speaker.tone(1047, 40);
    last_interaction_ms = now;
    draw_face(currently_blinking);
    last_render_ms = now;
    was_blinking = currently_blinking;
  }

  if (blink_changed || (now - last_render_ms > 200)) {
    draw_face(currently_blinking);
    last_render_ms = now;
    was_blinking = currently_blinking;
  }

  // Subtle "alive" layer between full behaviors, plus full-behavior firings.
  maybe_fire_micro();
  maybe_fire_idle();

  delay(20);
}
