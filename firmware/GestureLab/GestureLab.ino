#include <Arduino.h>
#include <M5StackChan.h>

// Forward declarations
void slow_blink();
void curious_tilt();
void snap_look();
void show_label(const char* name);

const uint16_t FACE_COLOR = TFT_DARKCYAN;

const char* gesture_names[] = {"slow_blink", "curious_tilt", "snap_look"};
const int gesture_count = 3;
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
    const char* name = gesture_names[gesture_index];
    show_label(name);

    if (gesture_index == 0) slow_blink();
    else if (gesture_index == 1) curious_tilt();
    else if (gesture_index == 2) snap_look();

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
