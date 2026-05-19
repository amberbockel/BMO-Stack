#include <Arduino.h>
#include <M5StackChan.h>
#include <M5Unified.h>

const uint16_t FACE_COLOR = TFT_DARKCYAN;
const uint8_t SPEAKER_VOLUME = 128;  // 0..255

// === Forward declarations ===
void play_boot();
void play_delight();
void play_huh();
void play_sad();
void play_curious();
void play_surprise();
void play_giggle();
void play_sleepy();
void play_ok();

void slow_blink();
void curious_tilt();
void snap_look();
void sad_droop();
void excited_wiggle();
void freeze();
void sigh();
void confused_shake();
void happy_bounce();
void double_blink();
void show_label(const char* name);

typedef void (*gesture_fn_t)();
struct Gesture { const char* name; gesture_fn_t fn; };

const Gesture gestures[] = {
  {"slow_blink",     slow_blink},
  {"curious_tilt",   curious_tilt},
  {"snap_look",      snap_look},
  {"sad_droop",      sad_droop},
  {"excited_wiggle", excited_wiggle},
  {"freeze",         freeze},
  {"sigh",           sigh},
  {"confused_shake", confused_shake},
  {"happy_bounce",   happy_bounce},
  {"double_blink",   double_blink},
};
const int gesture_count = sizeof(gestures) / sizeof(gestures[0]);
int gesture_index = 0;

void setup() {
  M5StackChan.begin();
  M5.Speaker.setVolume(SPEAKER_VOLUME);
  M5StackChan.Motion.goHome();
  delay(800);
  play_boot();
  delay(200);
  show_label("ready: tap head");
}

void loop() {
  M5StackChan.update();
  if (M5StackChan.TouchSensor.wasPressed()) {
    show_label(gestures[gesture_index].name);
    gestures[gesture_index].fn();
    delay(300);
    M5StackChan.Motion.goHome();
    delay(1200);
    show_label("(tap head for next)");
    gesture_index = (gesture_index + 1) % gesture_count;
  }
  delay(20);
}

void show_label(const char* name) {
  M5StackChan.Display().fillScreen(FACE_COLOR);
  M5StackChan.Display().setTextColor(TFT_WHITE);
  M5StackChan.Display().setTextSize(2);
  M5StackChan.Display().setCursor(10, 10);
  M5StackChan.Display().println(name);
}

// === Sound palette ===
// Each delay is slightly longer than its tone so notes stay distinct
// rather than slurring into one another.

void play_boot() {
  // 4 ascending major notes — "I'm awake!"
  M5.Speaker.tone(523,  80);  delay(85);   // C5
  M5.Speaker.tone(659,  80);  delay(85);   // E5
  M5.Speaker.tone(784,  80);  delay(85);   // G5
  M5.Speaker.tone(1047, 150); delay(160);  // C6
}

void play_delight() {
  // Ascending C major arpeggio — tightened for a more BMO bounce
  M5.Speaker.tone(523,  55);  delay(60);
  M5.Speaker.tone(659,  55);  delay(60);
  M5.Speaker.tone(784,  55);  delay(60);
  M5.Speaker.tone(1047, 100); delay(105);
}

void play_huh() {
  M5.Speaker.tone(220, 120);  delay(130);
}

void play_sad() {
  // Heavy 4-note descending phrase, dropped an octave from the v1
  // version. Final note held long for the "wah" weight.
  M5.Speaker.tone(392, 200);  delay(210);  // G4
  M5.Speaker.tone(349, 200);  delay(210);  // F4
  M5.Speaker.tone(294, 240);  delay(250);  // D4
  M5.Speaker.tone(247, 450);  delay(460);  // B3 (held)
}

void play_curious() {
  // Upward perfect fourth
  M5.Speaker.tone(784,  90);  delay(95);
  M5.Speaker.tone(1047, 120); delay(125);
}

void play_surprise() {
  M5.Speaker.tone(1568, 80);  delay(85);   // G6
}

void play_giggle() {
  // Chattery, bouncing — high range, fast, pitches jump around for variety.
  M5.Speaker.tone(1047, 45);  delay(50);   // C6
  M5.Speaker.tone(1319, 45);  delay(50);   // E6
  M5.Speaker.tone(1175, 45);  delay(50);   // D6
  M5.Speaker.tone(1397, 45);  delay(50);   // F6
  M5.Speaker.tone(1175, 45);  delay(50);   // D6
  M5.Speaker.tone(1319, 70);  delay(75);   // E6
}

void play_sleepy() {
  M5.Speaker.tone(523, 200);  delay(210);
  M5.Speaker.tone(440, 200);  delay(210);
  M5.Speaker.tone(330, 300);  delay(310);
}

void play_ok() {
  M5.Speaker.tone(523, 60);   delay(65);
  M5.Speaker.tone(784, 80);   delay(85);
}

// === Gestures (with sound timing tuned so audio and motion overlap) ===
// Servo commands are fire-and-forget; calling moveX then play_xxx() lets
// the head move during the sound. Where the sound is at the front, the
// gesture announces itself before reacting.

void slow_blink() {
  const int w = M5StackChan.Display().width();
  const int h = M5StackChan.Display().height();
  for (int i = 0; i < h / 2; i += 6) {
    M5StackChan.Display().fillRect(0, 0, w, i, TFT_BLACK);
    M5StackChan.Display().fillRect(0, h - i, w, i, TFT_BLACK);
    delay(18);
  }
  delay(180);
  M5StackChan.Display().fillScreen(FACE_COLOR);
}

void curious_tilt() {
  M5StackChan.Motion.moveX(300, 300);
  M5StackChan.Motion.moveY(650, 300);
  play_curious();
  delay(1300);
}

void snap_look() {
  M5StackChan.Motion.moveX(800, 1000);
  play_surprise();
  delay(1420);
}

void sad_droop() {
  M5StackChan.Motion.moveY(100, 200);
  play_sad();
  delay(1000);
}

void excited_wiggle() {
  play_giggle();
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(   0, 1000); delay(220);
}

void freeze() {
  // Intentional stillness, intentional silence.
  delay(2000);
}

void sigh() {
  M5StackChan.Motion.moveY(500, 200);
  delay(700);
  M5StackChan.Motion.moveY(120, 150);
  play_sleepy();
  delay(700);
}

void confused_shake() {
  play_huh();
  M5StackChan.Motion.moveX(-100, 600); delay(160);
  M5StackChan.Motion.moveX( 100, 600); delay(160);
  M5StackChan.Motion.moveX(-100, 600); delay(160);
  M5StackChan.Motion.moveX(   0, 600); delay(380);
}

void happy_bounce() {
  play_delight();
  M5StackChan.Motion.moveY(550, 900); delay(180);
  M5StackChan.Motion.moveY(300, 900); delay(180);
  M5StackChan.Motion.moveY(550, 900); delay(180);
  M5StackChan.Motion.moveY(350, 700); delay(280);
}

void double_blink() {
  slow_blink();
  delay(120);
  slow_blink();
}
