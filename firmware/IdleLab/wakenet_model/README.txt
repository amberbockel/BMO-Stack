# Hey Beepoh — microWakeWord model
Downloaded from https://microwakeword.com

## Files
  hey_beepoh.tflite    — TFLite model
  hey_beepoh.json      — ESPHome manifest

## ESPHome YAML example
micro_wake_word:
  models:
    - model: hey_beepoh.json
  on_wake_word_detected:
    - logger.log: "Wake word detected!"
