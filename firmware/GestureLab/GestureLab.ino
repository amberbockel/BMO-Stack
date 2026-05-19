#include <Arduino.h>
#include <M5StackChan.h>

const uint16_t FACE_COLOR = TFT_DARKCYAN;

// Forward declarations
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
  M5StackChan.Motion.goHome();
  delay(1000);
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
  delay(1500);
}

void snap_look() {
  M5StackChan.Motion.moveX(800, 1000);
  delay(1500);
}

void sad_droop() {
  M5StackChan.Motion.moveY(100, 200);
  delay(1500);
}

void excited_wiggle() {
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(-200, 1000); delay(170);
  M5StackChan.Motion.moveX( 200, 1000); delay(170);
  M5StackChan.Motion.moveX(   0, 1000); delay(220);
}

void freeze() {
  // Intentional stillness. The lack of motion IS the gesture.
  delay(2000);
}

void sigh() {
  M5StackChan.Motion.moveY(500, 200);
  delay(700);
  M5StackChan.Motion.moveY(120, 150);
  delay(1300);
}

void confused_shake() {
  M5StackChan.Motion.moveX(-100, 600); delay(160);
  M5StackChan.Motion.moveX( 100, 600); delay(160);
  M5StackChan.Motion.moveX(-100, 600); delay(160);
  M5StackChan.Motion.moveX(   0, 600); delay(380);
}

void happy_bounce() {
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
