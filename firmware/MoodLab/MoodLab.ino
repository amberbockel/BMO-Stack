#include <Arduino.h>
#include <M5StackChan.h>
#include <M5Unified.h>
#include <math.h>

// === Mood ===

struct Mood {
  float valence = 0.0f;
  float arousal = 0.3f;
  float energy  = 0.5f;

  static constexpr float V_NEUTRAL = 0.0f;
  static constexpr float A_NEUTRAL = 0.3f;
  static constexpr float E_NEUTRAL = 0.5f;

  // Decay rates (1/s); half-lives ~60s, ~30s, ~120s.
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

void event_happy(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.35f);
  m.arousal = Mood::clamp01(m.arousal + 0.20f);
  m.energy  = Mood::clamp01(m.energy  + 0.10f);
}
void event_sad(Mood& m) {
  m.valence = Mood::clamp11(m.valence - 0.35f);
  m.arousal = Mood::clamp01(m.arousal - 0.10f);
  m.energy  = Mood::clamp01(m.energy  - 0.10f);
}
void event_curious(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.10f);
  m.arousal = Mood::clamp01(m.arousal + 0.30f);
}
void event_surprised(Mood& m) {
  m.arousal = Mood::clamp01(m.arousal + 0.50f);
}
void event_tired(Mood& m) {
  m.energy  = Mood::clamp01(m.energy  - 0.30f);
  m.arousal = Mood::clamp01(m.arousal - 0.10f);
}
void event_cheer_up(Mood& m) {
  m.valence = Mood::clamp11(m.valence + 0.60f);
  m.energy  = Mood::clamp01(m.energy  + 0.30f);
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

uint16_t mood_to_color(const Mood& m) {
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

  float brightness = 0.4f + m.energy * 0.6f;
  r *= brightness;
  g *= brightness;
  b *= brightness;

  int ri = (int)(r < 0 ? 0 : (r > 255 ? 255 : r));
  int gi = (int)(g < 0 ? 0 : (g > 255 ? 255 : g));
  int bi = (int)(b < 0 ? 0 : (b > 255 ? 255 : b));

  return M5StackChan.Display().color565(ri, gi, bi);
}

// === Render ===

void render() {
  M5StackChan.Display().fillScreen(mood_to_color(mood));
  M5StackChan.Display().setTextColor(TFT_WHITE);

  M5StackChan.Display().setTextSize(2);
  M5StackChan.Display().setCursor(10, 10);
  M5StackChan.Display().printf("V: %+.2f", mood.valence);
  M5StackChan.Display().setCursor(10, 35);
  M5StackChan.Display().printf("A:  %.2f", mood.arousal);
  M5StackChan.Display().setCursor(10, 60);
  M5StackChan.Display().printf("E:  %.2f", mood.energy);

  M5StackChan.Display().setCursor(10, 110);
  M5StackChan.Display().printf("next: %s", events[next_event_index].name);

  M5StackChan.Display().setTextSize(1);
  M5StackChan.Display().setCursor(10, 220);
  M5StackChan.Display().print("tap head to nudge mood");
}

// === Loop ===

unsigned long last_tick_ms = 0;
unsigned long last_render_ms = 0;

void setup() {
  M5StackChan.begin();
  M5.Speaker.setVolume(128);
  M5StackChan.Motion.goHome();
  delay(800);
  // Quiet 2-note hello so we know audio works
  M5.Speaker.tone(523, 60); delay(65);
  M5.Speaker.tone(784, 80); delay(85);
  last_tick_ms = millis();
  render();
}

void loop() {
  M5StackChan.update();

  unsigned long now = millis();
  float dt = (now - last_tick_ms) / 1000.0f;
  mood.tick(dt);
  last_tick_ms = now;

  if (M5StackChan.TouchSensor.wasPressed()) {
    events[next_event_index].apply(mood);
    next_event_index = (next_event_index + 1) % event_count;
    M5.Speaker.tone(1047, 40);  // brief acknowledgement
    render();
    last_render_ms = now;
  }

  if (now - last_render_ms > 200) {
    render();
    last_render_ms = now;
  }

  delay(20);
}
