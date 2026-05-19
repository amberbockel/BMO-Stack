#include <M5Unified.h>

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.fillScreen(TFT_DARKCYAN);
  M5.Display.setTextColor(TFT_WHITE);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(40, 100);
  M5.Display.println("Hello, BMO");
}

void loop() {
}
