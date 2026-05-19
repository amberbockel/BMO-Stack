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
// Phase 9b: Wi-Fi provisioning
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

// === Hoisted types (Arduino auto-prototype workaround) ===
// Any type referenced in a function signature must be defined here, not
// next to the section that uses it, because Arduino auto-inserts forward
// declarations for all functions at the top of the file.
enum class MouthShape { SMILE, NEUTRAL, FROWN, OPEN, GRIN };
enum class EyeState   { BLINK, SLEEPY, NORMAL, WIDE, CONTENT, ASLEEP, SWIRL, HEART };
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
    case EyeState::SWIRL: {
      // Dizzy/spinning eyes -- bigger, off-center concentric pattern.
      face_buffer.drawCircle(cx, cy, 14, FEATURE_COLOR);
      face_buffer.drawCircle(cx, cy, 13, FEATURE_COLOR);  // double-thick outer
      face_buffer.drawCircle(cx + 3, cy + 2, 8, FEATURE_COLOR);  // off-center
      face_buffer.drawCircle(cx + 3, cy + 2, 7, FEATURE_COLOR);  // double inner
      face_buffer.fillCircle(cx + 5, cy + 3, 3, FEATURE_COLOR);  // center dot
      break;
    }
    case EyeState::HEART: {
      // Love-struck pet eyes -- deep red heart, 50% bigger than v2.
      uint16_t red = face_buffer.color565(200, 30, 50);
      face_buffer.fillCircle(cx - 6, cy - 3, 7, red);
      face_buffer.fillCircle(cx + 6, cy - 3, 7, red);
      face_buffer.fillTriangle(
        cx - 12, cy,
        cx + 12, cy,
        cx,      cy + 13,
        red);
      break;
    }
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
// Phase 9a state
extern bool sleeping;
extern const char* pwr_debug_label;
extern unsigned long pwr_debug_until;
float quiet_timeout_seconds();
void dizzy_gesture();

extern bool show_pink_cheeks;
extern unsigned long pink_cheeks_until;

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

  // Pink cheeks during dizzy. 50% bigger than v2 for more drama.
  if (show_pink_cheeks && now_ms < pink_cheeks_until) {
    uint16_t pink = face_buffer.color565(255, 130, 160);
    face_buffer.fillCircle(40,  135, 20, pink);
    face_buffer.fillCircle(280, 135, 20, pink);
  } else if (show_pink_cheeks) {
    show_pink_cheeks = false;
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

  // Power-button debug indicator -- only visible when the button fires.
  if (pwr_debug_label != nullptr && now < pwr_debug_until) {
    face_buffer.setTextSize(2);
    face_buffer.setTextColor(TFT_YELLOW);
    face_buffer.setCursor(220, 5);
    face_buffer.print(pwr_debug_label);
  }

  // Tiny "wifi" indicator in the top-left when connected
  draw_wifi_status_indicator();

  // Debug strip -- mood + idle timer status (hide later when "shipping")
  face_buffer.setTextSize(1);
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setCursor(5, 218);
  face_buffer.printf("V:%+0.2f A:%0.2f E:%0.2f", mood.valence, mood.arousal, mood.energy);
  face_buffer.setCursor(5, 228);
  float quiet_s = (millis() - last_interaction_ms) / 1000.0f;
  float timeout = quiet_timeout_seconds();
  face_buffer.printf("quiet:%4.1fs/%4.1fs  pet/swipe/shake/pwr",
    quiet_s, timeout);

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

// === Phase 9a: LED breathing pulse at mood color ===
// 12 RGB LEDs in two rows. Always-on subtle breathing at the current mood
// color, brightness modulated by a sine wave over a 3 s period. Adds the
// "soft glow" layer of liveness independent of head motion.

unsigned long last_led_update_ms = 0;
unsigned long led_phase_start_ms = 0;

// LED override -- behaviors (pet, dizzy, future voice commands) can
// take over the LEDs temporarily. After the until-time, breathing
// returns. Also exposes a hook the LLM tool layer will use later
// for commands like "BMO, turn your lights red."
bool led_override_active = false;
uint8_t led_override_r = 0, led_override_g = 0, led_override_b = 0;
unsigned long led_override_until = 0;
bool led_override_flash = false;  // if true, alternate full/dim instead of solid

void set_led_override(uint8_t r, uint8_t g, uint8_t b, unsigned long ms, bool flash) {
  led_override_active = true;
  led_override_r = r; led_override_g = g; led_override_b = b;
  led_override_until = millis() + ms;
  led_override_flash = flash;
}

void update_leds() {
  unsigned long now = millis();
  if (now - last_led_update_ms < 50) return;
  last_led_update_ms = now;

  // Override mode -- show a solid (or flashing) color until the timer expires.
  if (led_override_active && now < led_override_until) {
    uint8_t r = led_override_r, g = led_override_g, b = led_override_b;
    if (led_override_flash) {
      // Flash on/off at 8 Hz
      if ((now / 125) % 2 == 0) { r = 0; g = 0; b = 0; }
    }
    for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, b);
    M5StackChan.refreshRgb();
    return;
  } else if (led_override_active) {
    led_override_active = false;
  }

  // Breathing brightness, 3 s sinusoidal period, 10%..100% of LED ceiling
  float phase = ((now - led_phase_start_ms) % 3000) / 3000.0f;
  float breath = 0.10f + 0.90f * (0.5f - 0.5f * cosf(phase * 2.0f * PI));

  // Mood-derived hue (same math as mood_to_color, in float-space)
  float r = 55, g = 200, b = 170;
  if (mood.valence > 0) {
    r += mood.valence * 150; g += mood.valence * 30; b -= mood.valence * 80;
  } else {
    r -= (-mood.valence) * 30; g -= (-mood.valence) * 130; b += (-mood.valence) * 50;
  }
  // Scale by energy (mood brightness) AND breath. Final 0.35x is the LED
  // intensity ceiling -- LEDs are bright physical things, don't max them out.
  float bm = (0.4f + mood.energy * 0.6f) * breath * 0.35f;
  r *= bm; g *= bm; b *= bm;
  uint8_t ri = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
  uint8_t gi = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
  uint8_t bi = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));

  for (int i = 0; i < 12; i++) {
    M5StackChan.setRgbColor(i, ri, gi, bi);
  }
  M5StackChan.refreshRgb();
}

void leds_off() {
  for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, 0, 0, 0);
  M5StackChan.refreshRgb();
}

// === Phase 9a: IMU shake detection -> dizzy_gesture ===
// Samples accelerometer at ~30 Hz. A "peak" is a magnitude reading above
// 1.7 g (gravity is ~1.0 g). When 3+ peaks land within 1.5 s, the robot
// has been shaken -- fire the dizzy reaction.

unsigned long shake_peaks[5] = {0,0,0,0,0};
int shake_peak_idx = 0;
unsigned long last_shake_sample_ms = 0;

void check_shake() {
  unsigned long now = millis();
  if (now - last_shake_sample_ms < 30) return;
  last_shake_sample_ms = now;

  float ax = 0, ay = 0, az = 0;
  if (!M5.Imu.getAccelData(&ax, &ay, &az)) return;
  float mag = sqrtf(ax*ax + ay*ay + az*az);

  if (mag > 1.7f) {
    // Debounce: don't record a new peak within 100 ms of the last one
    if (now - shake_peaks[shake_peak_idx] > 100) {
      shake_peak_idx = (shake_peak_idx + 1) % 5;
      shake_peaks[shake_peak_idx] = now;
    }
  }

  int recent = 0;
  for (int i = 0; i < 5; i++) {
    if (shake_peaks[i] > 0 && (now - shake_peaks[i]) < 1500) recent++;
  }
  if (recent >= 3) {
    for (int i = 0; i < 5; i++) shake_peaks[i] = 0;
    last_interaction_ms = now;  // shake counts as interaction
    dizzy_gesture();
  }
}

void dizzy_gesture() {
  // Face: SWIRL eyes (off-center concentric rings) + open mouth, plus
  // pink cheek blush, locked for ~3.5 s.
  face_override_active = true;
  face_override_eye    = EyeState::SWIRL;
  face_override_mouth  = MouthShape::OPEN;
  face_override_until  = millis() + 3500;
  show_pink_cheeks     = true;
  pink_cheeks_until    = millis() + 3500;

  // LEDs flash hot pink/red for the duration
  set_led_override(220, 60, 100, 3500, true);

  current_idle_label   = "dizzy!";
  idle_label_until     = millis() + 3000;

  // Force a face redraw so user sees the dizzy expression immediately
  draw_face(false);

  // Wobble head left/right, amplitude decaying
  M5StackChan.Motion.moveX(-350, 600); delay(180);
  M5StackChan.Motion.moveX( 350, 600); delay(180);
  M5StackChan.Motion.moveX(-280, 500); delay(200);
  M5StackChan.Motion.moveX( 280, 500); delay(200);
  M5StackChan.Motion.moveX(-180, 400); delay(220);
  M5StackChan.Motion.moveX( 180, 400); delay(220);
  M5StackChan.Motion.moveX(   0, 300); delay(300);

  // Descending wobbly tones
  for (int i = 0; i < 4; i++) {
    M5.Speaker.tone(700 - i * 90, 140); delay(150);
    M5.Speaker.tone(500 - i * 60, 140); delay(150);
  }

  // Mood: high arousal (jangled), dropped energy (tired), slight negative
  mood.arousal = Mood::clamp01(mood.arousal + 0.40f);
  mood.energy  = Mood::clamp01(mood.energy  - 0.25f);
  mood.valence = Mood::clamp11(mood.valence - 0.20f);
}

// === Phase 9a: Touch reactions (pet / swipe) ===
// Replaces the mood-event-cycling tap UX from MoodLab. Now click = pet,
// swipe forward = excited "yes!", swipe backward = calming pet.

void on_pet_click() {
  mood.valence = Mood::clamp11(mood.valence + 0.40f);  // bigger bump
  mood.arousal = Mood::clamp01(mood.arousal + 0.15f);
  mood.energy  = Mood::clamp01(mood.energy  + 0.10f);

  // Love-struck heart eyes + grin for ~2 s. Dramatic, can't miss it.
  face_override_active = true;
  face_override_eye    = EyeState::HEART;
  face_override_mouth  = MouthShape::GRIN;
  face_override_until  = millis() + 2000;

  // Soft pink LED pulse to match the hearts
  set_led_override(255, 105, 160, 2000, false);

  current_idle_label = "love!";
  idle_label_until   = millis() + 1500;
  M5.Speaker.tone(880, 60); delay(70);
  M5.Speaker.tone(1047, 80); delay(85);
  last_interaction_ms = millis();
}

void on_swipe_forward() {
  mood.valence = Mood::clamp11(mood.valence + 0.40f);
  mood.arousal = Mood::clamp01(mood.arousal + 0.30f);
  mood.energy  = Mood::clamp01(mood.energy  + 0.20f);

  face_override_active = true;
  face_override_eye    = EyeState::WIDE;
  face_override_mouth  = MouthShape::GRIN;
  face_override_until  = millis() + 2500;

  current_idle_label = "swipe!";
  idle_label_until   = millis() + 1800;
  M5.Speaker.tone(523, 60);  delay(65);
  M5.Speaker.tone(784, 60);  delay(65);
  M5.Speaker.tone(1047, 120); delay(130);
  last_interaction_ms = millis();
}

void on_swipe_backward() {
  mood.valence = Mood::clamp11(mood.valence + 0.30f);
  mood.arousal = Mood::clamp01(mood.arousal - 0.20f);
  mood.energy  = Mood::clamp01(mood.energy  + 0.10f);

  face_override_active = true;
  face_override_eye    = EyeState::CONTENT;
  face_override_mouth  = MouthShape::SMILE;
  face_override_until  = millis() + 2500;

  current_idle_label = "calming";
  idle_label_until   = millis() + 1800;
  M5.Speaker.tone(659, 200); delay(210);
  M5.Speaker.tone(523, 250); delay(260);
  last_interaction_ms = millis();
}

// === Phase 9a: Power button -> sleep / wake / deep sleep ===
// Short press toggles a soft sleep state (display dim, LEDs off, no idle
// motion). Long press (2 s+) puts the chip into deep sleep -- press
// button again to wake.

bool sleeping = false;
unsigned long sleep_started_ms = 0;
const uint8_t SLEEP_BRIGHTNESS = 30;   // dim, conserves battery
const uint8_t WAKE_BRIGHTNESS  = 255;  // full

void enter_sleep() {
  sleeping = true;
  sleep_started_ms = millis();
  M5StackChan.Motion.goHome();
  M5StackChan.Display().setBrightness(SLEEP_BRIGHTNESS);
  leds_off();
  // Clear any face overrides so the sleep face shows clean
  face_override_active = false;
  show_pink_cheeks     = false;
}

void exit_sleep() {
  sleeping = false;
  M5StackChan.Display().setBrightness(WAKE_BRIGHTNESS);
  last_interaction_ms = millis();
  last_render_ms      = 0;
  led_phase_start_ms  = millis();
}

// Render the sleep face: dim mood-color background, ASLEEP eyes, mouth
// gently breathing open/closed, "Zz" letters drifting upward.
void draw_sleep_face() {
  face_buffer.fillScreen(mood_to_color(mood));

  // Eyes: ASLEEP shape
  draw_eye(80,  90, EyeState::ASLEEP);
  draw_eye(240, 90, EyeState::ASLEEP);

  // Mouth: breathing animation, 4 s cycle, oval grows then shrinks
  unsigned long now = millis();
  float bp = ((now - sleep_started_ms) % 4000) / 4000.0f;
  float breath = 0.5f - 0.5f * cosf(bp * 2.0f * PI);  // 0..1
  int mouth_ry = (int)(2 + breath * 7);  // 2..9 px tall
  face_buffer.fillEllipse(160, 130, 14, mouth_ry, FEATURE_COLOR);

  // Zz letters: drift upward + cycle in/out every 3 s
  int zcycle = ((now - sleep_started_ms) % 3000);
  int rise = zcycle / 80;  // up to ~37 px rise over 3 s
  if (zcycle < 2400) {
    face_buffer.setTextColor(FEATURE_COLOR);
    face_buffer.setTextSize(3);
    face_buffer.setCursor(250, 50 - rise);
    face_buffer.print("z");
    face_buffer.setTextSize(2);
    face_buffer.setCursor(275, 70 - rise);
    face_buffer.print("Z");
  }

  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

// === Triple-tap detection -> sleep ===
unsigned long recent_taps[3] = {0, 0, 0};
int tap_idx = 0;

bool record_tap_and_check_triple() {
  unsigned long now = millis();
  tap_idx = (tap_idx + 1) % 3;
  recent_taps[tap_idx] = now;
  // Triple = all three slots populated AND oldest within 1.2 s
  if (recent_taps[0] && recent_taps[1] && recent_taps[2]) {
    unsigned long oldest = recent_taps[0];
    for (int i = 1; i < 3; i++) if (recent_taps[i] < oldest) oldest = recent_taps[i];
    if (now - oldest < 1200) {
      // Triple tap! Clear so we don't double-fire.
      for (int i = 0; i < 3; i++) recent_taps[i] = 0;
      return true;
    }
  }
  return false;
}

// Visual debug -- shown in the corner of the screen when a power-button
// event fires, so we can tell whether the button is registering at all.
extern const char* pwr_debug_label;
extern unsigned long pwr_debug_until;

void handle_power_button() {
  // Confirmed: CoreS3's PMIC swallows short presses, only wasHold() registers.
  // So we use wasHold() for sleep toggle, and pressedFor(3000) for deep sleep.
  if (M5.BtnPWR.wasHold()) {
    pwr_debug_label = "PWR";
    pwr_debug_until = millis() + 1200;
    if (sleeping) exit_sleep();
    else          enter_sleep();
  }
  if (M5.BtnPWR.pressedFor(3000)) {
    pwr_debug_label = "DEEP";
    pwr_debug_until = millis() + 1200;
    enter_sleep();
    delay(300);
    esp_deep_sleep_start();
  }
}

// === Phase 9c: Audio capture and playback ===
// Foundation for Phase 9d (cloud STT/LLM). The HTTP handler just sets a
// flag; the actual record-and-playback runs in the main loop context,
// where blocking-for-seconds is safe. Each step writes a diagnostic
// label to the screen so we can see exactly where it stops if it stops.

const int    AUDIO_SAMPLE_RATE     = 16000;
const int    AUDIO_RECORD_SECONDS  = 4;  // +1 s buffer so warmup doesn't eat speech
const size_t AUDIO_BUFFER_SAMPLES  = AUDIO_SAMPLE_RATE * AUDIO_RECORD_SECONDS;
int16_t*     audio_buffer          = nullptr;
volatile bool audio_test_pending   = false;
volatile bool tone_test_pending    = false;
char audio_diag_label[40] = {0};

void init_audio_buffer() {
  audio_buffer = (int16_t*)heap_caps_malloc(
    AUDIO_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
}

void run_tone_only_test() {
  // Isolates the speaker -- never touches the mic. If a tone plays from
  // this, the speaker is fine when mic was never invoked.
  set_led_override(80, 220, 80, 2500, false);
  face_override_active = true;
  face_override_eye    = EyeState::NORMAL;
  face_override_mouth  = MouthShape::SMILE;
  face_override_until  = millis() + 2500;
  current_idle_label   = "tone test (mic untouched)";
  idle_label_until     = millis() + 2500;
  draw_face(false);
  M5.Speaker.setVolume(255);
  M5.Speaker.tone(440,  300); delay(330);
  M5.Speaker.tone(659,  300); delay(330);
  M5.Speaker.tone(880,  300); delay(330);
  M5.Speaker.tone(1320, 400); delay(420);
}

void set_audio_diag(const char* msg, int ms = 2500) {
  strncpy(audio_diag_label, msg, sizeof(audio_diag_label) - 1);
  current_idle_label = audio_diag_label;
  idle_label_until   = millis() + ms;
  draw_face(false);
}

void run_audio_test_now() {
  if (!audio_buffer) {
    set_audio_diag("audio buf NULL", 3000);
    return;
  }

  // STEP 1: "Get ready" -- speaker plays a 3-note ascending cue.
  // User knows something is coming; mic is NOT recording yet.
  face_override_active = true;
  face_override_eye    = EyeState::NORMAL;
  face_override_mouth  = MouthShape::SMILE;
  face_override_until  = millis() + 1200;
  set_led_override(255, 180, 0, 1200, false);
  set_audio_diag("get ready...", 1200);
  M5.Speaker.setVolume(255);
  M5.Speaker.tone(523, 120); delay(150);
  M5.Speaker.tone(659, 120); delay(150);
  M5.Speaker.tone(880, 200); delay(280);

  // STEP 2: Switch to mic and start recording
  if (!M5.Mic.begin()) {
    set_audio_diag("mic.begin FAILED", 3500);
    return;
  }
  if (!M5.Mic.record(audio_buffer, AUDIO_BUFFER_SAMPLES, AUDIO_SAMPLE_RATE)) {
    set_audio_diag("mic.record FAILED", 3500);
    M5.Mic.end();
    return;
  }

  // STEP 3: 400 ms mic warmup BEFORE we tell the user to speak.
  // Any "first part cut off" is buffered by the longer record duration.
  delay(400);

  // STEP 4: Bright green "SPEAK NOW!" cue -- user starts speaking now.
  face_override_active = true;
  face_override_eye    = EyeState::WIDE;
  face_override_mouth  = MouthShape::OPEN;
  face_override_until  = millis() + AUDIO_RECORD_SECONDS * 1000 + 200;
  set_led_override(0, 255, 0, AUDIO_RECORD_SECONDS * 1000, false);  // bright green
  set_audio_diag("SPEAK NOW!", AUDIO_RECORD_SECONDS * 1000);

  // Wait for the rest of the record window
  unsigned long t0 = millis();
  while (M5.Mic.isRecording() && (millis() - t0) < (AUDIO_RECORD_SECONDS * 1000 + 1000)) {
    delay(50);
  }
  M5.Mic.end();

  // Check we actually captured anything by finding the peak amplitude
  int peak = 0;
  for (size_t i = 0; i < AUDIO_BUFFER_SAMPLES; i++) {
    int v = audio_buffer[i] < 0 ? -audio_buffer[i] : audio_buffer[i];
    if (v > peak) peak = v;
  }

  // Step 2: thinking (HELD 5 seconds so user can read the peak value)
  face_override_active = true;
  face_override_eye    = EyeState::NORMAL;
  face_override_mouth  = MouthShape::NEUTRAL;
  face_override_until  = millis() + 5000;
  set_led_override(255, 180, 0, 5000, false);
  snprintf(audio_diag_label, sizeof(audio_diag_label), "peak=%d", peak);
  current_idle_label = audio_diag_label;
  idle_label_until   = millis() + 5000;
  draw_face(false);
  delay(5000);

  // Step 3: aggressively reset speaker -- end existing config, delay, re-init.
  // mic.end() can leave I2S in a half-initialized state on the CoreS3.
  delay(300);
  M5.Speaker.end();
  delay(200);
  if (!M5.Speaker.begin()) {
    set_audio_diag("spk.begin FAILED", 3500);
    return;
  }
  M5.Speaker.setVolume(255);  // max
  delay(100);

  // VERIFICATION TONE: if you hear this beep, speaker works.
  // If you don't hear it, speaker re-init failed silently.
  face_override_active = true;
  face_override_eye    = EyeState::NORMAL;
  face_override_mouth  = MouthShape::SMILE;
  face_override_until  = millis() + 1000;
  set_led_override(80, 220, 80, 1000, false);  // green for "speaker check"
  set_audio_diag("verify tone", 1000);
  M5.Speaker.tone(880, 300); delay(350);
  M5.Speaker.tone(1320, 300); delay(350);

  // Step 4: actual playback of the recorded buffer
  face_override_until  = millis() + AUDIO_RECORD_SECONDS * 1000 + 400;
  set_led_override(200, 100, 220, AUDIO_RECORD_SECONDS * 1000, false);
  set_audio_diag("playing back", AUDIO_RECORD_SECONDS * 1000);

  if (!M5.Speaker.playRaw(audio_buffer, AUDIO_BUFFER_SAMPLES, AUDIO_SAMPLE_RATE, false, 1, -1)) {
    set_audio_diag("spk.playRaw FAILED", 3500);
    return;
  }

  t0 = millis();
  while (M5.Speaker.isPlaying() && (millis() - t0) < (AUDIO_RECORD_SECONDS * 1000 + 1000)) {
    delay(50);
  }
  set_audio_diag("done", 2500);
}

// === Phase 9b: Wi-Fi provisioning ===
// On boot, try saved credentials. If none, or if connecting fails, enter
// setup mode: start an open AP called "BMO-Setup" with a captive portal
// that serves a credential-entry form. On submit, save to NVS and reboot.
// Once connected in STA mode, also serve a status page at http://bmo.local/
// with a "Forget Wi-Fi" button for re-provisioning.

WebServer http_server(80);
DNSServer dns_server;
Preferences wifi_prefs;
bool wifi_setup_mode = false;
String wifi_connected_ssid = "";

const char* SETUP_AP_NAME = "BMO-Setup";
const char* SETUP_AP_PASS = "letsbmo!";  // 8+ chars (WPA2 minimum)

const char* SETUP_PAGE_HTML = R"HTML(<!DOCTYPE html><html>
<head>
<meta name='viewport' content='width=device-width, initial-scale=1'>
<title>BMO Setup</title>
<style>
body { font-family: -apple-system, sans-serif; max-width: 420px; margin: 2em auto;
       padding: 0 1em; background: #adf2dc; color: #1a4d3c; }
h1 { font-size: 2em; }
input, button { width: 100%; padding: 12px; margin: 6px 0; font-size: 1em;
                box-sizing: border-box; border-radius: 8px; border: 1px solid #1a4d3c; }
button { background: #1a4d3c; color: white; border: none; font-weight: bold; }
</style>
</head>
<body>
<h1>Hi! I'm BMO.</h1>
<p>Pick a Wi-Fi to connect me to.</p>
<form action='/save' method='POST'>
  <input type='text' name='ssid' placeholder='Wi-Fi name' required autofocus>
  <input type='password' name='pass' placeholder='Password'>
  <button type='submit'>Connect BMO</button>
</form>
</body></html>)HTML";

void show_wifi_setup_screen() {
  face_buffer.fillScreen(mood_to_color(mood));
  face_buffer.setTextColor(FEATURE_COLOR);

  face_buffer.setTextSize(2);
  face_buffer.setCursor(20, 8);   face_buffer.print("Hi! I'm BMO.");
  face_buffer.setCursor(20, 35);  face_buffer.print("Setup my wifi:");

  face_buffer.setTextSize(1);
  face_buffer.setCursor(20, 70);  face_buffer.print("1. Join Wi-Fi:");
  face_buffer.setTextSize(2);
  face_buffer.setCursor(40, 85);  face_buffer.print("BMO-Setup");
  face_buffer.setTextSize(1);
  face_buffer.setCursor(40, 110); face_buffer.print("password: letsbmo!");

  face_buffer.setCursor(20, 135); face_buffer.print("2. Open in browser:");
  face_buffer.setTextSize(2);
  face_buffer.setCursor(40, 150); face_buffer.print("192.168.4.1");

  // Tiny on-screen diagnostic so we can see if a client actually joined
  face_buffer.setTextSize(1);
  face_buffer.setCursor(20, 185);
  int n = WiFi.softAPgetStationNum();
  face_buffer.printf("Clients connected: %d", n);

  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

void show_wifi_connecting_screen(const String& ssid) {
  face_buffer.fillScreen(mood_to_color(mood));
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setTextSize(2);
  face_buffer.setCursor(20, 50);  face_buffer.print("Connecting...");
  face_buffer.setCursor(20, 90);  face_buffer.print(ssid);
  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

void send_status_page() {
  String html = "<!DOCTYPE html><html><head>"
                "<meta name='viewport' content='width=device-width, initial-scale=1'>"
                "<title>BMO Status</title>"
                "<style>body{font-family:-apple-system,sans-serif;max-width:420px;"
                "margin:2em auto;padding:0 1em;background:#adf2dc;color:#1a4d3c;}"
                "button{width:100%;padding:12px;font-size:1em;background:#1a4d3c;"
                "color:white;border:none;border-radius:8px;font-weight:bold;"
                "margin-top:8px;}</style>"
                "</head><body><h1>BMO is online.</h1>";
  html += "<p>Wi-Fi: <b>" + wifi_connected_ssid + "</b></p>";
  html += "<p>IP: <b>" + WiFi.localIP().toString() + "</b></p>";
  html += "<h2>Audio tests</h2>"
          "<p><b>Tone only:</b> 4 ascending notes via the speaker. Never "
          "touches the mic. Use this to confirm the speaker works in isolation.</p>"
          "<form action='/test-tone' method='POST'>"
          "<button type='submit'>Test Tone Only</button></form>"
          "<p><b>Mic + speaker:</b> record 3 s then play it back.</p>"
          "<form action='/test-audio' method='POST'>"
          "<button type='submit'>Test Mic + Speaker (record + playback)</button></form>";
  html += "<h2>Settings</h2>"
          "<form action='/forget' method='POST'>"
          "<button type='submit'>Forget Wi-Fi (re-setup)</button></form>";
  html += "</body></html>";
  http_server.send(200, "text/html", html);
}

void register_setup_routes() {
  http_server.on("/", []() {
    http_server.send(200, "text/html", SETUP_PAGE_HTML);
  });
  // Captive-portal probes most phones make -- send them to /
  http_server.on("/generate_204", []() { http_server.send(200, "text/html", SETUP_PAGE_HTML); });
  http_server.on("/hotspot-detect.html", []() { http_server.send(200, "text/html", SETUP_PAGE_HTML); });
  http_server.onNotFound([]() {
    http_server.sendHeader("Location", "http://192.168.4.1/", true);
    http_server.send(302, "text/plain", "");
  });
  http_server.on("/save", HTTP_POST, []() {
    String ssid = http_server.arg("ssid");
    String pass = http_server.arg("pass");
    if (ssid.length() == 0) {
      http_server.send(400, "text/plain", "SSID required");
      return;
    }
    wifi_prefs.begin("bmo_wifi", false);
    wifi_prefs.putString("ssid", ssid);
    wifi_prefs.putString("pass", pass);
    wifi_prefs.end();
    http_server.send(200, "text/html",
      "<h1>Saved! BMO is restarting.</h1>"
      "<p>I'll connect to your Wi-Fi in a moment.</p>");
    delay(800);
    ESP.restart();
  });
}

void register_status_routes() {
  http_server.on("/", []() { send_status_page(); });
  http_server.on("/forget", HTTP_POST, []() {
    wifi_prefs.begin("bmo_wifi", false);
    wifi_prefs.clear();
    wifi_prefs.end();
    http_server.send(200, "text/html",
      "<h1>Wi-Fi forgotten.</h1><p>BMO is restarting into setup mode.</p>");
    delay(800);
    ESP.restart();
  });
  http_server.on("/test-audio", HTTP_POST, []() {
    audio_test_pending = true;
    http_server.send(200, "text/html",
      "<h1>BMO is listening for 3 seconds...</h1>"
      "<p>Then BMO will play it back. Watch BMO's face.</p>"
      "<p><a href='/'>Back to status</a></p>");
  });
  http_server.on("/test-tone", HTTP_POST, []() {
    tone_test_pending = true;
    http_server.send(200, "text/html",
      "<h1>Playing a tone test...</h1>"
      "<p><a href='/'>Back to status</a></p>");
  });
}

void start_wifi_setup_mode() {
  wifi_setup_mode = true;
  WiFi.mode(WIFI_AP);
  // Explicit AP IP config so 192.168.4.1 is reliable
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  // WPA2 password so iPhones/Androids don't auto-disconnect from a no-internet open AP
  WiFi.softAP(SETUP_AP_NAME, SETUP_AP_PASS);
  delay(100);
  dns_server.start(53, "*", IPAddress(192, 168, 4, 1));  // captive-portal DNS
  register_setup_routes();
  http_server.begin();
  show_wifi_setup_screen();
}

void wifi_init() {
  wifi_prefs.begin("bmo_wifi", true);
  String saved_ssid = wifi_prefs.getString("ssid", "");
  String saved_pass = wifi_prefs.getString("pass", "");
  wifi_prefs.end();

  if (saved_ssid.length() == 0) {
    start_wifi_setup_mode();
    return;
  }

  show_wifi_connecting_screen(saved_ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(saved_ssid.c_str(), saved_pass.c_str());

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifi_connected_ssid = saved_ssid;
    if (MDNS.begin("bmo")) MDNS.addService("http", "tcp", 80);
    register_status_routes();
    http_server.begin();
  } else {
    start_wifi_setup_mode();
  }
}

void draw_wifi_status_indicator() {
  // Tiny indicator in the top-left of the face, only when connected.
  if (wifi_setup_mode || wifi_connected_ssid.length() == 0) return;
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setTextSize(1);
  face_buffer.setCursor(2, 2);
  face_buffer.print("wifi");
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

// Phase 9a additions
bool show_pink_cheeks = false;
unsigned long pink_cheeks_until = 0;
const char* pwr_debug_label = nullptr;
unsigned long pwr_debug_until = 0;

unsigned long last_tick_ms   = 0;
unsigned long last_render_ms = 0;
unsigned long next_blink_at  = 0;
unsigned long blink_until    = 0;
bool was_blinking = false;

void setup() {
  M5StackChan.begin();
  M5.Speaker.setVolume(128);
  M5StackChan.Motion.goHome();
  delay(500);

  face_buffer.setPsram(true);
  face_buffer.setColorDepth(16);
  face_buffer.createSprite(320, 240);

  // Phase 9c: pre-allocate the audio capture buffer (in PSRAM)
  init_audio_buffer();

  // Wi-Fi before the boot chime so the screen can show setup instructions
  // immediately if needed (no faux-boot when we're in setup mode).
  wifi_init();

  // Quiet boot sound only once we're past wifi (or in setup mode anyway)
  M5.Speaker.tone(523, 60); delay(65);
  M5.Speaker.tone(784, 80); delay(85);

  randomSeed(esp_random());
  unsigned long now = millis();
  last_tick_ms        = now;
  last_interaction_ms = now;
  next_blink_at       = now + 2000;
  if (!wifi_setup_mode) draw_face(false);
}

void loop() {
  M5StackChan.update();
  M5.update();

  // === Wi-Fi setup-mode branch: serve captive portal, hold setup screen ===
  if (wifi_setup_mode) {
    dns_server.processNextRequest();
    http_server.handleClient();
    if (millis() - last_render_ms > 1000) {
      show_wifi_setup_screen();
      last_render_ms = millis();
    }
    delay(20);
    return;
  }

  // STA mode -- handle status-page requests in the background
  http_server.handleClient();

  handle_power_button();

  // === Sleep branch: render breathing-Zz face, wake on tap. ===
  if (sleeping) {
    auto& ts_sleep = M5StackChan.TouchSensor;
    if (ts_sleep.wasClicked()) {
      exit_sleep();
      // Don't count the wake-tap as a pet -- fall through to normal loop next iteration
      delay(20);
      return;
    }
    unsigned long now = millis();
    if (now - last_render_ms > 80) {
      draw_sleep_face();
      last_render_ms = now;
    }
    delay(40);
    return;
  }

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

  // Touch sensor: click = pet (unless 3rd of a triple, then sleep).
  // Swipe forward = excited, swipe back = calm.
  auto& ts = M5StackChan.TouchSensor;
  if (ts.wasClicked()) {
    if (record_tap_and_check_triple()) {
      enter_sleep();
    } else {
      on_pet_click();
    }
  }
  if (ts.wasSwipedForward())  on_swipe_forward();
  if (ts.wasSwipedBackward()) on_swipe_backward();

  // Shake -> dizzy gesture (checks IMU, fires the gesture if shaken)
  check_shake();

  if (blink_changed || (now - last_render_ms > 200)) {
    draw_face(currently_blinking);
    last_render_ms = now;
    was_blinking = currently_blinking;
  }

  // LED breathing pulse at mood color
  update_leds();

  // Phase 9c: run audio test if web button queued one
  if (audio_test_pending) {
    audio_test_pending = false;
    run_audio_test_now();
  }
  if (tone_test_pending) {
    tone_test_pending = false;
    run_tone_only_test();
  }

  // Subtle "alive" layer between full behaviors, plus full-behavior firings.
  maybe_fire_micro();
  maybe_fire_idle();

  delay(20);
}
