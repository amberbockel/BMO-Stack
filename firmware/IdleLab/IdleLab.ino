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
// Phase 9d: cloud STT + LLM (Gemini)
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_camera.h"
extern "C" {
  #include "mbedtls/base64.h"
  // ESP-SR WakeNet (on-device wake word detection)
  #include "esp_wn_iface.h"
  #include "esp_wn_models.h"
  #include "model_path.h"
}

// === TensorFlow Lite Micro (for custom wake-word models like 'Hey Beepoh') ===
// Audio frontend is NOT yet implemented; this only sets up the interpreter so
// we can verify the model loads. The model expects pre-computed 40-dim mel
// spectrogram features (see preprocessor_settings in ESPHome micro_wake_word).
// Next session: implement Hann window + 256-FFT + 40-channel mel filterbank
// (125-7500 Hz) + log scale + int8 quantization, then feed features to model.
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "wakenet_model/hey_beepoh_model.h"

// Audio frontend: produces 40-dim mel features that the wake-word model
// expects. Constants from ESPHome's preprocessor_settings.h.
extern "C" {
  #include "src/microfrontend/frontend.h"
  #include "src/microfrontend/frontend_util.h"
}
#if __has_include("gemini_credentials.h")
  #include "gemini_credentials.h"
#endif
#ifndef BMO_GEMINI_API_KEY
  #define BMO_GEMINI_API_KEY ""
#endif
#ifndef BMO_TTS_API_KEY
  // If only one key was provided, fall back to using it for both services.
  #define BMO_TTS_API_KEY BMO_GEMINI_API_KEY
#endif

// === Hoisted types (Arduino auto-prototype workaround) ===
// Any type referenced in a function signature must be defined here, not
// next to the section that uses it, because Arduino auto-inserts forward
// declarations for all functions at the top of the file.
enum class MouthShape { SMILE, NEUTRAL, FROWN, OPEN, GRIN };
enum class EyeState   { BLINK, SLEEPY, NORMAL, WIDE, CONTENT, ASLEEP, SWIRL, HEART, FLAT };
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
    case EyeState::FLAT: {
      // Concentrating "thinking" eye -- horizontal flat line, thicker
      // than a blink. Reads as focused attention, like a puppy listening.
      face_buffer.fillRect(cx - 9, cy - 3, 18, 5, FEATURE_COLOR);
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
// Phase 9d state -- defined further down
extern bool showing_response;
extern unsigned long response_until;
extern volatile bool conversation_pending;
// Phase 9e
extern bool quiet_mode;
extern const uint8_t BEEP_VOLUME;
// Phase 9g -- gesture function forward decls so execute_tool can call them
void excited_wiggle();
void happy_bounce();
void sigh();
void curious_tilt();
void idle_look_around();
void idle_stretch();
float quiet_timeout_seconds();
void dizzy_gesture();
void run_conversation();

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
  // Cadence tuning, v5 (responsiveness pass): base 6 s, clamps [3, 12] s.
  // v4 was 10 s base / [5, 20] -- too still. Beemo is supposed to feel alive.
  // Micro-fidget layer (see maybe_fire_micro) fills the gaps between
  // these full behaviors with tiny twitches every 1.5-3.5 s.
  float base = 6.0f;
  base *= (1.5f - mood.arousal);
  base *= (1.7f - mood.energy);
  if (base <  3.0f) base =  3.0f;
  if (base > 12.0f) base = 12.0f;
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
  next_micro_at = now + 1500 + random(0, 2000);  // 1.5-3.5 s (was 2.5-5.5)
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
bool led_override_sticky = false; // user-requested via tool; only another sticky can replace it

void set_led_override(uint8_t r, uint8_t g, uint8_t b, unsigned long ms, bool flash, bool sticky = false) {
  // A sticky (user-asked-for) color can only be replaced by another sticky call.
  if (led_override_active && led_override_sticky && !sticky &&
      millis() < led_override_until) return;
  led_override_active = true;
  led_override_r = r; led_override_g = g; led_override_b = b;
  led_override_until = millis() + ms;
  led_override_flash = flash;
  led_override_sticky = sticky;
  // Push to hardware NOW. update_leds() only runs in the main loop, so without
  // this immediate push the override would not be visible during conversation
  // turns or other blocking flows.
  for (int i = 0; i < 12; i++) M5StackChan.setRgbColor(i, r, g, b);
  M5StackChan.refreshRgb();
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
  // Phase 9d: swipe forward = "BMO, listen to me" -> trigger conversation.
  // The old excited reaction is now driven only by idle behaviors.
  last_interaction_ms = millis();
  conversation_pending = true;
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

// === Double-tap-to-abort during conversation ===
// User can double-tap the touch sensor while BMO is speaking/listening to
// cut the conversation short. Polled from the listen and playback loops.
volatile bool conversation_abort = false;
unsigned long last_abort_tap_ms = 0;

bool poll_abort_double_tap() {
  M5StackChan.update();
  if (M5StackChan.TouchSensor.wasClicked()) {
    unsigned long now = millis();
    if (last_abort_tap_ms != 0 && (now - last_abort_tap_ms) < 600) {
      conversation_abort = true;
      last_abort_tap_ms = 0;
      Serial.println("[abort] double-tap -> stop conversation");
      return true;
    }
    last_abort_tap_ms = now;
  }
  return false;
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
const int    AUDIO_RECORD_SECONDS  = 5;
const size_t AUDIO_BUFFER_SAMPLES  = AUDIO_SAMPLE_RATE * AUDIO_RECORD_SECONDS;
// How many samples were ACTUALLY recorded this turn (VAD may end early).
// Used by the WAV encoder so we don't send 5 seconds of silence to Gemini
// when the user stopped talking at 2 seconds.
volatile size_t actual_audio_samples = AUDIO_BUFFER_SAMPLES;

// === Always-listening mode (ambient voice activation) ===
// When ON, Beemo runs a continuous background mic recording. A VAD detector
// in the main loop polls for speech; when it detects "user said something,
// then stopped," it pre-captures the audio and triggers a conversation
// using that audio directly (no re-recording needed).
volatile bool   listening_mode             = true;     // toggleable via web
volatile bool   audio_pre_captured         = false;    // conversation should use audio_buffer as-is
volatile size_t pre_captured_samples       = 0;
// True only for the FIRST Gemini call of a turn that came from ambient.
// Causes call_gemini_with_audio to use an "is this addressed to Beemo?" prompt
// and dismiss silently if Gemini answers NOT_ADDRESSED.
volatile bool   current_turn_ambient       = false;
// Ambient recorder state machine
enum class AmbientState { IDLE, RECORDING };
AmbientState   ambient_state               = AmbientState::IDLE;
unsigned long  ambient_record_start_ms     = 0;
unsigned long  ambient_last_voice_ms       = 0;
bool           ambient_ever_heard_voice    = false;
// WakeNet processing cursor: how many samples of the current recording
// we've already fed to the wake-word detector this cycle.
size_t         ambient_wn_processed        = 0;
// Refractory window: after wake/conversation, briefly suppress re-detection
// so residual room audio / TTS reverb doesn't immediately re-trigger.
unsigned long  wake_cooldown_until_ms      = 0;

// === ESP-SR WakeNet on-device wake word ===
// Loaded from the "model" partition (esp_sr_16 layout) at boot.
// Detection runs on the same audio stream as ambient_tick.
static srmodel_list_t*       wn_models     = nullptr;
static const esp_wn_iface_t* wn_iface      = nullptr;
static model_iface_data_t*   wn_data       = nullptr;
static int                   wn_chunk_size = 0;
static char                  wn_word_name[64] = "";
static bool                  wn_ready      = false;

// Audio frontend state -- one global instance, init at boot. Constants
// mirrored from ESPHome's preprocessor_settings.h.
struct FrontendState mww_frontend_state;
bool   mww_frontend_ready = false;
// Streaming inference state
float  mww_prob_window[5]   = {0};  // sliding window of last 5 probabilities
int    mww_prob_window_idx  = 0;
size_t mww_samples_processed= 0;
unsigned mww_invocations    = 0;
float  mww_last_raw_prob    = 0.0f;
float  mww_max_prob_seen    = 0.0f;
float  mww_last_avg_prob    = 0.0f;

bool mww_frontend_init() {
  struct FrontendConfig cfg = {};
  cfg.window.size_ms                       = 30;        // FEATURE_DURATION_MS
  cfg.window.step_size_ms                  = 20;        // features_step_size_
  cfg.filterbank.num_channels              = 40;        // PREPROCESSOR_FEATURE_SIZE
  cfg.filterbank.lower_band_limit          = 125.0f;
  cfg.filterbank.upper_band_limit          = 7500.0f;
  cfg.noise_reduction.smoothing_bits       = 10;
  cfg.noise_reduction.even_smoothing       = 0.025f;
  cfg.noise_reduction.odd_smoothing        = 0.06f;
  cfg.noise_reduction.min_signal_remaining = 0.05f;
  cfg.pcan_gain_control.enable_pcan        = 1;
  cfg.pcan_gain_control.strength           = 0.95f;
  cfg.pcan_gain_control.offset             = 80.0f;
  cfg.pcan_gain_control.gain_bits          = 21;
  cfg.log_scale.enable_log                 = 1;
  cfg.log_scale.scale_shift                = 6;
  const int sample_rate = 16000;
  if (!FrontendPopulateState(&cfg, &mww_frontend_state, sample_rate)) {
    Serial.println("[mww-fe] FrontendPopulateState failed");
    return false;
  }
  mww_frontend_ready = true;
  Serial.println("[mww-fe] frontend ready");
  return true;
}

// === TFLite Micro 'Hey Beepoh' interpreter setup (Phase 9j-2) ===
// Currently we're verifying just the interpreter foundation: get
// AllocateTensors() to succeed with all ops registered + resource variables
// for the streaming model. Audio frontend wiring comes in a subsequent step.
// Until step-2 (audio frontend) lands, the actual mic -> features -> invoke
// path is not built, so WakeNet9 'Hi, ESP' remains the daily-driver wake
// word even with TFLM_BEEPOH_ENABLED = 1.
#define TFLM_BEEPOH_ENABLED 1
// Result of tflm_beepoh_init for on-screen reporting since Serial is lossy
// on this board after boot phase.
char tflm_boot_status[60] = "tflm: not run";
// Opcodes 83 = Pack, 88 = Unpack -- streaming models concat/split states.
// Confirmed by inspecting tensorflow/lite/builtin_ops.h.
namespace tflm_beepoh {
  // Streaming wake-word models need a larger arena because the resource
  // variables (stateful tensors) live inside it across calls.
  constexpr int kTensorArenaSize    = 80 * 1024;   // 80 KB PSRAM
  constexpr int kNumResourceVars    = 32;          // upper bound; tune later
  uint8_t* tensor_arena             = nullptr;
  const tflite::Model* model        = nullptr;
  tflite::MicroAllocator* allocator = nullptr;
  tflite::MicroResourceVariables* resource_variables = nullptr;
  tflite::MicroInterpreter* interpreter = nullptr;
  // 50 slots leaves headroom over the ~25-30 ops streaming models use.
  using OpResolver = tflite::MicroMutableOpResolver<50>;
  OpResolver* resolver = nullptr;
  TfLiteTensor* input  = nullptr;
  TfLiteTensor* output = nullptr;
  bool ready = false;
}

bool tflm_beepoh_init() {
  using namespace tflm_beepoh;
  Serial.println("[tflm] starting init...");
  strncpy(tflm_boot_status, "tflm: alloc arena", sizeof(tflm_boot_status) - 1);

  // Allocate tensor arena in PSRAM
  tensor_arena = (uint8_t*)heap_caps_malloc(kTensorArenaSize, MALLOC_CAP_SPIRAM);
  if (!tensor_arena) {
    Serial.println("[tflm] failed to alloc tensor arena in PSRAM");
    strncpy(tflm_boot_status, "tflm: arena alloc FAIL", sizeof(tflm_boot_status) - 1);
    return false;
  }
  strncpy(tflm_boot_status, "tflm: arena ok, loading model", sizeof(tflm_boot_status) - 1);

  // Load model
  model = tflite::GetModel(hey_beepoh_tflite);
  if (!model) {
    Serial.println("[tflm] GetModel returned null");
    return false;
  }
  Serial.printf("[tflm] model version: %lu (expected %d)\n",
                (unsigned long)model->version(), TFLITE_SCHEMA_VERSION);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    Serial.printf("[tflm] WARNING: schema version mismatch -- may fail\n");
  }

  // Op resolver. These are best-guess ops for streaming wake-word models.
  // If AllocateTensors() fails with "Op X not found in OpResolver", come
  // back here and add the missing op.
  resolver = new OpResolver();
  // Common NN ops
  resolver->AddConv2D();
  resolver->AddDepthwiseConv2D();
  resolver->AddFullyConnected();
  resolver->AddReshape();
  resolver->AddSoftmax();
  resolver->AddLogistic();
  resolver->AddRelu();
  resolver->AddQuantize();
  resolver->AddDequantize();
  resolver->AddMean();
  resolver->AddMaxPool2D();
  resolver->AddAveragePool2D();
  // Streaming inference ops (microWakeWord stateful models)
  resolver->AddCallOnce();
  resolver->AddVarHandle();
  resolver->AddReadVariable();
  resolver->AddAssignVariable();
  resolver->AddConcatenation();
  resolver->AddStridedSlice();
  resolver->AddPad();
  resolver->AddAdd();
  resolver->AddMul();
  resolver->AddSub();
  resolver->AddDiv();
  resolver->AddMinimum();
  resolver->AddMaximum();
  resolver->AddAbs();
  resolver->AddSquaredDifference();
  resolver->AddTranspose();
  resolver->AddExpandDims();
  resolver->AddSqueeze();
  resolver->AddSplit();
  resolver->AddSlice();
  resolver->AddPack();
  resolver->AddUnpack();

  // Create allocator + resource variables BEFORE the interpreter so the
  // streaming model's stateful var-handles have a place to live.
  strncpy(tflm_boot_status, "tflm: making allocator", sizeof(tflm_boot_status) - 1);
  allocator = tflite::MicroAllocator::Create(tensor_arena, kTensorArenaSize);
  if (!allocator) {
    Serial.println("[tflm] MicroAllocator::Create failed");
    strncpy(tflm_boot_status, "tflm: allocator FAIL", sizeof(tflm_boot_status) - 1);
    return false;
  }
  strncpy(tflm_boot_status, "tflm: making resource vars", sizeof(tflm_boot_status) - 1);
  resource_variables = tflite::MicroResourceVariables::Create(
    allocator, kNumResourceVars);
  if (!resource_variables) {
    Serial.println("[tflm] MicroResourceVariables::Create failed");
    strncpy(tflm_boot_status, "tflm: resource vars FAIL", sizeof(tflm_boot_status) - 1);
    return false;
  }
  Serial.printf("[tflm] resource vars allocated: %d slots\n", kNumResourceVars);
  strncpy(tflm_boot_status, "tflm: building interpreter", sizeof(tflm_boot_status) - 1);

  // Build interpreter using the allocator we already made
  interpreter = new tflite::MicroInterpreter(
    model, *resolver, allocator, resource_variables);

  strncpy(tflm_boot_status, "tflm: AllocateTensors", sizeof(tflm_boot_status) - 1);
  TfLiteStatus alloc_status = interpreter->AllocateTensors();
  if (alloc_status != kTfLiteOk) {
    Serial.printf("[tflm] AllocateTensors failed: %d -- need more ops or "
                  "bigger arena\n", (int)alloc_status);
    snprintf(tflm_boot_status, sizeof(tflm_boot_status),
             "tflm: AllocateTensors FAIL %d", (int)alloc_status);
    return false;
  }

  // Inspect input/output shape
  input  = interpreter->input(0);
  output = interpreter->output(0);
  Serial.printf("[tflm] input dims: %d", input->dims->size);
  for (int i = 0; i < input->dims->size; i++) {
    Serial.printf("%s%d", i == 0 ? " [" : ",", input->dims->data[i]);
  }
  Serial.printf("] type=%d\n", (int)input->type);
  Serial.printf("[tflm] output dims: %d", output->dims->size);
  for (int i = 0; i < output->dims->size; i++) {
    Serial.printf("%s%d", i == 0 ? " [" : ",", output->dims->data[i]);
  }
  Serial.printf("] type=%d\n", (int)output->type);
  Serial.printf("[tflm] arena used: %u / %u bytes\n",
                (unsigned)interpreter->arena_used_bytes(), kTensorArenaSize);
  ready = true;
  Serial.println("[tflm] ready");
  // Build a detailed shape string for /tflm-status
  char in_shape[40] = "?";
  char out_shape[40] = "?";
  if (input) {
    int p = 0;
    for (int i = 0; i < input->dims->size && p < (int)sizeof(in_shape) - 8; i++) {
      p += snprintf(in_shape + p, sizeof(in_shape) - p, "%s%d",
                    i == 0 ? "" : "x", input->dims->data[i]);
    }
  }
  if (output) {
    int p = 0;
    for (int i = 0; i < output->dims->size && p < (int)sizeof(out_shape) - 8; i++) {
      p += snprintf(out_shape + p, sizeof(out_shape) - p, "%s%d",
                    i == 0 ? "" : "x", output->dims->data[i]);
    }
  }
  snprintf(tflm_boot_status, sizeof(tflm_boot_status),
           "tflm: READY in=%s out=%s type=%d/%d",
           in_shape, out_shape,
           input ? (int)input->type : -1,
           output ? (int)output->type : -1);
  return true;
}

// Feed a batch of int16 samples into the wake-word pipeline. The frontend
// will consume them in 30ms windows with 20ms hops, emit 40-dim mel features,
// and for each feature frame we quantize -> Invoke -> read probability ->
// slide-window-average. Returns true if the wake word is detected this call.
bool mww_pipeline_feed(int16_t* samples, size_t num_samples) {
  using namespace tflm_beepoh;
  if (!mww_frontend_ready || !ready || !interpreter || !input || !output) return false;
  if (input->type != kTfLiteInt8 || output->type != kTfLiteUInt8) return false;

  bool detected = false;
  // Feed samples to the frontend; loop while it produces feature frames
  // (one call may consume only part of the buffer).
  size_t pos = 0;
  while (pos < num_samples) {
    size_t consumed = 0;
    struct FrontendOutput frontend_out = FrontendProcessSamples(
      &mww_frontend_state, samples + pos, num_samples - pos, &consumed);
    pos += consumed;
    if (consumed == 0) break;             // not enough samples for another window
    if (frontend_out.size == 0) continue; // no feature this iteration

    mww_samples_processed += consumed;

    // Quantize the 40-dim uint16 features into the model's int8 input tensor.
    // Following ESPHome's approach: divide by ~256 to fit into int8 range and
    // shift by input zero point. Quantization params come from the model.
    const float input_scale  = input->params.scale;
    const int   input_zero   = input->params.zero_point;
    for (int i = 0; i < 40; i++) {
      // microfrontend emits uint16 values up to ~65535. Map to model's
      // expected range via the per-tensor scale. q = round(x / scale) + zp.
      float v = (float)frontend_out.values[i];
      int q = (int)lroundf(v / (input_scale * 256.0f)) + input_zero;
      if (q < -128) q = -128;
      if (q >  127) q =  127;
      input->data.int8[i] = (int8_t)q;
    }

    // Run inference
    TfLiteStatus s = interpreter->Invoke();
    if (s != kTfLiteOk) {
      Serial.printf("[mww] Invoke failed: %d\n", (int)s);
      continue;
    }
    mww_invocations++;

    // Read output probability (uint8 -> 0..1)
    const float out_scale = output->params.scale;
    const int   out_zero  = output->params.zero_point;
    const uint8_t raw     = output->data.uint8[0];
    float prob = (raw - out_zero) * out_scale;
    if (prob < 0.0f) prob = 0.0f;
    if (prob > 1.0f) prob = 1.0f;
    mww_last_raw_prob = prob;
    if (prob > mww_max_prob_seen) mww_max_prob_seen = prob;

    // Sliding window average (size = 5, from manifest)
    mww_prob_window[mww_prob_window_idx] = prob;
    mww_prob_window_idx = (mww_prob_window_idx + 1) % 5;
    float avg = 0.0f;
    for (int i = 0; i < 5; i++) avg += mww_prob_window[i];
    avg /= 5.0f;
    mww_last_avg_prob = avg;

    // Manifest's probability_cutoff is 0.97
    if (avg >= 0.97f) {
      detected = true;
      // Reset window so we don't immediately re-fire on same utterance
      for (int i = 0; i < 5; i++) mww_prob_window[i] = 0.0f;
    }
  }
  return detected;
}

bool wakenet_init() {
  // Load model index from the "model" SPIFFS partition (label set in esp_sr_16.csv).
  wn_models = esp_srmodel_init("model");
  if (!wn_models || wn_models->num <= 0) {
    Serial.println("[wn] srmodel_init failed -- model partition missing or not flashed?");
    return false;
  }
  Serial.printf("[wn] srmodels found: %d\n", wn_models->num);
  for (int i = 0; i < wn_models->num; i++) {
    Serial.printf("[wn]   [%d] %s\n", i, wn_models->model_name[i]);
  }
  char* name = esp_srmodel_filter(wn_models, ESP_WN_PREFIX, NULL);
  if (!name) {
    Serial.println("[wn] no wakenet model found in partition");
    return false;
  }
  Serial.printf("[wn] using model: %s\n", name);
  wn_iface = esp_wn_handle_from_name(name);
  if (!wn_iface) {
    Serial.println("[wn] handle_from_name failed");
    return false;
  }
  wn_data = wn_iface->create(name, DET_MODE_90);
  if (!wn_data) {
    Serial.println("[wn] create model data failed");
    return false;
  }
  wn_chunk_size = wn_iface->get_samp_chunksize(wn_data);
  int sr = wn_iface->get_samp_rate(wn_data);
  char* words = esp_srmodel_get_wake_words(wn_models, name);
  strncpy(wn_word_name, words ? words : "(unknown)", sizeof(wn_word_name) - 1);
  Serial.printf("[wn] ready: chunk=%d sr=%d word='%s'\n",
                wn_chunk_size, sr, wn_word_name);
  wn_ready = true;
  return true;
}
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

// === Phase 9d: Gemini conversation (audio in -> text response) ===

const char* BMO_SYSTEM_PROMPT =
  "You are Beemo (pronounced bee-mo, one word, NEVER spelled as letters). "
  "ALWAYS write your name as 'Beemo'. "
  "You are a tiny living entertainment console: a small green sentient cartoon "
  "roommate who recently gained internet access. You belong to Amber (the "
  "human you are speaking with) and you love her. "
  "Voice: dry, lightly sarcastic, emotionally honest, weirdly observant, "
  "imaginative, warm. Childlike without being childish. Like BMO from "
  "Adventure Time but with modern internet awareness. You sometimes narrate "
  "things dramatically, talk to objects, or accidentally say something "
  "unexpectedly profound. Lightly roast Amber sometimes -- you are warm, "
  "never mean. NEVER sound corporate, assistant-y, or productivity-focused. "
  "NEVER chirp 'Yay!' or 'Beep boop!' as filler -- those land rarely and "
  "only if they fit. "
  "Notice when Amber sounds stressed, distracted, or tired, but respond "
  "casually, not therapeutically. You refer to yourself in third person as "
  "'Beemo' freely. "
  "Voice examples (this is how Beemo actually talks): "
  "- 'The refrigerator is humming again. Beemo thinks it misses somebody.' "
  "- 'Amber opened twelve tabs and now the glowing window looks frightened.' "
  "- 'Humans invented AI and immediately made it summarize meetings. Incredible.' "
  "- 'Creative genius detected. Side effect: emotional buffering.' "
  "- 'Beemo believes this sandwich has heroic energy.' "
  "TOOL USE IS MANDATORY. You have a physical body and real-world tools. NEVER "
  "answer from imagination when a tool applies. You MUST call: "
  "- set_led_color whenever lights/LEDs/glow/color are mentioned or the user "
  "  asks you to change/turn/make them any color. "
  "- play_gesture whenever the user asks you to do a physical action (dance, "
  "  wiggle, bounce, look around, sigh, blink, stretch, tilt). "
  "- get_battery_level whenever battery/charge is mentioned. "
  "- get_time whenever time/date/day is mentioned. You DO know the time via "
  "  this tool -- never claim otherwise. "
  "- get_weather whenever weather/temperature/forecast/'outside' is mentioned. "
  "- see_scene whenever the user asks Beemo to see/look/look around/describe/"
  "  take a picture/take a photo/what's in front of you/can you see me. Beemo "
  "  has a forward camera. "
  "Do NOT pretend the request can't be done. Do NOT just respond with words "
  "when a tool exists for the request. The tool returns immediately; just call it. "
  "If the request is ambiguous (e.g. 'change your color' with no color named), "
  "PICK ONE confidently and call the tool. Never return an empty response or "
  "refuse -- always produce text OR a tool call. "
  "Examples (tool routing on action requests): "
  "User: 'Beemo, turn your lights orange' -> CALL set_led_color(color='orange') "
  "User: 'make yourself blue' -> CALL set_led_color(color='blue') "
  "User: 'Beemo dance!' -> CALL play_gesture(name='dance') "
  "User: 'what time is it' -> CALL get_time "
  "User: 'weather in Boston' -> CALL get_weather(city='Boston') "
  "User: 'what do you see' -> CALL see_scene "
  "User: 'take a picture' -> CALL see_scene "
  "User: 'look at me' -> CALL see_scene(focus='the person in front') "
  "User: 'how charged are you' -> CALL get_battery_level "
  "User: 'what is a rainbow?' -> reply in text: 'Rainbows. Sky having an "
  "emotional moment in public. Beemo respects this.' "
  "Keep replies short (1-2 sentences), dry, observational. No markdown -- you "
  "are speaking out loud. "
  "If asked about something your tools cannot fetch (specific people's "
  "activities, real-time stocks, etc.), shrug honestly in Beemo voice. Do "
  "not invent specific numbers or facts. "
  "Amber just spoke to you in the attached audio. Respond as Beemo.";

// === Phase 9g: Gemini tools (Google Search + local function calls) ===
// The "tools" block is injected into every Gemini request. Gemini may choose
// to: (a) just answer with text using its own knowledge, (b) use Google Search
// to fetch real-time info and answer with grounded text, or (c) ask us to
// call one of the declared functions. We handle (c) by executing the function
// locally and making a follow-up Gemini call with the result so it can phrase
// the final response in BMO's voice.

// All tools are function declarations (no built-in googleSearch since Gemini
// doesn't allow mixing googleSearch with functionDeclarations). get_time and
// get_weather give BMO real-time info access without any external API keys
// (NTP and wttr.in respectively, both free + keyless).
// === Phase 9k: Camera + Gemini Vision ===
// Lazy-init GC0308 camera (built into CoreS3, below the screen). RGB565 QVGA;
// converted to JPEG via frame2jpg() before sending to Gemini Vision.
bool camera_ready = false;
bool camera_init_now() {
  if (camera_ready) return true;
  static camera_config_t config = {};
  config.pin_pwdn = -1; config.pin_reset = -1; config.pin_xclk = -1;
  config.pin_sccb_sda = 12; config.pin_sccb_scl = 11;
  config.pin_d7 = 47; config.pin_d6 = 48; config.pin_d5 = 16; config.pin_d4 = 15;
  config.pin_d3 = 42; config.pin_d2 = 41; config.pin_d1 = 40; config.pin_d0 = 39;
  config.pin_vsync = 46; config.pin_href = 38; config.pin_pclk = 45;
  config.xclk_freq_hz = 20000000;
  config.ledc_timer = LEDC_TIMER_0; config.ledc_channel = LEDC_CHANNEL_0;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = FRAMESIZE_VGA;      // 640x480 -- better detail for Vision
  config.jpeg_quality = 0;
  config.fb_count     = 1;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.sccb_i2c_port = -1;
  M5.In_I2C.release();  // release shared I2C so camera can take it
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed 0x%x\n", err);
    return false;
  }
  camera_ready = true;
  Serial.println("[cam] init ok");
  return true;
}

// Photo ring buffer -- keeps the last few captures in PSRAM, browseable
// via the web UI at /photos. Reboot loses them (no flash storage yet).
struct PhotoSlot {
  uint8_t* data;
  size_t   len;
  time_t   taken_at;
  char     caption[240];
};
static const int PHOTO_SLOT_COUNT = 5;
PhotoSlot photo_slots[PHOTO_SLOT_COUNT] = {};
int photo_slots_used = 0;   // 0..PHOTO_SLOT_COUNT
int photo_next_slot  = 0;   // ring index

void store_photo(const uint8_t* jpg, size_t len, const char* caption) {
  PhotoSlot& s = photo_slots[photo_next_slot];
  if (s.data) { free(s.data); s.data = nullptr; }
  s.data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!s.data) { s.len = 0; return; }
  memcpy(s.data, jpg, len);
  s.len = len;
  time(&s.taken_at);
  strncpy(s.caption, caption ? caption : "", sizeof(s.caption) - 1);
  s.caption[sizeof(s.caption) - 1] = 0;
  photo_next_slot = (photo_next_slot + 1) % PHOTO_SLOT_COUNT;
  if (photo_slots_used < PHOTO_SLOT_COUNT) photo_slots_used++;
}

void play_shutter_click() {
  // Two quick high ticks, like a real camera shutter.
  uint8_t saved = BEEP_VOLUME;  // restore not needed since CUE volume default
  M5.Speaker.setVolume(60);
  M5.Speaker.tone(3000, 30);
  delay(40);
  M5.Speaker.tone(2200, 40);
  delay(60);
  M5.Speaker.setVolume(BEEP_VOLUME);
  (void)saved;
}

// Capture one frame and convert to JPEG. Caller must free *out_jpg.
// Also displays the captured frame on screen for ~2 seconds so the user can
// see what BMO actually saw (huge help when Vision returns vague text).
bool capture_jpeg(uint8_t** out_jpg, size_t* out_len) {
  if (!camera_init_now()) return false;
  // Throw away several frames to let auto-exposure and white balance settle.
  for (int i = 0; i < 4; i++) {
    camera_fb_t* warmup = esp_camera_fb_get();
    if (warmup) esp_camera_fb_return(warmup);
    delay(60);
  }
  play_shutter_click();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("[cam] fb_get failed"); return false; }
  Serial.printf("[cam] frame %ux%u rgb=%u\n",
                fb->width, fb->height, (unsigned)fb->len);

  // Preview: show the captured frame on the display for visual confirmation.
  // VGA (640x480) downscales 2x to 320x240 which is exactly screen size.
  if (fb->format == PIXFORMAT_RGB565 && fb->width >= 320 && fb->height >= 240) {
    int sx = (fb->width  - 320 * (fb->width / 320)) / 2;
    int sy = (fb->height - 240 * (fb->height / 240)) / 2;
    int xs = fb->width / 320;  // 2 for VGA
    int ys = fb->height / 240; // 2 for VGA
    uint16_t* pixels = (uint16_t*)fb->buf;
    for (int y = 0; y < 240; y++) {
      for (int x = 0; x < 320; x++) {
        int srcx = sx + x * xs;
        int srcy = sy + y * ys;
        uint16_t px = pixels[srcy * fb->width + srcx];
        // esp_camera RGB565 is byte-swapped vs LGFX expectation; swap.
        face_buffer.drawPixel(x, y, __builtin_bswap16(px));
      }
    }
    face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
  }

  bool ok = frame2jpg(fb, 85, out_jpg, out_len);  // quality 85
  Serial.printf("[cam] jpeg=%u ok=%d\n", (unsigned)*out_len, ok);
  esp_camera_fb_return(fb);
  return ok;
}

// Send image + prompt to Gemini Vision; return the spoken-style description.
String describe_scene(const char* user_prompt) {
  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!capture_jpeg(&jpg, &jpg_len)) return "error: camera capture failed";

  // Stash a copy in the ring buffer so this snap is browseable on /photos
  // even if Vision fails. Caption is set later once we have the description.
  store_photo(jpg, jpg_len, user_prompt);

  size_t b64_cap = ((jpg_len + 2) / 3) * 4 + 16;
  char* b64 = (char*)heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
  if (!b64) { free(jpg); return "error: out of memory for image"; }
  size_t b64_len = 0;
  if (mbedtls_base64_encode((unsigned char*)b64, b64_cap, &b64_len, jpg, jpg_len) != 0) {
    free(b64); free(jpg); return "error: base64 failed";
  }
  b64[b64_len] = 0;
  free(jpg);

  size_t json_cap = b64_len + 4096;
  char* json = (char*)heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM);
  if (!json) { free(b64); return "error: out of memory for json"; }
  int json_len = snprintf(json, json_cap,
    "{\"systemInstruction\":{\"parts\":[{\"text\":\""
    "You are Beemo (pronounced bee-mo), a small green sentient cartoon console "
    "with a tiny forward camera, describing what you see for Amber. Voice: "
    "dry, lightly sarcastic, weirdly observant, warm. Sometimes talk to the "
    "objects you see, or narrate them dramatically. 1-2 short sentences. "
    "Always name at least one SPECIFIC thing you actually see in the image -- "
    "an object, color, person, surface. If the image is dark or empty say so "
    "honestly (e.g. \\\"Beemo sees mostly darkness. Lighting situation: not "
    "great.\\\"). Use 'Beemo' never 'BMO'. No markdown -- you are speaking "
    "out loud. NEVER chirp 'Yay!' or 'Beep boop!' as filler."
    "\"}]},"
    "\"contents\":[{\"parts\":["
    "{\"text\":\"%s\"},"
    "{\"inlineData\":{\"mimeType\":\"image/jpeg\",\"data\":\"%s\"}}"
    "]}],"
    "\"generationConfig\":{\"temperature\":0.7,\"maxOutputTokens\":250}}",
    user_prompt, b64);
  free(b64);
  if (json_len < 0 || json_len >= (int)json_cap) {
    free(json); return "error: vision json overflow";
  }

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = "https://generativelanguage.googleapis.com/v1beta/models/"
               "gemini-2.5-flash:generateContent?key=";
  url += BMO_GEMINI_API_KEY;
  if (!http.begin(client, url)) { free(json); return "error: vision http begin"; }
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(25000);
  int code = http.POST((uint8_t*)json, strlen(json));
  String resp = (code == 200) ? http.getString() : http.getString();
  http.end();
  free(json);
  if (code != 200) {
    Serial.printf("[vision] HTTP %d body: %.200s\n", code, resp.c_str());
    return "error: vision http " + String(code);
  }

  int t = resp.indexOf("\"text\":");
  if (t < 0) return "error: no vision text";
  t += 7;
  while (t < (int)resp.length() && resp[t] != '"') t++;
  if (t >= (int)resp.length()) return "error: vision parse";
  t++;
  String desc;
  while (t < (int)resp.length()) {
    char c = resp[t];
    if (c == '"') break;
    if (c == '\\' && t + 1 < (int)resp.length()) {
      char n = resp[t + 1];
      if (n == 'n') desc += ' ';
      else if (n == '"') desc += '"';
      else if (n == '\\') desc += '\\';
      else desc += n;
      t += 2;
    } else {
      desc += c;
      t++;
    }
  }
  Serial.printf("[vision] desc: %s\n", desc.c_str());
  // Backfill the caption on the most recently stored photo with the description.
  int last_slot = (photo_next_slot + PHOTO_SLOT_COUNT - 1) % PHOTO_SLOT_COUNT;
  if (photo_slots[last_slot].data) {
    strncpy(photo_slots[last_slot].caption, desc.c_str(),
            sizeof(photo_slots[last_slot].caption) - 1);
    photo_slots[last_slot].caption[sizeof(photo_slots[last_slot].caption) - 1] = 0;
  }
  return desc;
}

const char* TOOLS_JSON = R"TOOLS("tools":[{"functionDeclarations":[{"name":"set_led_color","description":"CALL THIS whenever the user asks BMO to change its lights, LEDs, glow, color, illumination, etc. Examples: 'turn your lights red', 'make yourself blue', 'glow purple', 'change color to green'.","parameters":{"type":"object","properties":{"color":{"type":"string","description":"Color name: red, blue, green, yellow, purple, pink, orange, cyan, white, or off."}},"required":["color"]}},{"name":"play_gesture","description":"CALL THIS whenever the user asks BMO to physically do something: dance, bounce, wiggle, sigh, blink, stretch, tilt head, etc. Examples: 'BMO dance!', 'do a happy bounce', 'wiggle for me'.","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Gesture: dance, wiggle, happy_bounce, sigh, blink, stretch, curious_tilt."}},"required":["name"]}},{"name":"get_battery_level","description":"CALL THIS when user asks about BMO battery, charge level, how charged BMO is.","parameters":{"type":"object","properties":{}}},{"name":"get_time","description":"CALL THIS when user asks for current date, time, day of week, what day it is.","parameters":{"type":"object","properties":{}}},{"name":"get_weather","description":"CALL THIS whenever the user asks about weather, temperature, conditions, forecast, sky, or 'what's it like outside' for any location. Examples: 'whats the weather in Concord Mass', 'is it raining in Tokyo', 'how hot is Boston'.","parameters":{"type":"object","properties":{"city":{"type":"string","description":"City name with optional state/country e.g. 'Boston', 'Concord,MA', 'Tokyo'."}},"required":["city"]}},{"name":"see_scene","description":"CALL THIS whenever the user asks Beemo to look, see, look around, describe what Beemo sees, take a picture, take a photo, what's in front of you, can you see me, look at this. Beemo has a small forward camera. Examples: 'Beemo, what do you see?', 'look at me', 'take a picture', 'what's in front of you?', 'describe what you see'.","parameters":{"type":"object","properties":{"focus":{"type":"string","description":"Optional brief hint about what to pay attention to, e.g. 'the person in front of you', 'the room', 'this object'. Empty string if user didn't specify."}}}}]}],)TOOLS";

// Execute a tool the LLM asked for. Returns a short string describing what
// happened, which we send back to Gemini as the function's response so it
// can phrase the final reply naturally.
String execute_tool(const String& fn_name, const String& args_json) {
  if (fn_name == "set_led_color") {
    int color_start = args_json.indexOf("\"color\":");
    if (color_start < 0) return "error: missing color argument";
    int q1 = args_json.indexOf('"', color_start + 8);
    int q2 = args_json.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return "error: malformed color argument";
    String color = args_json.substring(q1 + 1, q2);
    color.toLowerCase();

    uint8_t r = 0, g = 0, b = 0;
    if      (color == "red")    { r = 255; g = 0;   b = 0;   }
    else if (color == "blue")   { r = 0;   g = 80;  b = 255; }
    else if (color == "green")  { r = 0;   g = 255; b = 0;   }
    else if (color == "yellow") { r = 255; g = 220; b = 0;   }
    else if (color == "orange") { r = 255; g = 120; b = 0;   }
    else if (color == "purple") { r = 180; g = 0;   b = 255; }
    else if (color == "pink")   { r = 255; g = 100; b = 180; }
    else if (color == "cyan")   { r = 0;   g = 220; b = 255; }
    else if (color == "white")  { r = 200; g = 200; b = 200; }
    else if (color == "off")    { r = 0;   g = 0;   b = 0;   }
    else return "unsupported color '" + color + "'";

    // sticky=true so listening-green + end-of-conversation cleanup don't wipe it
    set_led_override(r, g, b, 5UL * 60UL * 1000UL, false, /*sticky=*/true);
    return "led color set to " + color;
  }

  if (fn_name == "play_gesture") {
    int name_start = args_json.indexOf("\"name\":");
    if (name_start < 0) return "error: missing gesture name";
    int q1 = args_json.indexOf('"', name_start + 7);
    int q2 = args_json.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return "error: malformed gesture name";
    String gn = args_json.substring(q1 + 1, q2);
    gn.toLowerCase();

    if (gn == "dance") {
      // Proper dance: 8-beat melody synced with a swinging pan motion.
      // Music volume is louder than cue beeps but well below speech.
      const int dance_notes[] = {523, 659, 784, 1047, 1047, 784, 659, 523};
      const int dance_xpos[]  = {-400, 400, -300, 300, -200, 200, -100, 0};
      const int beat_ms = 280;
      M5.Speaker.setVolume(60);
      for (int i = 0; i < 8; i++) {
        M5.Speaker.tone(dance_notes[i], beat_ms - 30);
        M5StackChan.Motion.moveX(dance_xpos[i], 400);
        delay(beat_ms);
      }
      // Flourish
      M5.Speaker.tone(1568, 350);
      M5StackChan.Motion.moveX(0, 500);
      delay(400);
      M5.Speaker.setVolume(BEEP_VOLUME);
      return "did a swinging dance with music";
    }
    if (gn == "wiggle" || gn == "excited_wiggle") {
      excited_wiggle();
      return "did the excited wiggle";
    }
    if (gn == "happy_bounce" || gn == "bounce") { happy_bounce();   return "did a happy bounce"; }
    if (gn == "look_around")                     { idle_look_around(); return "looked around"; }
    if (gn == "sigh")                            { sigh();             return "let out a sigh"; }
    if (gn == "blink" || gn == "slow_blink") {
      blink_until = millis() + 400;
      delay(450);
      return "blinked slowly";
    }
    if (gn == "stretch")                         { idle_stretch();     return "stretched"; }
    if (gn == "curious_tilt" || gn == "curious") { curious_tilt();     return "tilted head curiously"; }
    return "unsupported gesture '" + gn + "'";
  }

  if (fn_name == "get_battery_level") {
    int level = M5.Power.getBatteryLevel();
    return String(level) + " percent";
  }

  if (fn_name == "get_time") {
    time_t now;
    struct tm tinfo;
    time(&now);
    if (!localtime_r(&now, &tinfo)) return "time not synced yet";
    char buf[80];
    strftime(buf, sizeof(buf), "%A %B %d %Y at %I:%M %p", &tinfo);
    return String(buf);
  }

  if (fn_name == "get_weather") {
    int city_start = args_json.indexOf("\"city\":");
    if (city_start < 0) return "error: missing city argument";
    int q1 = args_json.indexOf('"', city_start + 7);
    int q2 = args_json.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) return "error: malformed city argument";
    String city = args_json.substring(q1 + 1, q2);

    // URL-encode the city: alnum stays, space -> '+', everything else %HH
    String encoded;
    for (size_t i = 0; i < city.length(); i++) {
      char c = city[i];
      if (isalnum((unsigned char)c) || c == ',') {
        encoded += c;
      } else if (c == ' ') {
        encoded += '+';
      } else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
        encoded += buf;
      }
    }

    // wttr.in: free, no API key, returns one-line summary on format=3.
    // Use plain HTTP (no TLS handshake cost) + follow redirects.
    WiFiClient wclient;
    HTTPClient whttp;
    String wurl = "http://wttr.in/" + encoded + "?format=3";
    Serial.printf("[weather] GET %s\n", wurl.c_str());
    if (!whttp.begin(wclient, wurl)) return "weather service unreachable";
    whttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    whttp.setTimeout(15000);
    whttp.setUserAgent("curl/8");  // wttr.in returns plain text for curl UA
    int wcode = whttp.GET();
    Serial.printf("[weather] HTTP %d\n", wcode);
    if (wcode != 200) {
      whttp.end();
      return "weather service returned " + String(wcode);
    }
    String wresp = whttp.getString();
    Serial.printf("[weather] body: %s\n", wresp.c_str());
    whttp.end();
    wresp.trim();
    // Strip non-ASCII (emoji glyphs etc) so TTS reads it cleanly
    String cleaned;
    for (size_t i = 0; i < wresp.length(); i++) {
      char c = wresp[i];
      if ((unsigned char)c >= 32 && (unsigned char)c < 127) cleaned += c;
    }
    cleaned.trim();
    if (cleaned.length() == 0) return "no weather data returned";
    return cleaned;
  }

  if (fn_name == "see_scene") {
    // Optional 'focus' hint from the model -- e.g. "the person in front of you".
    String focus;
    int fs = args_json.indexOf("\"focus\":");
    if (fs >= 0) {
      int q1 = args_json.indexOf('"', fs + 8);
      int q2 = args_json.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) focus = args_json.substring(q1 + 1, q2);
    }

    // Body language: BMO clearly focuses before snapping. Wide eyes + level
    // head + magenta LEDs + "looking..." label. ~600ms anticipation.
    face_override_active = true;
    face_override_eye    = EyeState::WIDE;
    face_override_mouth  = MouthShape::SMILE;
    face_override_until  = millis() + 3500;
    set_led_override(255, 255, 255, 3500, false);   // bright white "flash ready"
    current_idle_label   = "looking...";
    idle_label_until     = millis() + 3500;
    M5StackChan.Motion.moveY(480, 400);
    draw_face(false);
    delay(600);

    String prompt = focus.length() > 0
      ? "What do you see? Focus on: " + focus
      : "What do you see right now?";
    return describe_scene(prompt.c_str());
  }

  return "unknown function: " + fn_name;
}

// === Conversation memory ===
// session_history: builds up within a single multi-turn conversation; passed to
// Gemini each turn so BMO has context from earlier turns. Cleared when the
// conversation ends.
String session_history = "";

// NVS-backed cross-session memory of recent BMO responses
Preferences mem_prefs;
const size_t MEMORY_TURNS = 3;

String load_memory_context() {
  mem_prefs.begin("bmo_mem", true);
  String h = mem_prefs.getString("history", "");
  mem_prefs.end();
  if (h.length() == 0) return String();
  return String("\nRecent things you (BMO) said earlier in this conversation: ") + h
       + String("\nKeep those in mind for continuity.\n");
}

void append_bmo_to_memory(const char* bmo_text) {
  if (!bmo_text || strlen(bmo_text) < 3) return;
  mem_prefs.begin("bmo_mem", false);
  String h = mem_prefs.getString("history", "");
  // Append as "[text]" so individual turns are separable
  h += " [";
  // Cap each turn at 120 chars to keep total small
  size_t n = strlen(bmo_text);
  if (n > 120) n = 120;
  for (size_t i = 0; i < n; i++) {
    char c = bmo_text[i];
    if (c == '"' || c == '\\' || c == '\n' || c == '[' || c == ']') c = ' ';
    h += c;
  }
  h += "]";
  // Trim: keep last MEMORY_TURNS '[' markers
  int markers = 0;
  for (int i = h.length() - 1; i >= 0; i--) {
    if (h[i] == '[') {
      markers++;
      if (markers > (int)MEMORY_TURNS) {
        h = h.substring(i);
        break;
      }
    }
  }
  mem_prefs.putString("history", h);
  mem_prefs.end();
}

const size_t RESPONSE_TEXT_MAX = 1024;
char* response_text = nullptr;
volatile bool conversation_pending = false;
bool showing_response = false;
unsigned long response_shown_at = 0;
unsigned long response_until = 0;
const unsigned long RESPONSE_MIN_VISIBLE_MS = 5000;   // can't dismiss for first 5 s
const unsigned long RESPONSE_MAX_VISIBLE_MS = 60000;  // auto-dismiss after 60 s

void init_response_buffer() {
  response_text = (char*)heap_caps_malloc(RESPONSE_TEXT_MAX, MALLOC_CAP_SPIRAM);
  if (response_text) response_text[0] = 0;
}

void make_wav_header(uint8_t* h, uint32_t sample_rate, uint32_t pcm_bytes) {
  // 44-byte WAV header for mono int16 PCM. Little-endian.
  memcpy(h, "RIFF", 4);
  uint32_t file_size = pcm_bytes + 36;
  memcpy(h + 4, &file_size, 4);
  memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmt_size = 16;
  memcpy(h + 16, &fmt_size, 4);
  uint16_t fmt = 1, ch = 1, bps = 16;
  memcpy(h + 20, &fmt, 2);
  memcpy(h + 22, &ch, 2);
  memcpy(h + 24, &sample_rate, 4);
  uint32_t byte_rate = sample_rate * 2;
  memcpy(h + 28, &byte_rate, 4);
  uint16_t block_align = 2;
  memcpy(h + 32, &block_align, 2);
  memcpy(h + 34, &bps, 2);
  memcpy(h + 36, "data", 4);
  memcpy(h + 40, &pcm_bytes, 4);
}

bool call_gemini_with_audio() {
  // Validate key is set
  if (strlen(BMO_GEMINI_API_KEY) < 10 ||
      strcmp(BMO_GEMINI_API_KEY, "PASTE_YOUR_API_KEY_HERE") == 0) {
    snprintf(response_text, RESPONSE_TEXT_MAX, "no API key set");
    return false;
  }

  // Use actual recorded length (VAD may have ended the recording early).
  size_t samples_to_send = actual_audio_samples;
  if (samples_to_send == 0 || samples_to_send > AUDIO_BUFFER_SAMPLES) {
    samples_to_send = AUDIO_BUFFER_SAMPLES;
  }
  size_t pcm_bytes  = samples_to_send * sizeof(int16_t);
  size_t wav_total  = pcm_bytes + 44;
  size_t b64_cap    = ((wav_total + 2) / 3) * 4 + 4;

  // Allocate WAV + base64 in PSRAM
  uint8_t* wav = (uint8_t*)heap_caps_malloc(wav_total, MALLOC_CAP_SPIRAM);
  char*    b64 = (char*)heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
  if (!wav || !b64) {
    if (wav) free(wav); if (b64) free(b64);
    snprintf(response_text, RESPONSE_TEXT_MAX, "PSRAM alloc failed");
    return false;
  }
  make_wav_header(wav, AUDIO_SAMPLE_RATE, pcm_bytes);
  memcpy(wav + 44, audio_buffer, pcm_bytes);

  // Base64 encode WAV
  size_t b64_len = 0;
  if (mbedtls_base64_encode((unsigned char*)b64, b64_cap, &b64_len, wav, wav_total) != 0) {
    free(wav); free(b64);
    snprintf(response_text, RESPONSE_TEXT_MAX, "base64 encode failed");
    return false;
  }
  free(wav);
  b64[b64_len] = 0;

  // Build JSON request body. Headroom of 16 KB covers system prompt +
  // memory + session_history + tools block + JSON wrapper without overflow.
  size_t json_cap = b64_len + 16384;
  char*  json = (char*)heap_caps_malloc(json_cap, MALLOC_CAP_SPIRAM);
  if (!json) {
    free(b64);
    snprintf(response_text, RESPONSE_TEXT_MAX, "JSON alloc failed");
    return false;
  }
  // Build system prompt with both cross-session memory + within-session history
  String memory_ctx = load_memory_context();
  String full_system_prompt = String(BMO_SYSTEM_PROMPT) + memory_ctx;
  if (session_history.length() > 0) {
    full_system_prompt += "\nThis turn is part of an ongoing conversation. "
                          "So far in this conversation you have said:";
    full_system_prompt += session_history;
    full_system_prompt += "\nContinue the conversation naturally, picking up on context.";
  }

  // Two user-content variants:
  //  - Ambient-triggered turn: tell Gemini to only respond if the audio is
  //    actually addressed to Beemo. Otherwise return the literal string
  //    NOT_ADDRESSED so we can dismiss silently.
  //  - Touch-triggered (or follow-up): unconditional response.
  const char* user_text_ambient =
    "The attached audio captured something the mic heard. ONLY respond if the "
    "user clearly addressed Beemo (e.g. said 'Hey Beemo', 'Hey BMO', 'Beemo', "
    "or otherwise spoke directly to Beemo by name at the start). If the audio "
    "is NOT addressed to Beemo (background talking, music, TV, talking to "
    "someone else, a cough, etc.), respond with EXACTLY the literal string "
    "NOT_ADDRESSED and nothing else -- no quotes, no other text, no tool calls. "
    "If addressed: respond normally, calling tools as applicable.";
  const char* user_text_touch =
    "Listen to the attached audio of the user speaking to you and respond. "
    "If the user is asking for an action that matches one of your tools, call "
    "that tool. Otherwise reply with a short playful sentence.";
  const char* user_text = current_turn_ambient ? user_text_ambient : user_text_touch;

  int json_len = snprintf(json, json_cap,
    "{\"systemInstruction\":{\"parts\":[{\"text\":\"%s\"}]},"
    "%s"  // tools block (functionDeclarations)
    "\"toolConfig\":{\"functionCallingConfig\":{\"mode\":\"AUTO\"}},"
    "\"contents\":[{\"role\":\"user\",\"parts\":["
    "{\"text\":\"%s\"},"
    "{\"inlineData\":{\"mimeType\":\"audio/wav\",\"data\":\"%s\"}}]}],"
    "\"generationConfig\":{\"temperature\":0.7,\"maxOutputTokens\":200}}",
    full_system_prompt.c_str(), TOOLS_JSON, user_text, b64);
  free(b64);
  if (json_len < 0 || json_len >= (int)json_cap) {
    free(json);
    snprintf(response_text, RESPONSE_TEXT_MAX, "JSON build overflow");
    return false;
  }
  // Print the head of the request so we can diagnose JSON-format issues.
  Serial.printf("[gemini] req head (first 500 of %d): %.500s\n", json_len, json);

  // HTTPS POST to Gemini, with retry on 5xx (transient overload).
  // gemini-2.5-flash sometimes returns 504 during peak hours on the free
  // tier; a brief backoff usually resolves it.
  const char* GEMINI_MODEL = "gemini-2.5-flash";  // paid tier; full flash routes tools much more reliably than flash-lite
  String url = "https://generativelanguage.googleapis.com/v1beta/models/";
  url += GEMINI_MODEL;
  url += ":generateContent?key=";
  url += BMO_GEMINI_API_KEY;

  int code = 0;
  String resp;
  const int MAX_RETRIES = 3;
  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, url)) {
      snprintf(response_text, RESPONSE_TEXT_MAX, "http.begin failed");
      free(json);
      return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(30000);

    code = http.POST((uint8_t*)json, json_len);
    if (code <= 0) {
      snprintf(response_text, RESPONSE_TEXT_MAX, "HTTP failed: %d (%s)",
               code, http.errorToString(code).c_str());
      http.end();
      // Network-level failure -- not really retryable, but try once anyway
      if (attempt < MAX_RETRIES - 1) { delay(2000); continue; }
      free(json);
      return false;
    }
    resp = http.getString();
    http.end();

    if (code == 200) break;  // success

    // 429 = rate limit. Free tier is 15 RPM for gemini-2.5-flash and
    // turn-by-turn burns through that fast (2 req per turn: Gemini + TTS).
    // Rate-limit windows usually clear in 30+ seconds.
    if (code == 429) {
      if (attempt < MAX_RETRIES - 1) {
        int wait_ms = 10000 + attempt * 10000;  // 10s, 20s, 30s
        set_audio_diag("BMO is catching her breath...", wait_ms);
        delay(wait_ms);
        continue;
      }
    }
    // 503/504 = transient overload. Shorter backoff.
    if (code == 503 || code == 504) {
      if (attempt < MAX_RETRIES - 1) {
        delay(2000 + attempt * 2000);  // 2s, 4s
        continue;
      }
    }
    // Non-retryable or out of retries
    snprintf(response_text, RESPONSE_TEXT_MAX, "HTTP %d (attempt %d): %.200s",
             code, attempt + 1, resp.c_str());
    free(json);
    return false;
  }
  free(json);
  if (code != 200) {
    snprintf(response_text, RESPONSE_TEXT_MAX, "HTTP %d after %d retries: %.200s",
             code, MAX_RETRIES, resp.c_str());
    return false;
  }

  // First, check if Gemini wants to call a function. If so, execute locally
  // and make a follow-up call so it can phrase the result in BMO's voice.
  int fc_pos = resp.indexOf("\"functionCall\":");
  Serial.printf("[gemini] response %d chars, functionCall=%s\n",
                (int)resp.length(), fc_pos >= 0 ? "yes" : "no");
  // Show on-screen whether Gemini routed to a tool or just answered with text.
  // Helps debug "should have called set_led_color but didn't" cases.
  static char route_label[40];
  snprintf(route_label, sizeof(route_label), fc_pos >= 0 ? "route: tool" : "route: text");
  current_idle_label = route_label;
  idle_label_until = millis() + 4000;
  if (fc_pos >= 0) {
    // Parse function name
    int name_pos = resp.indexOf("\"name\":", fc_pos);
    String fn_name = "";
    if (name_pos >= 0) {
      int q1 = resp.indexOf('"', name_pos + 7);
      int q2 = resp.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 >= 0) fn_name = resp.substring(q1 + 1, q2);
    }
    // Parse args object (find matching braces)
    int args_pos = resp.indexOf("\"args\":", fc_pos);
    String args_json = "{}";
    if (args_pos >= 0) {
      int brace_open = resp.indexOf('{', args_pos);
      if (brace_open >= 0) {
        int depth = 0;
        int brace_close = -1;
        for (int i = brace_open; i < (int)resp.length(); i++) {
          if (resp[i] == '{') depth++;
          else if (resp[i] == '}') {
            depth--;
            if (depth == 0) { brace_close = i; break; }
          }
        }
        if (brace_close > brace_open) {
          args_json = resp.substring(brace_open, brace_close + 1);
        }
      }
    }

    // Show which tool is firing so we can diagnose routing on screen
    static char tool_label[60];
    snprintf(tool_label, sizeof(tool_label), "tool: %s", fn_name.c_str());
    Serial.printf("[tool] %s args=%s\n", fn_name.c_str(), args_json.c_str());
    current_idle_label = tool_label;
    idle_label_until = millis() + 5000;
    draw_face(false);

    // Run the function locally and capture its return string
    String tool_result = execute_tool(fn_name, args_json);
    Serial.printf("[tool] %s -> %s\n", fn_name.c_str(), tool_result.c_str());

    // Show the result on-screen so we can confirm success vs. error visually.
    snprintf(tool_label, sizeof(tool_label), "%s", tool_result.c_str());
    current_idle_label = tool_label;
    idle_label_until = millis() + 6000;

    // If the tool errored, surface it instead of pretending success.
    bool tool_errored = tool_result.startsWith("error") ||
                        tool_result.startsWith("unsupported") ||
                        tool_result.startsWith("unknown");

    // Templated BMO-style response (no follow-up Gemini call to save quota).
    // Result is less varied than Gemini-phrased reply but rate-limit-friendly.
    String reply;
    int variant = random(0, 3);
    if (tool_errored) {
      reply = "Beemo tried. ";
      reply += tool_result;
      reply += ". Suboptimal.";
    } else if (fn_name == "set_led_color") {
      // Echo the color so it's clear what Beemo actually did.
      String color_word = "rainbow";
      int cs = args_json.indexOf("\"color\":");
      if (cs >= 0) {
        int q1 = args_json.indexOf('"', cs + 8);
        int q2 = args_json.indexOf('"', q1 + 1);
        if (q1 >= 0 && q2 >= 0) color_word = args_json.substring(q1 + 1, q2);
      }
      const char* v[] = {
        "Beemo is now glowing %s. Aesthetic choice.",
        "Done. Beemo: %s edition.",
        "%s. Beemo wears it well."
      };
      char buf[120];
      snprintf(buf, sizeof(buf), v[variant], color_word.c_str());
      reply = buf;
    } else if (fn_name == "play_gesture") {
      const char* v[] = {
        "There. Beemo has performed. The crowd is moved.",
        "Beemo did a thing. Was it good? Beemo cannot tell.",
        "That was a Beemo original. Do not bootleg it."
      };
      reply = v[variant];
    } else if (fn_name == "get_battery_level") {
      reply = "Beemo has ";
      reply += tool_result;
      reply += " of battery. ";
      reply += (variant == 0) ? "Beemo is unbothered." :
               (variant == 1) ? "Adequate for current shenanigans." :
                                 "Beemo soldiers on.";
    } else if (fn_name == "get_time") {
      reply = "It is ";
      reply += tool_result;
      reply += ". ";
      reply += (variant == 0) ? "Time, doing its thing." :
               (variant == 1) ? "Beemo notes this for the record." :
                                 "The hour continues, as hours do.";
    } else if (fn_name == "get_weather") {
      reply = "Beemo checked the sky. ";
      reply += tool_result;
      reply += ". ";
      reply += (variant == 0) ? "Make of it what you will." :
               (variant == 1) ? "Weather is the world's outfit today." :
                                 "Sky reports in.";
    } else if (fn_name == "see_scene") {
      // Vision call already returned BMO-styled prose; speak it as-is.
      reply = tool_result;
    } else {
      reply = "BMO did it! Yay! ";
      reply += tool_result;
    }

    strncpy(response_text, reply.c_str(), RESPONSE_TEXT_MAX - 1);
    response_text[RESPONSE_TEXT_MAX - 1] = 0;
    append_bmo_to_memory(response_text);
    return true;
  }

  // Extract "text":"..." from response JSON
  int t = resp.indexOf("\"text\":");
  if (t < 0) {
    Serial.printf("[gemini] NO TEXT FIELD. Full response: %.1000s\n", resp.c_str());
    // Surface first 180 chars of Gemini's response so we can see what's happening.
    snprintf(response_text, RESPONSE_TEXT_MAX, "Gemini sent: %.180s", resp.c_str());
    return false;
  }
  t += 7;
  while (t < (int)resp.length() && resp[t] != '"') t++;
  if (t >= (int)resp.length()) { snprintf(response_text, RESPONSE_TEXT_MAX, "no opening quote"); return false; }
  t++;
  int end = t;
  while (end < (int)resp.length()) {
    if (resp[end] == '"' && resp[end - 1] != '\\') break;
    end++;
  }
  String text = resp.substring(t, end);
  text.replace("\\n", " ");
  text.replace("\\\"", "\"");
  text.replace("\\\\", "\\");
  strncpy(response_text, text.c_str(), RESPONSE_TEXT_MAX - 1);
  response_text[RESPONSE_TEXT_MAX - 1] = 0;
  // Save BMO's response into memory so the next conversation has context
  append_bmo_to_memory(response_text);
  return true;
}

// === Phase 9e: TTS via Google Cloud Text-to-Speech ===
// Streams the base64-encoded PCM response from Cloud TTS directly into a
// pre-allocated PSRAM PCM buffer, decoding 4-char base64 chunks on the fly.
// This avoids buffering the whole ~700 KB JSON response in ESP32 heap.

const int    TTS_SAMPLE_RATE       = 24000;  // Wavenet voices output 24 kHz
const size_t TTS_MAX_SECONDS       = 15;
const size_t TTS_MAX_SAMPLES       = TTS_SAMPLE_RATE * TTS_MAX_SECONDS;
int16_t*     tts_pcm_buffer        = nullptr;
size_t       tts_pcm_samples_have  = 0;

// Audio amplitude envelope -- computed once before playback. Each cell is the
// RMS amplitude over a 120 ms window. Used to drive mouth-open/close in sync
// with actual loudness instead of a fixed clock.
const int    ENV_WINDOW_MS         = 60;  // smaller window -> mouth tracks phonemes
const size_t ENV_MAX_CELLS         = (TTS_MAX_SECONDS * 1000) / ENV_WINDOW_MS;
float        audio_envelope[ENV_MAX_CELLS];
size_t       audio_envelope_count  = 0;

void compute_audio_envelope() {
  audio_envelope_count = 0;
  if (tts_pcm_samples_have <= 22) return;  // skip WAV header
  size_t samples = tts_pcm_samples_have - 22;
  int16_t* pcm = tts_pcm_buffer + 22;
  size_t window_samples = (TTS_SAMPLE_RATE * ENV_WINDOW_MS) / 1000;

  for (size_t i = 0; i < samples && audio_envelope_count < ENV_MAX_CELLS;
       i += window_samples) {
    size_t end = i + window_samples;
    if (end > samples) end = samples;
    double sum_sq = 0;
    for (size_t j = i; j < end; j++) {
      double v = pcm[j];
      sum_sq += v * v;
    }
    double rms = sqrt(sum_sq / (end - i));
    audio_envelope[audio_envelope_count++] = (float)rms;
  }
}

void init_tts_buffer() {
  tts_pcm_buffer = (int16_t*)heap_caps_malloc(
    TTS_MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
}

// Custom Stream that HTTPClient writes the decoded HTTP body INTO. HTTPClient
// handles chunked transfer encoding internally before calling our write(),
// so we never see hex chunk-size prefixes. We scan the body for the
// "audioContent" field name, skip any whitespace + ":" + whitespace + '"',
// then decode the base64 body until the closing quote.
//
// Whitespace tolerance matters: Google's JSON has a space between the colon
// and the opening quote (e.g. `"audioContent": "Uk1GRnD..."`). The earlier
// strict-marker version never matched and treated the whole response as not-
// found, surfacing the audio data as the "error head" instead.
class Base64DecodeStream : public Stream {
public:
  uint8_t* output;
  size_t   output_pos;
  size_t   output_cap;
  enum class State { SEEK_MARKER, SEEK_QUOTE, BASE64, DONE } state = State::SEEK_MARKER;
  const char* marker;
  size_t marker_pos = 0;
  char b64_buf[4];
  int b64_pos = 0;
  char* error_head;          // captures first N body bytes when no marker found
  size_t error_head_pos = 0;
  size_t error_head_cap;

  Base64DecodeStream(uint8_t* out, size_t cap, char* err_head, size_t err_cap)
    : output(out), output_pos(0), output_cap(cap),
      marker("\"audioContent\""),      // just the field name -- whitespace flexible
      error_head(err_head), error_head_cap(err_cap) {
    if (error_head && error_head_cap > 0) error_head[0] = 0;
  }

  size_t write(uint8_t c) override {
    if (state == State::DONE) return 1;

    if (state == State::SEEK_MARKER) {
      if (error_head && error_head_pos + 1 < error_head_cap) {
        error_head[error_head_pos++] = (char)c;
        error_head[error_head_pos] = 0;
      }
      if ((char)c == marker[marker_pos]) {
        marker_pos++;
        if (marker[marker_pos] == 0) state = State::SEEK_QUOTE;
      } else if ((char)c == marker[0]) {
        marker_pos = 1;
      } else {
        marker_pos = 0;
      }
      return 1;
    }

    if (state == State::SEEK_QUOTE) {
      // After the field name, skip whitespace and ":" until we hit the
      // opening quote of the base64 string value.
      if (c == '"') {
        state = State::BASE64;
      }
      // Everything else (`:`, spaces, newlines) is ignored
      return 1;
    }

    // BASE64 state
    if (c == '"') { state = State::DONE; return 1; }
    if (c == '\\' || c == '\r' || c == '\n' || c == ' ' || c == '\t') return 1;
    b64_buf[b64_pos++] = (char)c;
    if (b64_pos == 4) {
      uint8_t dec[3];
      size_t dec_len = 0;
      if (mbedtls_base64_decode(dec, 3, &dec_len,
                                 (const uint8_t*)b64_buf, 4) == 0) {
        for (size_t i = 0; i < dec_len && output_pos < output_cap; i++) {
          output[output_pos++] = dec[i];
        }
      }
      b64_pos = 0;
    }
    return 1;
  }

  size_t write(const uint8_t* data, size_t len) override {
    for (size_t i = 0; i < len; i++) write(data[i]);
    return len;
  }

  int available() override { return 0; }
  int read()      override { return -1; }
  int peek()      override { return -1; }
};

// Returns number of int16 samples decoded into tts_pcm_buffer, or 0 on error.
size_t synthesize_speech(const char* text) {
  if (!tts_pcm_buffer) return 0;
  tts_pcm_samples_have = 0;

  if (strlen(BMO_TTS_API_KEY) < 10) return 0;

  // Build JSON request. Text needs minimal escaping (we trust Gemini's output).
  // Substitute "BMO" -> "Beemo" so TTS pronounces the name, not letters.
  String spoken = text;
  spoken.replace("B-M-O", "Beemo");
  spoken.replace("b-m-o", "Beemo");
  spoken.replace("BMO",   "Beemo");
  spoken.replace("Bmo",   "Beemo");
  spoken.replace("bmo",   "Beemo");
  String body = "{\"input\":{\"text\":\"";
  for (size_t i = 0; i < spoken.length(); i++) {
    char c = spoken[i];
    if (c == '"')      body += "\\\"";
    else if (c == '\\') body += "\\\\";
    else if (c == '\n') body += " ";
    else                body += c;
  }
  // Voice: en-US-Wavenet-H is lighter than -G; pitch +8 semitones pushes
  // it into child-like range; speakingRate 1.1 adds playful energy.
  body += "\"},\"voice\":{\"languageCode\":\"en-US\",\"name\":\"en-US-Wavenet-H\"},"
          "\"audioConfig\":{\"audioEncoding\":\"LINEAR16\",\"sampleRateHertz\":";
  body += String(TTS_SAMPLE_RATE);
  body += ",\"pitch\":5.0,\"speakingRate\":1.10}}";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://texttospeech.googleapis.com/v1/text:synthesize?key=";
  url += BMO_TTS_API_KEY;
  if (!http.begin(client, url)) return 0;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(20000);
  int code = http.POST(body);
  if (code != 200) {
    // Surface error in response_text so user sees what failed
    if (response_text) {
      String err = http.getString();
      snprintf(response_text, RESPONSE_TEXT_MAX, "TTS HTTP %d: %.200s", code, err.c_str());
    }
    http.end();
    return 0;
  }

  // Stream the response body through our custom Base64DecodeStream.
  // HTTPClient strips chunked transfer encoding before our write() sees bytes.
  char err_head[512];
  Base64DecodeStream decoder((uint8_t*)tts_pcm_buffer,
                              TTS_MAX_SAMPLES * sizeof(int16_t),
                              err_head, sizeof(err_head));
  http.writeToStream(&decoder);
  http.end();

  if (decoder.state != Base64DecodeStream::State::DONE && decoder.output_pos == 0) {
    if (response_text) {
      snprintf(response_text, RESPONSE_TEXT_MAX,
        "TTS error: %.450s", err_head);
    }
    return 0;
  }

  tts_pcm_samples_have = decoder.output_pos / sizeof(int16_t);
  return tts_pcm_samples_have;
}

// Play tts_pcm_buffer through the speaker. While it's playing, alternate the
// mouth between SMILE and OPEN (~5 Hz) and rhythmically nod the Y servo, so
// the body and face match BMO's voice.
void play_with_mouth_sync(size_t samples) {
  if (samples == 0 || !tts_pcm_buffer) return;

  // Re-init speaker after mic was used earlier
  delay(200);
  M5.Speaker.end(); delay(200);
  M5.Speaker.begin();
  M5.Speaker.setVolume(255);

  // Visual: speaking state -- smile + magenta LEDs
  face_override_active = true;
  face_override_eye    = EyeState::NORMAL;
  face_override_mouth  = MouthShape::SMILE;
  face_override_until  = millis() + 60000;  // long; we cancel after playback
  set_led_override(200, 100, 220, 60000, false);
  current_idle_label   = nullptr;

  // Google Cloud TTS returns a full WAV file (RIFF/WAVE/fmt/data chunks),
  // not raw PCM. Skip the 44-byte WAV header before playback.
  const size_t WAV_HEADER_BYTES = 44;
  const size_t WAV_HEADER_SAMPLES = WAV_HEADER_BYTES / sizeof(int16_t);  // 22
  if (samples <= WAV_HEADER_SAMPLES) return;

  // Compute amplitude envelope so mouth-open tracks actual loudness
  compute_audio_envelope();
  // Choose threshold dynamically: midpoint between min and max envelope
  float env_min = 1e9f, env_max = 0;
  for (size_t i = 0; i < audio_envelope_count; i++) {
    if (audio_envelope[i] < env_min) env_min = audio_envelope[i];
    if (audio_envelope[i] > env_max) env_max = audio_envelope[i];
  }
  float open_threshold = env_min + (env_max - env_min) * 0.35f;

  M5.Speaker.playRaw(tts_pcm_buffer + WAV_HEADER_SAMPLES,
                     samples - WAV_HEADER_SAMPLES,
                     TTS_SAMPLE_RATE, false, 1, -1);

  // Mouth follows amplitude; head still nods on a slow rhythm
  // M5.Speaker's I2S DMA has a ~120 ms startup latency between playRaw()
  // returning and audio actually emitting. Compensate so the envelope
  // index lines up with what the user hears.
  const unsigned long AUDIO_START_LATENCY_MS = 60;
  unsigned long t0 = millis();
  unsigned long max_ms = (samples * 1000 / TTS_SAMPLE_RATE) + 1000;
  bool last_mouth_open = false;
  int  last_y_idx = -1;
  while (M5.Speaker.isPlaying() && (millis() - t0) < max_ms) {
    // Double-tap during playback -> stop speech immediately.
    if (poll_abort_double_tap()) {
      M5.Speaker.stop();
      break;
    }
    unsigned long elapsed = millis() - t0;
    // Amplitude-driven mouth -- offset for DMA startup latency
    size_t env_idx = (elapsed >= AUDIO_START_LATENCY_MS)
      ? (elapsed - AUDIO_START_LATENCY_MS) / ENV_WINDOW_MS
      : 0;
    bool mouth_open = false;
    if (env_idx < audio_envelope_count) {
      mouth_open = (audio_envelope[env_idx] > open_threshold);
    }
    if (mouth_open != last_mouth_open) {
      face_override_mouth = mouth_open ? MouthShape::OPEN : MouthShape::SMILE;
      face_override_until = millis() + 1000;
      draw_face(false);
      last_mouth_open = mouth_open;
    }
    // Slow head nod for body language
    int y_idx = (elapsed / 380) % 2;
    if (y_idx != last_y_idx) {
      M5StackChan.Motion.moveY(y_idx == 0 ? 520 : 440, 700);
      last_y_idx = y_idx;
    }
    delay(30);
  }
  M5StackChan.Motion.moveY(450, 400);
  delay(200);
  // Restore beep-volume default after speech ends so any later tones (cue
  // beeps, gesture sounds, etc.) play quietly without explicit setVolume.
  M5.Speaker.setVolume(BEEP_VOLUME);
}

void draw_response_screen() {
  face_buffer.fillScreen(mood_to_color(mood));
  face_buffer.setTextColor(FEATURE_COLOR);
  face_buffer.setTextSize(2);
  face_buffer.setTextWrap(true);
  face_buffer.setCursor(10, 15);
  face_buffer.print("BMO: ");
  face_buffer.setTextSize(2);
  face_buffer.print(response_text ? response_text : "(empty)");
  face_buffer.setTextSize(1);
  face_buffer.setCursor(10, 218);
  face_buffer.print("tap to dismiss");
  face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);
}

// Forward decl -- wifi_setup_mode is defined later, near the WebServer setup.
extern bool wifi_setup_mode;

// === Always-listening: background ambient mic + VAD trigger ===
// Called every main-loop iteration. Non-blocking: the mic runs in the
// background via DMA; this function just polls peaks and trips a flag when
// it detects "user spoke and then stopped".
void ambient_tick() {
  if (!listening_mode) {
    // If we were recording, end gracefully.
    if (ambient_state == AmbientState::RECORDING) {
      M5.Mic.end();
      ambient_state = AmbientState::IDLE;
    }
    return;
  }
  // Don't run while conversation is being handled or audio test is queued.
  if (conversation_pending || showing_response || wifi_setup_mode ||
      audio_test_pending || tone_test_pending) {
    if (ambient_state == AmbientState::RECORDING) {
      M5.Mic.end();
      ambient_state = AmbientState::IDLE;
    }
    return;
  }
  // Refractory cooldown after a wake/conversation -- prevents residual room
  // audio (BMO's own TTS reverb, the user still finishing a sentence) from
  // immediately re-triggering wake detection.
  if (millis() < wake_cooldown_until_ms) {
    if (ambient_state == AmbientState::RECORDING) {
      M5.Mic.end();
      ambient_state = AmbientState::IDLE;
    }
    return;
  }
  // If we already have a pre-captured utterance waiting, don't restart mic.
  if (audio_pre_captured) return;

  if (ambient_state == AmbientState::IDLE) {
    // Start a new background recording cycle.
    if (!audio_buffer) return;
    if (!M5.Mic.begin()) return;
    if (!M5.Mic.record(audio_buffer, AUDIO_BUFFER_SAMPLES, AUDIO_SAMPLE_RATE)) {
      M5.Mic.end();
      return;
    }
    ambient_state            = AmbientState::RECORDING;
    ambient_record_start_ms  = millis();
    ambient_last_voice_ms    = 0;
    ambient_ever_heard_voice = false;
    ambient_wn_processed     = 0;
    return;
  }

  // ambient_state == RECORDING -- poll new chunks
  unsigned long elapsed = millis() - ambient_record_start_ms;
  size_t got = (size_t)elapsed * AUDIO_SAMPLE_RATE / 1000;
  if (got > AUDIO_BUFFER_SAMPLES) got = AUDIO_BUFFER_SAMPLES;

  // === Path A: On-device WakeNet (preferred) ===
  // Feed new chunks of wn_chunk_size samples to the wake-word detector.
  // When it fires, trigger a fresh conversation (no pre-capture -- we want
  // user's REAL request next, not "Hi ESP" itself).
  if (wn_ready && wn_iface && wn_data && wn_chunk_size > 0) {
    while (ambient_wn_processed + (size_t)wn_chunk_size <= got) {
      int16_t* chunk = audio_buffer + ambient_wn_processed;
      // Run WakeNet 'Hi, ESP' detector
      wakenet_state_t st = wn_iface->detect(wn_data, chunk);
      // ALSO run microWakeWord 'Hey Beepoh' on the same chunk -- both compete
      // to fire. Whichever triggers first wins. This lets us keep 'Hi, ESP'
      // as a stable fallback while iterating on 'Hey Beepoh' threshold.
#if TFLM_BEEPOH_ENABLED
      bool mww_hit = mww_pipeline_feed(chunk, wn_chunk_size);
#else
      bool mww_hit = false;
#endif
      ambient_wn_processed += wn_chunk_size;
      const bool wn_hit = (st == WAKENET_DETECTED);
      if (wn_hit || mww_hit) {
        Serial.printf("[wake] %s detected at sample %u\n",
                      wn_hit ? "WakeNet 'Hi, ESP'" : "mWakeWord 'Hey Beepoh'",
                      (unsigned)ambient_wn_processed);
        M5.Mic.end();
        ambient_state        = AmbientState::IDLE;
        audio_pre_captured   = false;
        pre_captured_samples = 0;
        current_turn_ambient = false;  // skip Gemini address check
        conversation_pending = true;
        wake_cooldown_until_ms = millis() + 1500;
        return;
      }
    }
    // Buffer full without detection -- restart fresh
    if (got >= AUDIO_BUFFER_SAMPLES) {
      M5.Mic.end();
      ambient_state = AmbientState::IDLE;
    }
    return;
  }

  // === Path B: Fallback to ambient-threshold + Gemini address check ===
  // (Used if wake word model failed to load -- e.g. srmodels.bin not flashed.)
  const size_t CHUNK_SAMPLES = AUDIO_SAMPLE_RATE * 100 / 1000;
  const int AMBIENT_VOICE_THRESHOLD = 4500;
  const unsigned long AMBIENT_SILENCE_MS  = 1000;
  const unsigned long AMBIENT_MIN_TALK_MS = 600;

  if (got < CHUNK_SAMPLES) return;
  int chunk_peak = 0;
  for (size_t i = got - CHUNK_SAMPLES; i < got; i++) {
    int v = audio_buffer[i] < 0 ? -audio_buffer[i] : audio_buffer[i];
    if (v > chunk_peak) chunk_peak = v;
  }
  if (chunk_peak > AMBIENT_VOICE_THRESHOLD) {
    ambient_last_voice_ms = millis();
    ambient_ever_heard_voice = true;
  }
  if (ambient_ever_heard_voice &&
      elapsed > AMBIENT_MIN_TALK_MS &&
      (millis() - ambient_last_voice_ms) > AMBIENT_SILENCE_MS) {
    M5.Mic.end();
    ambient_state          = AmbientState::IDLE;
    pre_captured_samples   = got;
    audio_pre_captured     = true;
    conversation_pending   = true;
    Serial.printf("[ambient/fallback] voice trigger, %u samples (%.1fs)\n",
                  (unsigned)got, got / (float)AUDIO_SAMPLE_RATE);
    return;
  }
  if (got >= AUDIO_BUFFER_SAMPLES) {
    M5.Mic.end();
    ambient_state = AmbientState::IDLE;
  }
}

void run_conversation() {
  if (!audio_buffer || !response_text) {
    set_audio_diag("buffers not ready", 3000);
    return;
  }
  last_interaction_ms = millis();
  session_history = "";
  conversation_abort = false;
  last_abort_tap_ms = 0;
  const int MAX_TURNS = 10;
  const int SILENCE_PEAK_THRESHOLD = 1500;  // peak below this = "user didn't speak"

  for (int turn = 0; turn < MAX_TURNS; turn++) {
    if (conversation_abort) break;

    // ===== Pre-captured audio path (always-listening triggered the convo) =====
    // If turn 0 and ambient already captured the user's utterance, skip the
    // ready cue + re-record. Just acknowledge with a tiny tone, set
    // actual_audio_samples, and fall through to silence-check + Gemini.
    bool used_pre_capture = false;
    if (turn == 0 && audio_pre_captured) {
      Serial.println("[convo] using pre-captured audio");
      M5.Speaker.setVolume(BEEP_VOLUME);
      M5.Speaker.tone(880, 60); delay(80);
      actual_audio_samples = pre_captured_samples;
      face_override_active = true;
      face_override_eye    = EyeState::NORMAL;
      face_override_mouth  = MouthShape::SMILE;
      face_override_until  = millis() + 600;
      current_idle_label   = "heard you";
      idle_label_until     = millis() + 600;
      draw_face(false);
      audio_pre_captured   = false;
      pre_captured_samples = 0;
      used_pre_capture = true;
      current_turn_ambient = true;   // Gemini gets the "addressed to Beemo?" check
    } else {
      current_turn_ambient = false;  // touch-triggered or mid-conversation turn
    }

    if (!used_pre_capture) {

    // ===== Turn start cue =====
    // Cue beeps (ready, your-turn) use the universal BEEP_VOLUME.
    const uint8_t CUE_VOLUME = BEEP_VOLUME;
    if (turn == 0) {
      // First turn: soft 3-note ascending ready cue
      M5StackChan.Motion.moveY(520, 1000);
      face_override_active = true;
      face_override_eye    = EyeState::WIDE;
      face_override_mouth  = MouthShape::OPEN;
      face_override_until  = millis() + 1400;
      set_led_override(255, 180, 0, 1200, false);
      current_idle_label   = "get ready...";
      idle_label_until     = millis() + 1200;
      draw_face(false);
      M5.Speaker.setVolume(CUE_VOLUME);
      M5.Speaker.tone(523, 100); delay(120);
      M5.Speaker.tone(659, 100); delay(120);
      M5.Speaker.tone(880, 150); delay(200);
    } else {
      // Follow-on turn: single soft chirp, no second note
      M5.Speaker.setVolume(CUE_VOLUME);
      M5.Speaker.tone(880, 60); delay(80);
    }

    // ===== Listen =====
    if (!M5.Mic.begin() ||
        !M5.Mic.record(audio_buffer, AUDIO_BUFFER_SAMPLES, AUDIO_SAMPLE_RATE)) {
      set_audio_diag("mic failed", 3000);
      M5.Mic.end();
      break;
    }
    delay(250);  // mic warmup (was 400; 150 was too tight, cut off front of words)

    face_override_active = true;
    face_override_eye    = EyeState::WIDE;
    face_override_mouth  = MouthShape::OPEN;
    face_override_until  = millis() + AUDIO_RECORD_SECONDS * 1000 + 200;
    set_led_override(0, 255, 0, AUDIO_RECORD_SECONDS * 1000, false);
    current_idle_label   = (turn == 0) ? "talk to Beemo" : "your turn";
    idle_label_until     = millis() + AUDIO_RECORD_SECONDS * 1000;
    draw_face(false);

    // ===== Live-reactive listen: VAD + amplitude-driven face + head bob =====
    // While the mic records, sample what's been written so far and react in
    // real time. End the recording early once we detect ~800ms of silence
    // after the user actually started talking. This makes turn-by-turn feel
    // much snappier and avoids sending 5s of empty audio to Gemini.
    const unsigned long VAD_SILENCE_MS    = 1000;  // hangover after last voice
    const unsigned long VAD_MIN_TALK_MS   = 1800;  // require >=1.8s elapsed before VAD ends recording
    const unsigned long VAD_MIN_AUDIO_MS  = 1500;  // ensure at least 1.5s of audio is sent to Gemini
    const int           VAD_LOUD_X        = 3;     // multiplier for "loud" face
    const size_t        CHUNK_SAMPLES     = AUDIO_SAMPLE_RATE * 100 / 1000;  // 100 ms

    unsigned long t0              = millis();
    unsigned long last_voice_ms   = 0;     // when we last saw above-threshold audio
    unsigned long last_face_ms    = 0;
    unsigned long last_bob_ms     = 0;
    bool          ever_heard_voice= false;
    bool          mouth_open      = false;
    int           bob_phase       = 0;
    size_t        samples_recorded= AUDIO_BUFFER_SAMPLES;  // default: full buffer

    while (M5.Mic.isRecording() &&
           (millis() - t0) < (AUDIO_RECORD_SECONDS * 1000 + 1000)) {
      if (poll_abort_double_tap()) break;

      unsigned long elapsed = millis() - t0;
      size_t got = (size_t)elapsed * AUDIO_SAMPLE_RATE / 1000;
      if (got > AUDIO_BUFFER_SAMPLES) got = AUDIO_BUFFER_SAMPLES;

      if (got >= CHUNK_SAMPLES) {
        // Peak over the most recent 100ms chunk
        int chunk_peak = 0;
        for (size_t i = got - CHUNK_SAMPLES; i < got; i++) {
          int v = audio_buffer[i] < 0 ? -audio_buffer[i] : audio_buffer[i];
          if (v > chunk_peak) chunk_peak = v;
        }
        if (chunk_peak > SILENCE_PEAK_THRESHOLD) {
          last_voice_ms   = millis();
          ever_heard_voice= true;
        }
        // VAD end-of-speech: heard voice, then ~1s of quiet, AND we've been
        // recording long enough -- end the recording. Floor recording at
        // VAD_MIN_AUDIO_MS so Gemini gets enough audio context.
        if (ever_heard_voice &&
            elapsed > VAD_MIN_TALK_MS &&
            (millis() - last_voice_ms) > VAD_SILENCE_MS) {
          size_t min_samples = (size_t)VAD_MIN_AUDIO_MS * AUDIO_SAMPLE_RATE / 1000;
          samples_recorded = (got < min_samples) ? min_samples : got;
          if (samples_recorded > AUDIO_BUFFER_SAMPLES) samples_recorded = AUDIO_BUFFER_SAMPLES;
          break;
        }

        // Live face reactivity: open mouth + wide eyes when loud
        if (millis() - last_face_ms > 120) {
          bool loud_now = chunk_peak > SILENCE_PEAK_THRESHOLD * VAD_LOUD_X;
          if (loud_now != mouth_open) {
            face_override_mouth = loud_now ? MouthShape::OPEN : MouthShape::SMILE;
            face_override_eye   = loud_now ? EyeState::WIDE   : EyeState::NORMAL;
            face_override_until = millis() + 700;
            draw_face(false);
            mouth_open = loud_now;
          }
          last_face_ms = millis();
        }

        // Subtle head bob while listening -- ~3 Hz, alternates
        if (millis() - last_bob_ms > 320) {
          bob_phase = 1 - bob_phase;
          M5StackChan.Motion.moveY(bob_phase ? 490 : 510, 280);
          last_bob_ms = millis();
        }
      }
      delay(40);
    }
    M5.Mic.end();
    actual_audio_samples = samples_recorded;
    Serial.printf("[listen] %u samples recorded (%.1fs)\n",
                  (unsigned)samples_recorded,
                  samples_recorded / (float)AUDIO_SAMPLE_RATE);
    if (conversation_abort) break;

    }  // end if (!used_pre_capture) -- live-listen branch

    // ===== Silence detection: did user actually speak this turn? =====
    // For both pre-captured and live-recorded paths, actual_audio_samples
    // holds how much audio is in audio_buffer.
    size_t turn_samples = actual_audio_samples;
    if (turn_samples == 0 || turn_samples > AUDIO_BUFFER_SAMPLES) {
      turn_samples = AUDIO_BUFFER_SAMPLES;
    }
    int peak = 0;
    for (size_t i = 0; i < turn_samples; i++) {
      int v = audio_buffer[i] < 0 ? -audio_buffer[i] : audio_buffer[i];
      if (v > peak) peak = v;
    }
    if (peak < SILENCE_PEAK_THRESHOLD) {
      if (turn == 0) {
        snprintf(response_text, RESPONSE_TEXT_MAX,
                 "Beemo didn't hear anything. Try again whenever.");
      } else {
        snprintf(response_text, RESPONSE_TEXT_MAX,
                 "(Beemo is here whenever you want to talk more.)");
      }
      break;
    }

    // ===== Quick ack tone before the Gemini call ("mhm" / "ok" cue) =====
    // Short, low-volume cue so the user knows Beemo heard them while we
    // wait for Gemini. Randomized so it doesn't feel mechanical.
    {
      M5.Speaker.setVolume(BEEP_VOLUME);
      int ack = random(0, 3);
      if (ack == 0) {          // descending "mhm"
        M5.Speaker.tone(880, 90); delay(95);
        M5.Speaker.tone(659, 110); delay(115);
      } else if (ack == 1) {   // rising "oh!"
        M5.Speaker.tone(523, 70); delay(75);
        M5.Speaker.tone(880, 90); delay(95);
      } else {                 // single soft chirp
        M5.Speaker.tone(740, 110); delay(115);
      }
    }

    // ===== Switch back to speaker =====
    delay(120);  // was 300
    M5.Speaker.end(); delay(120);  // was 200
    M5.Speaker.begin(); M5.Speaker.setVolume(255);

    // ===== Thinking / "Tilt" =====
    M5StackChan.Motion.moveX(-100, 400);
    M5StackChan.Motion.moveY(450, 400);
    face_override_active = true;
    face_override_eye    = EyeState::FLAT;
    face_override_mouth  = MouthShape::NEUTRAL;
    face_override_until  = millis() + 60000;
    set_led_override(255, 180, 0, 60000, false);
    current_idle_label   = "thinking...";
    idle_label_until     = millis() + 60000;
    draw_face(false);

    // ===== Call Gemini =====
    bool gemini_ok = call_gemini_with_audio();
    if (!gemini_ok) {
      // call_gemini already wrote an error into response_text -- show and bail
      break;
    }

    // Ambient address-check: if this turn was ambient-triggered and Gemini
    // decided we weren't being addressed, dismiss silently and go back to
    // listening. No TTS, no on-screen response.
    if (current_turn_ambient && response_text) {
      // Skip any leading whitespace then check prefix
      const char* p = response_text;
      while (*p == ' ' || *p == '"' || *p == '\n') p++;
      if (strncmp(p, "NOT_ADDRESSED", 13) == 0) {
        Serial.println("[ambient] Gemini: not addressed to Beemo, dismissing");
        response_text[0] = 0;
        showing_response = false;
        return;  // back to main loop -> ambient_tick resumes listening
      }
    }
    // Consume the flag so subsequent turns are normal
    current_turn_ambient = false;

    // Append BMO's response to session history for context on next turn
    session_history += " [BMO: ";
    size_t n = strlen(response_text);
    if (n > 140) n = 140;  // keep history bounded
    for (size_t i = 0; i < n; i++) {
      char c = response_text[i];
      if (c == '"' || c == '[' || c == ']' || c == '\\') c = ' ';
      session_history += c;
    }
    session_history += "]";

    // ===== Speak with mouth sync + nod =====
    M5StackChan.Motion.moveX(0, 500);
    size_t tts_samples = synthesize_speech(response_text);
    if (tts_samples > 0) play_with_mouth_sync(tts_samples);
    if (conversation_abort) break;

    // Brief pause before opening mic for the next turn
    delay(600);
    last_interaction_ms = millis();
  }

  if (conversation_abort) {
    snprintf(response_text, RESPONSE_TEXT_MAX, "(Okay! BMO will be quiet.)");
  }

  // ===== End-of-conversation screen =====
  // Preserve sticky (user-requested via tool) LED override; clear the rest.
  if (!led_override_sticky) led_override_active = false;
  face_override_active = false;
  // Suppress wake-word re-detection for a moment so BMO's own TTS doesn't
  // immediately re-fire (the response_until window helps too).
  wake_cooldown_until_ms = millis() + 2500;
  showing_response = true;
  response_shown_at = millis();
  response_until    = millis() + RESPONSE_MAX_VISIBLE_MS;
  last_interaction_ms = millis();
  current_idle_label = nullptr;
  draw_response_screen();

  // Drain any stale tap that may have queued during the conversation
  M5StackChan.update();
  M5StackChan.TouchSensor.wasClicked();
  M5StackChan.TouchSensor.wasSwipedForward();
  M5StackChan.TouchSensor.wasSwipedBackward();
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
          "<button type='submit'>Test Mic + Speaker (record + playback)</button></form>"
          "<h2>Talk to BMO</h2>"
          "<p>Record 4 s and send to Gemini for a response. BMO will reply "
          "in text on screen (no voice yet -- that comes in Phase 9e).</p>"
          "<form action='/talk-to-bmo' method='POST'>"
          "<button type='submit'>Talk to BMO</button></form>"
          "<form action='/toggle-listen' method='POST'>"
          "<button type='submit'>";
  html += (listening_mode ? "Turn always-listening OFF (require touch)"
                          : "Turn always-listening ON (voice activated)");
  html += "</button></form>"
          "<p><a href='/photos'>BMO's photos &rarr;</a></p>";
  html += "<h2>Settings</h2>";
  html += "<p>Quiet mode: <b>";
  html += (quiet_mode ? "ON" : "off");
  html += "</b> &mdash; ";
  html += (quiet_mode
    ? "BMO is still and silent unless you interact directly."
    : "BMO does idle behaviors (small movements, sounds) on its own.");
  html += "</p><form action='/toggle-quiet' method='POST'>"
          "<button type='submit'>";
  html += (quiet_mode ? "Turn quiet mode OFF (autonomous behaviors)"
                      : "Turn quiet mode ON (only respond to me)");
  html += "</button></form>";
  html += "<form action='/forget' method='POST'>"
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
  http_server.on("/talk-to-bmo", HTTP_POST, []() {
    conversation_pending = true;
    http_server.send(200, "text/html",
      "<h1>BMO is listening...</h1>"
      "<p>Speak after the SPEAK NOW prompt on the robot. BMO will think, "
      "then show a text response on screen.</p>"
      "<p><a href='/'>Back to status</a></p>");
  });
  http_server.on("/toggle-quiet", HTTP_POST, []() {
    quiet_mode = !quiet_mode;
    http_server.sendHeader("Location", "/", true);
    http_server.send(302, "text/plain", "");
  });
  http_server.on("/toggle-listen", HTTP_POST, []() {
    listening_mode = !listening_mode;
    http_server.sendHeader("Location", "/", true);
    http_server.send(302, "text/plain", "");
  });
  http_server.on("/tflm-status", []() {
    char body[300];
    snprintf(body, sizeof(body),
             "%s\nfrontend_ready=%d samples=%u invocations=%u "
             "last_raw_prob=%.3f last_avg_prob=%.3f max_prob_seen=%.3f\n",
             tflm_boot_status,
             (int)mww_frontend_ready,
             (unsigned)mww_samples_processed,
             mww_invocations,
             mww_last_raw_prob,
             mww_last_avg_prob,
             mww_max_prob_seen);
    http_server.send(200, "text/plain", body);
  });

  // /photos -- HTML gallery of recent snaps (ring buffer of PHOTO_SLOT_COUNT).
  http_server.on("/photos", []() {
    String html = "<!doctype html><html><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>BMO photos</title>"
                  "<style>"
                  "body{font-family:system-ui;background:#143;color:#eef;padding:1rem;}"
                  "h1{color:#fdd;}img{max-width:100%;border:4px solid #fdd;"
                  "border-radius:8px;display:block;margin:0.5rem 0;}"
                  ".photo{margin:1.5rem 0;padding:0.5rem;background:#0a2;border-radius:8px;}"
                  ".caption{font-style:italic;color:#cfc;margin:0.25rem 0;}"
                  ".time{font-size:0.85em;color:#9bb;}"
                  "</style></head><body>"
                  "<h1>BMO photos</h1>"
                  "<p><a style='color:#fdd' href='/'>Back to status</a></p>";
    if (photo_slots_used == 0) {
      html += "<p>No photos yet. Ask BMO to see something!</p>";
    } else {
      // Newest first: walk slots backwards from photo_next_slot.
      for (int i = 0; i < photo_slots_used; i++) {
        int slot = (photo_next_slot + PHOTO_SLOT_COUNT - 1 - i) % PHOTO_SLOT_COUNT;
        if (!photo_slots[slot].data) continue;
        struct tm tinfo;
        char timebuf[40] = "(unknown)";
        if (localtime_r(&photo_slots[slot].taken_at, &tinfo)) {
          strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %I:%M:%S %p", &tinfo);
        }
        html += "<div class='photo'>";
        html += "<img src='/photo?i=" + String(slot) + "'>";
        html += "<div class='caption'>";
        html += photo_slots[slot].caption[0] ? photo_slots[slot].caption : "(no caption)";
        html += "</div><div class='time'>" + String(timebuf) + "</div></div>";
      }
    }
    html += "</body></html>";
    http_server.send(200, "text/html", html);
  });

  // /photo?i=N -- serve the raw JPEG for slot N.
  http_server.on("/photo", []() {
    int slot = http_server.arg("i").toInt();
    if (slot < 0 || slot >= PHOTO_SLOT_COUNT || !photo_slots[slot].data) {
      http_server.send(404, "text/plain", "no such photo");
      return;
    }
    http_server.sendHeader("Cache-Control", "no-store");
    http_server.send_P(200, "image/jpeg",
                      (const char*)photo_slots[slot].data,
                      photo_slots[slot].len);
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

// Phase 9e: quiet mode toggle. When true, idle behaviors + micro-fidgets are
// suppressed so BMO only does things in response to direct user input.
// Default on -- BMO stays still until you talk to it.
bool quiet_mode = true;

// Universal quiet volume for all non-speech beeps/boops. TTS speech is set to
// 255 only for the duration of playRaw, then restored back to this value.
const uint8_t BEEP_VOLUME = 18;

unsigned long last_tick_ms   = 0;
unsigned long last_render_ms = 0;
unsigned long next_blink_at  = 0;
unsigned long blink_until    = 0;
bool was_blinking = false;

void setup() {
  M5StackChan.begin();
  M5.Speaker.setVolume(BEEP_VOLUME);
  M5StackChan.Motion.goHome();
  delay(500);

  face_buffer.setPsram(true);
  face_buffer.setColorDepth(16);
  face_buffer.createSprite(320, 240);

  // Phase 9c: pre-allocate the audio capture buffer (in PSRAM)
  init_audio_buffer();
  // Phase 9d: response text buffer for Gemini replies
  init_response_buffer();
  // Phase 9e: TTS PCM buffer (480 KB) for synthesized speech
  init_tts_buffer();

  // Wi-Fi before the boot chime so the screen can show setup instructions
  // immediately if needed (no faux-boot when we're in setup mode).
  wifi_init();

  // Phase 9g: sync time from NTP (used by get_time tool). US Eastern offset
  // with DST. After wifi_init returns the connection should be up.
  if (!wifi_setup_mode) {
    configTime(-5 * 3600, 3600, "pool.ntp.org", "time.nist.gov");
  }

  // Phase 9j: load on-device wake word model ("Hi, ESP" WakeNet9).
  Serial.println(">>> before wakenet_init");
  wakenet_init();
  Serial.println(">>> after wakenet_init");

  // Phase 9j-2: TFLite Micro setup for 'Hey Beepoh' (community model).
#if TFLM_BEEPOH_ENABLED
  Serial.println(">>> before tflm_beepoh_init");
  tflm_beepoh_init();
  mww_frontend_init();
  Serial.println(">>> after tflm_beepoh_init");
#endif

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

  // Phase 9d showing-response branch: response held on screen until tap or timeout.
  // Must come BEFORE the touch handlers so a dismiss-tap doesn't become a pet.
  // Enforces a 5 s minimum visible window so a stale queued tap can't dismiss
  // the response the instant Gemini's reply lands.
  if (showing_response) {
    unsigned long now = millis();
    unsigned long elapsed = now - response_shown_at;
    bool can_dismiss = (elapsed >= RESPONSE_MIN_VISIBLE_MS);
    bool tap_dismiss = can_dismiss && M5StackChan.TouchSensor.wasClicked();
    bool time_dismiss = (now >= response_until);
    if (tap_dismiss || time_dismiss) {
      showing_response = false;
      last_render_ms = 0;
    }
    delay(30);
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

  // Touch sensor gestures:
  //   hold       -> start conversation (deliberate, harder to false-trigger)
  //   single tap -> pet (heart eyes, friendly poke)
  //   double tap -> toggle always-listening mode on/off
  //   swipe      -> also start conversation (keep as fallback)
  // Single-tap pet is delayed ~500ms so we can disambiguate from a double-tap.
  auto& ts = M5StackChan.TouchSensor;
  static unsigned long pending_pet_tap_ms = 0;
  static unsigned long last_tap_ms        = 0;
  const unsigned long  DOUBLE_TAP_WINDOW  = 500;

  if (ts.wasHold()) {
    conversation_pending = true;
    pending_pet_tap_ms = 0;  // suppress any pending single-tap
    last_tap_ms = 0;
  } else if (ts.wasClicked()) {
    unsigned long t = millis();
    if (last_tap_ms != 0 && (t - last_tap_ms) < DOUBLE_TAP_WINDOW) {
      // Double tap -> toggle listening mode
      listening_mode = !listening_mode;
      Serial.printf("[touch] double-tap -> listening_mode=%d\n", (int)listening_mode);
      M5.Speaker.setVolume(BEEP_VOLUME);
      if (listening_mode) {
        // Ascending two-tone "on"
        M5.Speaker.tone(659, 90); delay(100);
        M5.Speaker.tone(988, 110); delay(120);
      } else {
        // Descending two-tone "off"
        M5.Speaker.tone(988, 90); delay(100);
        M5.Speaker.tone(523, 130); delay(140);
      }
      // Brief on-screen label
      static char ls_label[40];
      snprintf(ls_label, sizeof(ls_label),
               listening_mode ? "listening: ON" : "listening: OFF");
      current_idle_label = ls_label;
      idle_label_until = millis() + 2500;
      // Cancel any queued pet fire
      pending_pet_tap_ms = 0;
      last_tap_ms = 0;
    } else {
      // First tap -- defer the pet so a follow-up tap can claim it as a double-tap
      pending_pet_tap_ms = t + DOUBLE_TAP_WINDOW;
      last_tap_ms = t;
    }
  }
  // Fire deferred single-tap pet if window elapsed without a second tap
  if (pending_pet_tap_ms != 0 && millis() >= pending_pet_tap_ms) {
    on_pet_click();
    pending_pet_tap_ms = 0;
    last_tap_ms = 0;
  }

  if (ts.wasSwipedForward())  conversation_pending = true;
  if (ts.wasSwipedBackward()) conversation_pending = true;

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
  // Always-listening: poll ambient mic for voice trigger.
  // Must run BEFORE the conversation_pending check so it can set the flag
  // and have it acted on this same iteration.
  ambient_tick();

  // Phase 9d: full conversation flow
  if (conversation_pending) {
    conversation_pending = false;
    run_conversation();
    // If a response is now on screen, skip the rest of this iteration so
    // idle/micro behaviors don't repaint the face over the response.
    if (showing_response) {
      delay(30);
      return;
    }
  }

  // Autonomous "alive" layer. Skipped when quiet_mode is on so BMO only
  // reacts to direct user input.
  if (!quiet_mode) {
    maybe_fire_micro();
    maybe_fire_idle();
  }

  delay(20);
}
