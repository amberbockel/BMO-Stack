# Wake word "Hey Beemo" — submission guide

**Written:** 2026-05-20 overnight, after research.
**Status:** Honest correction of what I told you in the session.

---

## Heads-up: the picture changed during research

In the session I told you "submit a request to Espressif's free customization service, get a model back in 1-2 weeks, scaffold the firmware in the meantime." That was based on outdated information. Sorry. Here's the actual lay of the land as of May 2026:

- **Espressif's official customization is enterprise-only.** Their docs require you to provide a recorded corpus of 500+ speakers (including 100+ children) saying the wake word 15× each at 1m and 3m, 16kHz/16-bit/mono WAV. Cost is quote-based. Email to `sales@espressif.com`. **Not a hobbyist path.**
- **Picovoice Porcupine** lets you type "Hey Beemo" in a console and instantly produces a wake word model. **But the free tier only outputs Linux/Mac/Windows/x86 binaries — ESP32 requires an enterprise license** (quote-based).
- **CustomESP-SR** (third-party) is the only one-stop paid service that targets ESP-SR/WakeNet directly. $1,000 USD, ~10 business days, no audio recording needed. They handle synthetic corpus + training. Model drops into ESP-SR projects.
- **microWakeWord** (open-source) is the modern hobbyist standard. Free. Produces a TensorFlow Lite Micro `.tflite` file that runs on ESP32-S3. **This is your path.**

---

## Recommended path: microWakeWord

Two sub-paths — pick one in the morning:

### Path A — Community request (zero work, slower)

The Home Assistant microWakeWord maintainer is currently taking custom wake word requests for free and batch-training them.

**Morning steps (~5 minutes):**

1. Go to **https://community.home-assistant.io/t/microwakeword-custom-v2-wake-words-taking-requests-12-2-only/803409**
2. Create a free Home Assistant community account if you don't have one (right side, "Sign Up").
3. Reply on the thread with:
   - Wake word: `Hey Beemo`
   - Target: ESP32-S3 / TFLite Micro
   - Use case: M5Stack CoreS3 personal robot, BMO companion build
4. Wait for the maintainer to batch-train. Turnaround varies (days to weeks; not committed). They post the result back in the thread.
5. When the file arrives, drop the `.tflite` into `firmware/IdleLab/wakenet_model/` (folder doesn't exist yet — create it) and tell me.

### Path B — Self-train in Colab (free, ~3 hours of GPU time, you can sleep through it)

If you don't want to wait on the community thread, you can train your own model in a Google Colab notebook.

**Morning steps (~15 minutes to start training; training runs unattended for hours):**

1. Open **https://github.com/OHF-Voice/micro-wake-word** in a browser.
2. Click on `notebooks/basic_training_notebook.ipynb`.
3. At the top of the rendered notebook, click "**Open in Colab**".
4. Sign in with your Google account.
5. Set the runtime to GPU (Runtime → Change runtime type → T4 GPU).
6. In the first config cell, change the wake word string to `"hey beemo"`.
7. Run all cells (Runtime → Run all). Training takes ~1–3 hours on a free GPU.
8. The final cell saves the `.tflite` file. Download it.
9. Drop into `firmware/IdleLab/wakenet_model/` and tell me.

### Path C — CustomESP-SR (if you want to throw $1k at the problem)

Skip unless budget is open. Wake word in 10 business days, no work on your end.

1. **https://custom-espsr.com/**
2. Order form: wake word `Hey Beemo`, language English (US), chip ESP32-S3.
3. Pay $1,000.
4. ~10 business days, you test in a browser dashboard, then receive a WakeNet9 model file.

---

## What's NOT going to work in 15 minutes tomorrow

To be clear about scope:
- There is no free, instant, on-device custom wake word that gets you "Hey Beemo" specifically working tomorrow.
- The fastest realistic delivery is **Path B** (Colab self-train, 3 hours, free) but you have to actually start the training.
- Until the model arrives, **hold-to-talk stays the daily-driver gesture.** It works well.

---

## Firmware integration when the model arrives

Once the `.tflite` file is dropped into `firmware/IdleLab/wakenet_model/`:

- I'll wire it via TensorFlow Lite Micro on the ESP32-S3.
- The wake detector will run in a background task, consuming I2S mic frames.
- On detection, it flips the same `conversation_pending` flag the hold-gesture sets.
- We'll keep hold-to-talk as a fallback in case wake-word misfires/misses.

Estimated integration time: 1 session (~2 hours), assuming the TFLite Micro library plays nice with M5Unified's mic. I'll do a research dry-run during this overnight slot to flag any gotchas.

---

## Reference

Sources:
- [Espressif official customization (enterprise)](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/ESP_Wake_Words_Customization.html)
- [microWakeWord on GitHub (OHF-Voice/micro-wake-word)](https://github.com/OHF-Voice/micro-wake-word)
- [Home Assistant microWakeWord requests thread](https://community.home-assistant.io/t/microwakeword-custom-v2-wake-words-taking-requests-12-2-only/803409)
- [CustomESP-SR ($1k paid service)](https://custom-espsr.com/)
- [Picovoice Porcupine (no free ESP32)](https://picovoice.ai/docs/quick-start/console-porcupine/)
