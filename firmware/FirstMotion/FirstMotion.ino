#include <Arduino.h>
#include <M5StackChan.h>

void setup() {
  M5StackChan.begin();

  M5StackChan.Display().fillScreen(TFT_BLACK);
  M5StackChan.Display().setTextSize(2);
  M5StackChan.Display().setTextColor(TFT_WHITE);
  M5StackChan.Display().setCursor(10, 100);
  M5StackChan.Display().println("FirstMotion");

  M5StackChan.Motion.goHome();
  delay(1500);
}

void loop() {
  M5StackChan.update();
  M5StackChan.Motion.moveX(-600, 1000);
  delay(2000);

  M5StackChan.update();
  M5StackChan.Motion.moveX(600, 1000);
  delay(2000);
}
