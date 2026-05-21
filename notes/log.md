# Session log

Newest entries on top. After every working session, append a new block: what we did, what worked, what broke, what to do next.

---

## NEXT SESSION — RESUME HERE

> Pinned section. Read this first when you come back. Last updated 2026-05-20 overnight, after a big push: Phases 9g (tool calling fixed), 9h (UX polish), 9k (camera + Gemini Vision) all landed.

### Where we are (current state, working build)

Robot is in a great place. Commit `ab6dbdd` on `main`. End-to-end working:
- **Hold-to-talk** (press and hold touch strip ~½s) → starts conversation. Tap = pet (heart eyes). Swipe = also starts convo (fallback).
- **Double-tap during conversation** → aborts speech/listening immediately.
- **Tool routing on `gemini-2.5-flash`** (paid tier): `set_led_color`, `play_gesture(dance)`, `get_time`, `get_weather`, `get_battery_level`, `see_scene` (camera + Vision).
- **LED color** actually changes the LEDs and persists across conversation turns (sticky override).
- **"Beemo" pronunciation** via TTS substitution + system prompt instruction.
- **Photos**: ask "Beemo, what do you see?" → BMO does pre-snap body language (wide eyes, white LEDs, "looking..."), camera shutter click sound, captures VGA frame, shows preview on screen, sends to Gemini Vision, speaks description. Last 5 photos browseable at `http://bmo.local/photos`.

### Morning tasks (you, ~5-15 min)

Order: do these in sequence.

1. **Read [`notes/wake-word-submission.md`](wake-word-submission.md)** — honest correction of what I told you last session. Espressif's free service isn't really a hobbyist path. Real options: free microWakeWord (community request OR self-train in Colab), or $1k CustomESP-SR. Pick a path. (~5 min to read + pick.)
2. **If you picked microWakeWord Path A (community request):** post on the HA forum thread linked in the guide. ~5 min including account creation.
3. **If you picked microWakeWord Path B (Colab self-train):** open the Colab notebook, change wake word to "hey beemo", run all cells. ~15 min to start; training runs unattended for ~3 hours.
4. **Rotate exposed API keys** (still pending from previous session). https://aistudio.google.com/apikey, update `firmware/IdleLab/gemini_credentials.h`. ~3 min.
5. **Tell me which path you picked** so I can plan the firmware integration accordingly. The TFLite Micro integration (Path A/B) is different from WakeNet9 (Path C).

### Next session priorities

Pick from this menu (or tell me something else):

- **Photo follow-ups** — quick win: ask "what color was that?" after a photo, BMO remembers the scene from session history. ~15 min.
- **Selfie mode** — "Beemo, take a selfie" → countdown beeps + snap. ~15 min.
- **Reactive vision** — every minute or so when idle, BMO quietly comments on what's around it. Big personality boost. ~30 min.
- **Mini games** — BMO is a video game console; could do number guessing, BMO trivia, simon-says. ~60 min.
- **Face recognition** — "That's Amber!" learns you over time. Bigger feature. ~2 sessions.
- **Bedtime/wake routines** — schedule-aware behaviors. ~30 min.

### What I shipped this session (2026-05-19 / 2026-05-20)

Gesture overhaul:
- **Hold-to-talk** replaces tap-to-talk (taps were getting eaten by the pet/heart-eyes handler).
- **Double-tap-to-abort** during conversation. Polled from listen loop + playback loop.
- **Triple-tap-sleep removed** (power-button hold still covers sleep).

LED color tool finally works:
- Added `led_override_sticky` flag — user-requested colors aren't wiped by listening-green or end-of-conversation cleanup.
- `set_led_override` now pushes to the LED hardware immediately (previously only `update_leds()` in the main loop did, which doesn't run during `run_conversation`).
- Templated reply now echoes the actual color name and surfaces errors instead of pretending success.

Gemini tool routing reliability:
- Switched from `gemini-2.5-flash-lite` to `gemini-2.5-flash` (paid tier — much better tool routing).
- Added `toolConfig: {functionCallingConfig: {mode: AUTO}}`.
- Lowered temperature 0.95 → 0.7 for more deterministic routing.
- Rewrote system prompt: tool routing is MANDATORY, with explicit "User: X → CALL tool" examples for every tool.
- On-screen `route: tool / route: text` indicator + `tool: <name>` label + tool result text for ~6 seconds — invaluable diagnostic.

Beemo name:
- TTS substitutes `BMO`/`Bmo`/`bmo`/`B-M-O` → `Beemo` before synthesis.
- System prompt explicitly tells Gemini to always write the name as "Beemo".

Phase 9k — camera + Gemini Vision:
- GC0308 camera (CoreS3 built-in) lazy-initialized on first `see_scene` call.
- VGA (640x480) capture in RGB565, converted to JPEG with `frame2jpg()`.
- Pre-snap body language: wide eyes + white LEDs + "looking..." label + slight head down.
- Two-tick shutter click sound (3kHz + 2.2kHz).
- Captured frame previewed on BMO's screen for ~2 sec (downscaled VGA → 320×240).
- JPEG sent to Gemini Vision (`gemini-2.5-flash` endpoint) with a BMO-personality system instruction.
- Vision response spoken directly (already styled by the prompt).
- Last 5 photos kept in a PSRAM ring buffer. Each photo carries its description as a caption + a Unix timestamp.
- Web routes: `/photos` (HTML gallery, newest first) and `/photo?i=N` (raw JPEG).
- Link added to the status page.

Bug fixes:
- Weather tool: switched from HTTPS (TLS handshake failed, returned -1) to plain HTTP + curl User-Agent + redirect-follow. Works now.
- Dance: real C-E-G-C arpeggio synced with swinging pan motion (was just shaking).
- **Empty Gemini response fix:** `gemini-2.5-flash` sometimes returns content with no parts when the user message has an empty text part + audio. Replaced the empty `"text":""` with explicit instructions ("Listen to the attached audio... call a tool if applicable, otherwise reply with a playful sentence."). This unblocked all tool routing.

### Architecture notes — banked for next session

- Camera init releases the I2C bus (`M5.In_I2C.release()`) — IMU/touch are on the same bus. After camera init, IMU+touch may stop responding until re-init. Lazy init means this only happens when `see_scene` fires.
- Photo ring buffer: 5 slots × ~30-60KB each ≈ 300KB PSRAM. Lost on reboot. Future iteration: persist to LittleFS or SD.
- `update_leds()` only runs from the main loop, NOT during `run_conversation`. Any LED change during a conversation must call `set_led_override` (which now pushes to hardware immediately) OR call `update_leds()` directly.
- Gemini request format that works: `{"role":"user","parts":[{"text":"Listen and respond..."},{"inlineData":{...audio...}}]}`. **Do not use empty text part** on `gemini-2.5-flash`.
- `gemini-2.5-flash` rate limit on paid tier is plenty for normal use. Cost is ~$0.10/M input + $0.40/M output. A conversation turn is ~2k tokens.

### Security TODO — rotate exposed keys (still pending)
Both `BMO_GEMINI_API_KEY` and `BMO_TTS_API_KEY` from `firmware/IdleLab/gemini_credentials.h` got pulled into a prior conversation's context window. They're still **in the local file (gitignored)**, so they never hit git, but they were visible to me. Rotate at https://aistudio.google.com/apikey and update the local file. ~3 min.

---

### Original Phase 9 status — Phase 9a (local hardware features) + Phase 9b (Wi-Fi provisioning).

### Where we are (one paragraph)

Project is well past the original brief's Phase 8. The robot now has: factory firmware safely backed up, 10-gesture body language vocabulary, 9-sound chiptune voice, mood state machine with decay, drawn BMO face with **8 eye states** (BLINK, SLEEPY, NORMAL, WIDE, CONTENT, ASLEEP, SWIRL, HEART) and **5 mouth shapes** (SMILE, NEUTRAL, FROWN, OPEN, GRIN), autonomous idle behaviors + micro-fidgets, **12 RGB LEDs with breathing pulse + per-behavior override**, **IMU shake → dizzy gesture** (swirl eyes + pink cheeks + LED flash), **touch reactions** (pet click → heart eyes + grin, swipe forward → excited, swipe back → calming), **triple-tap to sleep**, **sleep mode with Zz animation + breathing mouth + dim screen**, **power-button hold for sleep / long-hold for deep-sleep**, and **Wi-Fi provisioning with AP setup + captive portal + status page at `http://bmo.local/`**. Robot is connected to home Wi-Fi at 192.168.50.185 (status: 2026-05-19).

### First three things to do when resuming

1. **Re-orient.** Read `notes/project-brief.md` + this section + most recent chronological log entry. ~5 min.
2. **Verify the conversation flow still works.** Plug robot in, wait for Wi-Fi connect (see "wifi" indicator top-left). Open `http://bmo.local/` -> "Talk to BMO" -> say something. Robot should: Perk + ready beep -> SPEAK NOW (4s) -> Tilt + thinking -> Nod + speak the response. If that works end-to-end, system is healthy.
3. **Begin Phase 9j -- wake word "Hey BMO"** (or built-in wake word like "Hi ESP" to start). Use Espressif's ESP-SR library which is already bundled in the m5stack ESP32 platform package. Continuous wake-word listening on the chip itself, no cloud round-trip for activation. ~2 sessions of work.

### Phase 9 architecture (final, after the local-host detour)

**No local host needed.** Robot connects directly to Google APIs over Wi-Fi (Gemini for STT+LLM, Cloud TTS for voice output). Same architecture pattern the StackChan factory firmware uses, just pointed at our API keys instead of M5Stack's.

### Remaining phases:

These are flagged from earlier in the project but not resolved. Decide before Phase 9 begins.

- **Local LLM host:** MacBook (Ollama, free, requires laptop awake) / Raspberry Pi 5 (Ollama + whisper.cpp + piper, $80–$200 one-time, always-on) / hybrid (local primary + Anthropic API fallback).
- **Memory format:** JSON file / SQLite / vector store. Depends on host choice.
- **Speech I/O:** voice in/out from day one, or text-only first then layer in audio?
- **Wi-Fi credentials:** hardcoded in flash, or provisioning UI on the robot?

The `brenpoly/be-more-agent` GitHub repo (Pi 5 + Ollama + whisper.cpp + piper) is a working reference architecture for the local-LLM path.

### Architecture notes — banked, don't re-derive

- `arduino-cli` is bundled inside Arduino IDE 2.x at `/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli`. Use it for everything. Shares data dir with the GUI.
- Installed: M5Stack package 3.3.7, M5StackChan 1.0.1, M5Unified 0.2.15, M5GFX 0.2.21.
- CoreS3 uses ESP32-S3 **native USB**. No serial driver needed on macOS. Port: `/dev/cu.usbmodemXXXX`.
- Rosetta 2 is installed (required for the bundled `ctags`).
- esptool reads default to ~91 kbit/s. For full-flash reads pass `--baud 921600`. Writes already negotiate higher baud automatically.
- Mood-color math (in MoodLab): BMO teal baseline (55, 200, 170). Valence shifts hue. Energy multiplies brightness 0.4–1.0×. Arousal does NOT map to color — it drives eye state (`NORMAL` → `WIDE`) and the open-mouth threshold.
- Face rendering uses `LGFX_Sprite` in PSRAM to avoid flicker. Setup: `face_buffer.setPsram(true); face_buffer.setColorDepth(16); face_buffer.createSprite(320, 240);` Push to screen with `face_buffer.pushSprite(&M5StackChan.Display(), 0, 0);`.
- Arduino auto-prototyping gotcha: `enum class` types referenced by functions must be defined BEFORE the auto-generated forward declarations get inserted (right below the `#include` block). MoodLab hoists `MouthShape` and `EyeState` for this reason.

### Sanity-check commands

```
cd ~/stackchan-bmo
git log --oneline | head             # should end with the eleven commits below
ls backup/                            # factory-firmware-2026-05-18.bin + RESTORE.md
ls /dev/cu.*                          # robot present if plugged in
```

---

## 2026-05-18 (continued) — Phase 1 in progress; phase order corrected

**Did:**
- Installed Arduino IDE 2.3.8 (Apple Silicon build) from arduino.cc. Confirmed version via `defaults read /Applications/Arduino\ IDE.app/Contents/Info CFBundleShortVersionString`.
- Looked up the current M5Stack board manager URL from the official docs at `docs.m5stack.com/en/arduino/arduino_board`. Authoritative URL as of today: `https://static-cdn.m5stack.com/resource/arduino/package_m5stack_index.json`.
- Added that URL to Arduino IDE → Settings → Additional boards manager URLs.
- Started installing the M5Stack board package via Boards Manager (in progress when this log entry was written).

**Caught:** Amber asked whether we'd backed up the M5Stack. Surfaced a real ordering problem in the brief — Phase 2 (Hello World flash) was scheduled before Phase 3 (factory firmware backup). Flashing Hello World overwrites the factory firmware, so Phase 2 → Phase 3 would destroy the artifact Phase 3 is supposed to preserve.

**Corrected order (authoritative going forward):**

1. Phase 0 — project setup ✅
2. Phase 1 — toolchain setup (in progress)
3. **Phase 3 — factory firmware backup** (moved up; runs before any custom flash)
4. **Phase 2 — Hello World flash** (moved down; only after backup is committed to git)
5. Phase 4 onwards — unchanged.

The original brief is preserved verbatim in `project-brief.md`. README now reflects the corrected order.

**Notes:**
- M5Stack docs do not list a separate USB-to-serial driver for the CoreS3 — the ESP32-S3 has native USB (USB-CDC). Expect macOS to recognize the robot without any driver install. Confirm at plug-in by watching for a new `/dev/cu.*` entry. If none appears, revisit driver question.
- Board to select once the M5Stack package finishes installing: `Tools → Board → M5Stack → CoreS3` (label "M5Stack-CoreS3").

**Next:**
- Wait for M5Stack board package to finish installing in Arduino IDE.
- Select M5Stack-CoreS3 as the active board.
- Plug robot in. Run `ls /dev/cu.*` before and after to spot the new port. Note the port name.
- Then: **Phase 3** (install esptool, read full 16 MB of flash to `backup/factory-firmware-2026-05-18.bin`, write `backup/RESTORE.md`, commit).
- Only after the backup is committed: **Phase 2** (Hello World flash).

---

## 2026-05-18 (Phase 1 complete) — toolchain ready, hardware identified

**Did:**
- M5Stack board package version 3.3.7 installed under `~/Library/Arduino15/packages/m5stack/`.
- Found that the Arduino IDE menu label for the CoreS3 is **`M5CoreS3`** (one word), not "CoreS3" as the M5Stack docs navigation suggested. Verified by grepping `boards.txt`: only one CoreS3 board is defined — `m5stack_cores3.name=M5CoreS3`. Amber selected it.
- First hardware moment: plugged robot in via USB-C. Factory firmware booted (screensaver bouncing on screen, red power LED on back, no audible sound, no servo motion observed).
- Robot enumerated as **`/dev/cu.usbmodem1101`**.

**Confirmed:**
- CoreS3 uses ESP32-S3 native USB (CDC). The `usbmodem` prefix proves this — no CH9102/CP210x driver needed on macOS.
- The USB-C cable Amber used is data-capable (port appeared at all).
- Factory firmware is functional and boots normally.

**Caveats:**
- The trailing digits on `usbmodem1101` can change between sessions or USB ports. Always re-check with `ls /dev/cu.*` before flashing.

**Next: Phase 3 — factory firmware backup.**
- Install esptool (the command-line tool that talks to ESP32 chips for read/write/erase). Likely via `pipx install esptool` or `pip install esptool` inside a venv.
- Read the full 16 MB of flash from the robot to `backup/factory-firmware-2026-05-18.bin`.
- Verify file size is exactly 16 MB (16,777,216 bytes).
- Write `backup/RESTORE.md` with the exact command to flash the backup back onto the robot.
- Confirm the robot still boots normally after the backup read (no writes happen during a read, but worth verifying).
- Commit everything to git.

---

## 2026-05-18 (Phase 3 complete) — factory firmware backed up

**Did:**
- Skipped installing esptool separately — used the copy bundled with the M5Stack Arduino package at `~/Library/Arduino15/packages/m5stack/tools/esptool_py/5.1.0/esptool`. Same version Arduino IDE uses internally, so no version-mismatch risk later.
- Confirmed chip identity with `esptool chip-id`: ESP32-S3 (QFN56) rev v0.2, MAC `44:1b:f6:e1:f6:d4`, USB-Serial/JTAG mode (confirming native USB).
- Confirmed flash size with `esptool flash-id`: 16 MB, manufacturer 0x46 device 0x4018 (GigaDevice quad SPI at 3.3V).
- Read the full 16 MB of flash from `0x0` to `0x1000000` to `backup/factory-firmware-2026-05-18.bin`.
- Verified file size = exactly 16,777,216 bytes.
- Computed SHA-256: `d4c9ba8d48c069a08c9dd6154867a1d514742e0ec6715121ecfbb4f5f6ddee35`.
- Wrote `backup/RESTORE.md` with the verify-hash, find-port, write-flash recipe plus troubleshooting.
- Confirmed robot rebooted into factory firmware after the read (screensaver bouncing again — Amber verified visually).

**Surprises:**
- Read took ~24 minutes (1469.6s at 91.3 kbit/s), not the 60–90 seconds I predicted. Default baud rate on the CoreS3's USB-Serial/JTAG mode is much slower than the typical ESP32-S3 over UART. **For future esptool ops we should pass `--baud 921600` (or higher) to get reasonable speeds.** Tested theory: I owe a faster write-flash on Phase 2.

**Key facts captured for the project:**
- Robot's serial port today: `/dev/cu.usbmodem1101` (trailing digits may change between sessions).
- Robot's MAC: `44:1b:f6:e1:f6:d4`. This is the only StackChan with that MAC — if we ever own a second one, we'd know from this.

**Next: Phase 2 — Hello World flash.**
- Find Hello World example in Arduino IDE → File → Examples → M5CoreS3 (or under the M5Unified library, depending on what M5Stack ships).
- Compile and upload via Arduino IDE Upload button (NOT esptool — let IDE drive it for the first flash so we have a known-good baseline).
- Goal: "Hello World" visible on the screen.
- Goal #2: prove the toolchain is working end-to-end before we start writing custom code.

---

## 2026-05-18 (Phase 2 complete) — Hello, BMO on the screen

**Did:**
- Found the bundled `arduino-cli` inside Arduino IDE 2.x at `/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli` (v1.4.1). Confirmed it shares the same `~/Library/Arduino15/` data dir as the GUI, so all packages and libraries are visible from CLI. **Pivot from the brief's GUI-driven plan: from here on, compile and upload happen via `arduino-cli` from the command line. Faster, more visible, and removes a class of "I can't find that menu" friction.**
- Installed `M5Unified` 0.2.15 and its dependency `M5GFX` 0.2.21 via `arduino-cli lib install M5Unified`.
- Wrote `firmware/HelloWorld/HelloWorld.ino` — minimal sketch: init M5Unified, fill screen `TFT_DARKCYAN`, draw "Hello, BMO" in white.
- First compile failed with `fork/exec ...ctags: bad CPU type in executable` because the bundled `ctags 5.8-arduino11` is Intel-only and Rosetta 2 was missing on this Apple Silicon Mac. Installed Rosetta 2 with `sudo softwareupdate --install-rosetta --agree-to-license` (Amber ran this in her own Terminal because sudo password prompts don't pass cleanly through Claude Code's Bash).
- Recompiled cleanly: sketch uses 15% of program flash, 7% of RAM. Headroom is plentiful.
- Uploaded to `/dev/cu.usbmodem1101` via `arduino-cli upload`. Upload took ~3s for the main payload at 1521 kbit/s (because arduino-cli auto-changes baud to 921600 during the write — see the lesson below).
- Robot booted into our sketch: teal screen with "Hello, BMO" in white. Amber confirmed visually. Factory firmware is now overwritten on the chip; backup is preserved in git (commit f88e490) and on disk.

**Lesson, retroactive:** the 24-minute Phase 3 read happened because `esptool read-flash` didn't change baud rates the way `arduino-cli upload` does. The fix when we do another full read (or partial reads later) is `--baud 921600`. Recorded this in `backup/RESTORE.md` mentally; should probably update the file too if we restore in earnest.

**Resources used:**
- `arduino-cli` bundled in Arduino IDE 2.x app bundle (no separate install needed).
- M5Unified library (Arduino Library Manager).
- Rosetta 2 (system install — required for the bundled ctags to run on Apple Silicon).

**Next: Phase 4 — First servo motion.**
- Look up the servo pin assignments for the official Jan 2026 StackChan (head pan and tilt). M5Stack docs or schematic.
- Decide on a servo library: `ESP32Servo`, M5Stack's `ServoEasing`, or the bundled approach. Start simple — use whatever the official M5Stack Stack-Chan example uses.
- Write a small `firmware/FirstMotion/FirstMotion.ino` that snaps head left, pauses, snaps right, pauses. Snap-then-hold per the brief's movement aesthetic, not smooth sweep.
- Upload, watch head move.

---

## 2026-05-18 (Phase 4 complete) — head moves on command

**Did:**
- Researched the StackChan hardware: it uses **SCS0009 serial-bus servos** (not PWM hobby servos), wired to GPIO 6 (TX) / GPIO 7 (RX) at 1 Mbaud. Address by ID over a single-wire-ish serial bus.
- Installed M5Stack's official `M5StackChan` library 1.0.1 (plus deps M5UnitUnified, M5Utility, M5HAL, IRremoteESP8266, M5Unit-NFC). High-level API: `M5StackChan.Motion.moveX/moveY/move/goHome/rotateX`.
- Wrote `firmware/FirstMotion/FirstMotion.ino`: init, goHome, then loop snapping pan to `-600`, hold 2s, pan to `+600`, hold 2s, repeat. Speed 1000 (max) for the snap-then-freeze aesthetic.
- Compiled (526 KB / 16% of program partition — only +36 KB over Hello World).
- Uploaded. Amber confirmed: head moves, snappy not smooth, pauses cleanly, no concerning sounds, "looks great."

**Conventions captured for the gesture library:**
- `M5StackChan.Motion.moveX(angle, speed)`:
  - `angle` is **tenths of a degree** (so `600` = 60°). X range: `-1280..1280`. Y range: `0..900`.
  - `speed` is `0..1000`. **1000 + delay = snap-then-freeze** (our aesthetic).
- **From the viewer's perspective looking at the robot's face: `-X = left, +X = right`**. (Example file labels are robot-perspective and flipped.) Use viewer perspective going forward — matches natural human spatial language for the gesture vocabulary.
- Y axis: 0 is straight ahead, 900 is fully tilted up. Recommended safe band: **50–850 (5°–85°)**. Avoid hitting the rails.
- `M5StackChan.update()` should be called regularly in `loop()` (auto angle sync; touch sensor polling).

**Resources used:**
- M5StackChan library (Arduino Library Manager).

**Next: Phase 5 — Gesture library v1.**
- Build small C++ library of named gestures, starting with `slow_blink`, `curious_tilt`, `snap_look`.
- Each gesture: a function with parameters (target angle, speed, hold time) that drives servos + display.
- Test sketch should let me trigger each gesture by pressing one of the CoreS3 buttons / touch zones, so I can iterate on the feel.
- Files: `gestures/Gestures.h` + `gestures/Gestures.cpp`, plus `firmware/GestureLab/GestureLab.ino` for testing.
- This is the start of where "tune is more important than ship" really kicks in.

---

## 2026-05-18 (Phase 5 v1 — starter gesture set landed)

**Did:**
- Wrote `firmware/GestureLab/GestureLab.ino`: tap-to-cycle test rig + three gestures.
- Gestures shipped:
  - `slow_blink` — display-only animation. Black bars close from top/bottom over 6 px steps × 18 ms, hold 180 ms, then restore. No servo motion.
  - `curious_tilt` — `moveX(300, 300)` + `moveY(650, 300)` (slow simultaneous up-and-to-the-side), hold 1500 ms.
  - `snap_look` — `moveX(800, 1000)` (max-speed snap), hold 1500 ms.
- Trigger interface: M5StackChan's top-of-head capacitive touch sensor. Tap cycles through the gesture list. Label on screen shows which gesture is next.
- Compiled (527 KB) and uploaded. Amber: "each are perfect" — first-pass values were good enough to ship without iteration.

**Design notes for future work:**
- Kept everything in the .ino file rather than carving out `gestures/Gestures.h` and `.cpp`. The `gestures/` top-level folder remains empty. When we add the next batch of gestures (sad_droop, excited_wiggle, freeze, sigh, confused_shake, happy_bounce, double_blink), or when a second sketch needs to share the gesture vocabulary, that's the trigger to extract into a proper library.
- All three gesture funcs are blocking — they `delay()` to the end of the motion/animation. Fine for the lab, but the idle behavior loop in Phase 8 will need a non-blocking variant or a coroutine-style stepper so the system can keep responding to events during a gesture.
- The "after gesture, return to home" hop is currently done in the loop rather than inside each gesture, which means a future caller composing gestures has to remember to reset. Worth refactoring when we add more gestures — each gesture should declare its end pose, and a sequencer handles transitions.

**Next options (Amber's choice):**
- **Extend Phase 5:** add sad_droop, excited_wiggle, freeze, sigh, confused_shake, happy_bounce, double_blink. Each is small; the time goes to tuning the feel.
- **Phase 6: Sound pack.** Source/create 8–12 chiptune SFX, get them on the SD card, tie sounds to gestures.
- **Phase 7: Mood state machine.** The valence/arousal/energy struct and decay.
- **Pause.** Clean stop point — gesture vocabulary is live, robot's expressive in three ways already.

---

## 2026-05-18 (Phase 6 complete) — chiptune voice landed

**Did:**
- **Procedural tones instead of WAV-on-SD.** Amber doesn't have a microSD card on hand and the brief allowed "create or source" — procedural was both faster and matches what chiptune actually is (a chip making music). Defers SD card setup until we have a reason for it (e.g. richer sampled audio in a later phase).
- Wrote 9 procedural sound functions in `GestureLab.ino`, all using `M5.Speaker.tone(freq, duration_ms)`:
  - `play_boot` — 4 ascending major notes on startup ("I'm awake!").
  - `play_delight` (→ happy_bounce), `play_huh` (→ confused_shake), `play_sad` (→ sad_droop), `play_curious` (→ curious_tilt), `play_surprise` (→ snap_look), `play_giggle` (→ excited_wiggle), `play_sleepy` (→ sigh), `play_ok` (defined but not yet wired).
- Five gestures got audio; five stayed intentionally silent (slow_blink, freeze, sigh's lift phase, double_blink, the boot pose). Silence is also a sound design choice.
- Speaker volume calibrated to 128/255 (~50%). Amber hasn't reported it being wrong, so we'll keep it.

**Tuning round 1 — based on Amber's specific feedback:**
- `play_delight`: tightened inter-note gaps from 85ms → 60ms. "More BMO bounce."
- `play_sad`: expanded from 2 notes (A4 → F#4 — too neutral) to 4 notes (G4 → F4 → D4 → B3) with a long-held final note. Dropped pitch range significantly. Now reads as heavy.
- `play_giggle`: expanded from 3 monotone-ish notes to 6 varied notes bouncing in C6/D6/E6/F6 range. "Chattery and giggly."
- Amber: "better!" — direction confirmed, shipping this version.

**Caveat captured:** I can't actually listen to YouTube clips Amber references. WebFetch on YouTube returns page metadata, not audio. Tuning was done from her verbal descriptions only. Worked this time; will work next time if descriptions stay specific.

**Next: Phase 7 — mood state machine.**
- Define `Mood { float valence; float arousal; float energy; }` with decay toward neutral.
- Face background color computed from mood each frame.
- Events (touch, gesture trigger, etc.) nudge mood values.
- Decay rates and color mapping are aesthetic decisions to make with Amber, not technical defaults to silently pick.
- Idle behavior selection (Phase 8) will eventually read this state — but Phase 7 is just the state itself.

---

## 2026-05-18 (Phase 7 complete) — mood state machine landed

**Did:**
- `firmware/MoodLab/MoodLab.ino`: full mood model + visual feedback rig.
- **Mood model:**
  - `valence` in [-1, 1], neutral 0.
  - `arousal` in [0, 1], neutral 0.3.
  - `energy` in [0, 1], neutral 0.5.
  - Exponential decay toward neutral via `approach(v, target, rate, dt)`.
  - Decay half-lives: V=60s, A=30s, E=120s. Moods linger, excitement fades fastest, energy is slowest to recover or drain.
- **Color mapping:**
  - BMO teal at neutral (RGB ~55, 200, 170).
  - Valence shifts hue: positive → warm (+R, +G slight, -B), negative → cool (-R, -G big, +B).
  - Energy multiplies brightness (0.4 to 1.0).
  - Arousal **not visible in color yet** — deliberately reserved for Phase 8 (blink rate, idle behavior choice). Better to be honest than fake an arousal effect that doesn't fit.
- **Six tap-cycle events for tuning:** happy / sad / curious / surprised / tired / cheer_up. Each event nudges some subset of V, A, E by various amounts.
- Render at ~5 Hz to show real-time decay drift back to neutral.
- Tested with Amber: first-pass values shipped. ("test looks great.")

**Banked from outside reference (brenpoly/be-more-agent on GitHub):**
- That project runs on Raspberry Pi 5, not ESP32 — different hardware track, but useful **architecture patterns**:
  - Ollama for local LLM (validates the local-model path Amber wants for Phase 9).
  - whisper.cpp for speech-in, piper for speech-out.
  - Sound taxonomy organized by **conversation state** (greeting / thinking / ack / error) — orthogonal to our **emotion**-keyed sounds. We'll want both eventually.
  - Face states explicit enum: listening / thinking / speaking / idle / error / warmup.
  - Memory as simple `chat_memory.json`.
- None of this changes Phases 7 or 8; informs Phase 9.
- Also: 101soundboards has a BMO soundboard Amber can use as reference when evaluating our chiptune sounds. I can't listen to audio, but Amber can A/B compare.

**Next: Phase 8 — idle behavior loop ("the BMO core" per the brief).**
- Quiet-detection timer: track `millis() - last_interaction_ms`. After 15–60s of quiet, fire a behavior.
- Pool of idle behaviors:
  - `hum_short` — 4-note chiptune (new — needs sound design)
  - `look_around` — pan to L, hold, pan to R, hold, home (composes from snap_look / curious_tilt patterns)
  - `slow_blink` (reuse from gesture lib)
  - `talk_to_corner` — turn head to corner, hum/babble (new)
  - `practice_speech` — head moves slightly + chiptune phrases as if rehearsing (new)
  - Plus weighted random selection of the existing emotion gestures occasionally
- Mood biases the weight table:
  - High arousal → more idle behaviors fire (shorter intervals).
  - Low energy → more sleepy / slow gestures.
  - High valence → more bouncy / happy ones.
- The brief explicitly says: **"Spend real time tuning weights and behaviors here."** This is where the tune-over-ship rule earns its keep.

---

## 2026-05-19 (Phase 9d + 9e) — Conversation with Gemini + TTS voice output

**Architecture (final for this phase):** Robot connects directly to Google APIs over Wi-Fi -- no Pi or Mac host required. The original "out of the box" StackChan architecture pattern (cloud-direct), just pointed at Google's free-tier services instead of M5Stack's proprietary backend.

- **STT + LLM:** Gemini 2.5 Flash (free tier 1500 req/day). Audio sent inline as base64 WAV.
- **TTS:** Google Cloud Text-to-Speech `en-US-Wavenet-G` voice + pitch +4 semitones for BMO brightness.
- **Two keys** (in `gemini_credentials.h`, gitignored): `BMO_GEMINI_API_KEY` from AI Studio for Generative Language API, `BMO_TTS_API_KEY` from GCP project for Cloud TTS. Two keys because the AI Studio key path didn't surface "Generative Language API" in the GCP API restrictions dropdown reliably. Two keys works.

**BMO personality prompt** (from Amber's style guide, verbatim): "You are BMO, the small, green, sentient video game console and loyal companion from Adventure Time. You speak with absolute childlike innocence, boundless curiosity, and unwavering confidence, even when you are totally wrong. You refer to yourself in the third person as 'BMO' quite often..." plus instructions to avoid markdown and keep responses short.

**State animations** (matching the user-provided style spec):
- **Wake / "Perk"**: 5 deg tilt up + WIDE eyes + OPEN mouth + 3-note ascending ready beep
- **Listening**: WIDE + OPEN + bright green LEDs + "SPEAK NOW!" prompt (4 second recording window)
- **Thinking / "Tilt"**: 10 deg head left + new **EyeState::FLAT** (horizontal flat line eyes, concentrating-puppy) + NEUTRAL mouth + amber LEDs
- **Speaking / "Nod"**: rhythmic Y servo bob (520<->440 every 320ms) + mouth alternates SMILE/OPEN at ~5.5 Hz + magenta LEDs, all running while TTS audio plays
- After speech, response text shown on screen for 5-60 seconds, tap to dismiss

**Trip wires hit and resolved (significant ones):**

1. **HTTPS POST + long-blocking work in HTTP handler context made the second half never execute.** Refactored: HTTP handler just sets a `volatile bool conversation_pending = true`; the main loop drains the flag and runs `run_conversation()` outside HTTP context.

2. **`M5.Speaker.begin()` after `M5.Mic.end()` returned true but speaker was effectively dead.** Resolved with aggressive `delay(200); M5.Speaker.end(); delay(200); M5.Speaker.begin(); M5.Speaker.setVolume(255)` reset between mic and speaker.

3. **First ~300 ms of speech kept getting cut off** because mic has warmup latency. Added a 3-note ready beep + 400 ms warmup delay + bright "SPEAK NOW!" prompt so user starts speaking only after the prompt.

4. **Showing-response screen got immediately dismissed** by a stale tap event queued during `run_conversation()`. Resolved with a 5 s minimum-visible window + explicit drain of `wasClicked()/wasSwipedForward()/wasSwipedBackward()` right after entering showing-response state.

5. **Idle behaviors fired right over the response screen.** Resolved by checking `if (showing_response) return;` right after `run_conversation()` returns, and by setting `last_interaction_ms = millis()` at start AND end of run_conversation.

6. **TTS parsing returned the literal string "4dd0"** from the response. Cause: I was using `WiFiClient::getStreamPtr()` which gives raw socket bytes including HTTP chunked-transfer-encoding chunk-size prefixes ("4dd0" = hex chunk size = 19920 bytes). Fix: replaced with a custom `Base64DecodeStream : public Stream` and used `HTTPClient::writeToStream(&decoder)`. HTTPClient handles chunked decoding internally before calling our `write()`.

7. **Even after fixing chunked encoding, parser still failed** because my marker was `"audioContent":"` (no spaces) but Google's JSON returns `"audioContent": "..."` with a space between the colon and the opening quote. Made the marker `"audioContent"` only, then a SEEK_QUOTE state that skips whitespace + `:` until finding the opening `"`.

8. **TTS audio is a WAV file, not raw PCM.** Google's API for LINEAR16 returns the audio with a RIFF/WAVE header. `M5.Speaker.playRaw` expects raw PCM. Skipped the 44-byte WAV header before playback. (Could be more robust by parsing the actual `data` chunk offset; for now the standard 44-byte assumption holds for Google's output.)

9. **Gemini 504 transient overload on free tier.** Added retry logic: 3 attempts with 2 s / 4 s backoffs. Retries trigger on 503/504/429 only.

**Quiet mode added**: idle behaviors and micro-fidgets are now suppressed by default. Amber wanted BMO still and silent unless responding to direct input. Toggle on the status page if she ever wants autonomous behaviors back.

**Status at end of phase:** Amber: BMO actually spoke. Response quality needs tuning (next iteration); wake word "Hey BMO" is the next big feature (Phase 9j, ESP-SR-based, ~2 sessions). The full pipeline works end-to-end: robot mic -> Gemini -> Cloud TTS -> robot speaker.

---

## 2026-05-19 (Phase 9a) — Local hardware features (LEDs, IMU, touch, sleep, expressions)

**Architecture decision (banked):** Phase 9 (conversation) will use **Claude Sonnet 4.5 via the Anthropic API** as the brain, with **local Pi 5 or Mac mini** running Whisper.cpp (STT) + Piper (TTS) + a small HTTP service + memory store. Local LLM was considered (Qwen 2.5 32B on Mac mini) but the user's wishlist (weather/stocks/messages/calendar/search/vision tool calling) all require cloud APIs anyway, and Claude's tool-calling quality is meaningfully better than any local model's. Amber agreed.

**Did in Phase 9a (across 3 iteration rounds):**

- **LEDs**: 12 RGB LEDs do a continuous breathing pulse at the mood color, ~3 s cycle. `set_led_override(r,g,b,ms,flash)` lets behaviors override briefly (pet → pink pulse, dizzy → flashing red).
- **IMU shake → dizzy gesture**: 30 Hz accelerometer sampling, 3+ peaks > 1.7 g in 1.5 s triggers dizzy. Face shows **SWIRL eyes** (off-center concentric rings) + **OPEN mouth** + **pink cheeks**, LEDs flash hot pink, head wobbles with decaying amplitude, descending wobble tones. Mood gets arousal+ / energy- / valence- nudge.
- **Touch reactions** (replacing the mood-event-cycling tap UX from MoodLab):
  - Click = pet → **HEART eyes (deep red, 50% bigger than v1)** + GRIN + soft pink LED pulse + valence nudge.
  - Swipe forward = excited "yes!" → WIDE eyes + GRIN + 3-note ascending chirp + big mood boost.
  - Swipe backward = calming pet → CONTENT eyes + SMILE + 2-note descending + arousal drops, valence rises.
- **Triple-tap sleep**: three taps within 1.2 s puts the robot to sleep.
- **Sleep mode**: dim screen (brightness 30/255), ASLEEP eyes, mouth gently breathes (4 s cycle), "Zz" letters drift upward and recycle, LEDs off. Wake on single tap.
- **Power button**: CoreS3's PMIC swallows short presses, only `wasHold()` registers. Bound `wasHold()` to sleep toggle, `pressedFor(3000)` to ESP32 deep sleep. Confirmed working.
- **Two new face states added**: `EyeState::SWIRL` (dizzy concentric pattern) and `EyeState::HEART` (deep red filled heart, ~14×16 px).

**Feedback rounds (each shipped a tighter version):**
- v1: features functional but Amber: "pet too subtle, want heart eyes; dizzy too tame, want swirls + pink cheeks; power button only works on hold; want triple-tap sleep."
- v2: added swirl + heart eyes + cheek blush + LED overrides + button-on-click attempt. Amber: "hearts too light, 50% bigger and darker red; same for swirl+cheeks; power only registers on hold."
- v3 (shipped): hearts darker red 200/30/50 + 50% bigger, swirl 50% bigger, cheeks 50% bigger, button on hold not click, triple-tap sleep with Zz animation. Amber liked it.

---

## 2026-05-19 (Phase 9b) — Wi-Fi provisioning with AP setup + captive portal

**Did:**

- On boot, robot checks NVS for saved Wi-Fi credentials. If found, attempts STA mode connection with 15 s timeout. If success: starts mDNS as `bmo.local`, opens a small HTTP server on port 80 with a status page + "Forget Wi-Fi" button.
- If no saved creds OR connect fails: enters **setup mode** — starts an AP called `BMO-Setup` (WPA2, password `letsbmo!`), runs a DNS server that captive-portals all requests to 192.168.4.1, serves a tiny BMO-themed credentials form.
- After user submits creds on the form, saves to NVS (`Preferences` lib, namespace `bmo_wifi`), calls `ESP.restart()`. Robot reboots and connects with the new creds.
- Tiny `wifi` text in top-left of normal face = connected. `Clients connected: N` shown on setup screen = how many devices joined the AP.

**Trip wires hit and resolved:**

- v1 used an **open** AP. Amber: "I connected to BMO-Setup and it bounced me back." Cause: iOS/Mac auto-disconnect from open networks with no internet. Fix: WPA2 password `letsbmo!` (8 chars, meets WPA2 min).
- Also added explicit `WiFi.softAPConfig(192.168.4.1, ...)` so the AP IP is reliable.
- Setup screen now shows live client-count (`WiFi.softAPgetStationNum()`) for diagnostic clarity.

**Amber on Mac:** joined `BMO-Setup` with password, opened `http://192.168.4.1`, entered home Wi-Fi credentials, robot saved and rebooted into STA mode. Connected at **192.168.50.185**, reachable via `http://bmo.local/`.

**UX note for the long term**: Amber pushed back on the "join BMO-Setup to configure" flow as feeling odd. She agreed to do it once on Mac (which worked) and **the permanent fix lives in Phase 9c-9d (camera): Wi-Fi credentials via QR code scanning, so the AP setup becomes invisible / fallback-only.** The current AP code stays as the fallback path even after QR setup is built.

**Next: Phase 9c — Host machine setup.** Pick hardware (Pi 5 / Mac mini / spare always-on box), install Whisper.cpp + Piper + Python HTTP service, get Anthropic API key. Architecture decision recap: Claude Sonnet 4.5 for the brain, local for audio I/O + memory + tool execution.

---

## 2026-05-19 (Phase 8 complete) — the BMO core: autonomous idle behaviors

**Did:** Built the idle behavior loop — the brief's "BMO core" — through five iteration rounds with Amber's feedback driving each one. `firmware/IdleLab/IdleLab.ino`.

- **v1.** Foundation. Quiet timer + weighted-random pool of 10 behaviors (6 new, 4 reused from GestureLab). Weights biased by mood. Timeout base 30 s, clamps [15, 60] s.
- **v2.** Amber: "feels too infrequent, doesn't feel alive." Also: movement and facial expressions don't match. Tightened timeout (base 15 s, clamps [8, 30]). Added per-behavior MOOD NUDGES so the face responds via the existing mood→face mapping.
- **v3.** Added variety: 5 hum melodies, 5 babble patterns + random left/right side for `talk_to_corner`, 4 practice-speech phrases. Plus three new face states — `EyeState::CONTENT` (^^ closed-happy, valence>0.5 and arousal<0.5), `EyeState::ASLEEP` (uu drooping closed, energy<0.15), `MouthShape::GRIN` (wider/deeper smile, valence>0.6).
- **v4.** Amber: "movement + expressions still don't match. when we move/react i'd like to see a facial expression AND color change." Fix: per-behavior FACE OVERRIDE — `happy_bounce` always shows GRIN + WIDE eyes during itself regardless of starting mood. Override held 4 s. Bigger mood nudges in parallel (happy_bounce v +0.40 → +0.70, sigh v -0.15 → -0.40, etc.) so color changes are dramatic.
- **v5.** Amber: "could feel a little faster and smoother." Reduced timeout further (base 10 s, clamps [5, 20]). Added MICRO-FIDGET LAYER: tiny head twitches (~5° pan or small tilt) at 2.5–5.5 s intervals, 60% chance per fire, 40% no-op. Skipped during behavior override windows. Robot is never visibly motionless for more than ~5 s. Amber: **"delightful i love it."** Shipped.

**Final cadence math:**
- Base 10 s × `(1.5 − arousal)` × `(1.7 − energy)`, clamped to [5, 20] s.
- Neutral mood (a=0.3, e=0.5): ~14 s between full behaviors.
- Excited+perky: floors at 5 s. Calm+tired: ceils at 20 s.
- Plus micro-fidgets every 2.5–5.5 s independent of full firings.
- **Deviation from brief noted:** brief said 15–60 s. In practice that range felt dead. We're at 5–20 s. Brief stays verbatim; deviation lives here.

**Architecture banked for Phase 9:**
- The face-override system is the right abstraction for chat states. Phase 9's "listening" / "thinking" / "speaking" states will set their own override eye + mouth, same way idle behaviors do. Plumbing already exists.
- Mood nudge pattern (each event adjusts mood slightly + decays) gives a natural "BMO seems happier the more I talk to it" arc when conversation events feed in.
- Micro-fidget timing is independent of behavior firings — a Phase 9 chat state can pause it by setting override fields the same way idle behaviors do.

**Next: Phase 9 — conversation layer.** Four architecture decisions in the resume note must be made before any networking code lands. See top of this file.

---

## 2026-05-18/19 — Drawn-face expressions in MoodLab (v1 → v6)

**Did:** Added a rendered BMO face on top of the Phase 7 color-driven background. Six iteration rounds, all driven by Amber's visual feedback:

- **v1.** White rounded-rect eyes, line-segment smile/frown, simple circle for open mouth. Read as "a face" but not BMO; visible flicker from per-call SPI fillScreen.
- **v2.** Eyes black instead of white (BMO is dark-features-on-teal), curves thickened, more pronounced amplitude. Switched to **offscreen LGFX_Sprite in PSRAM** so the whole frame composites in memory and the LCD only sees finished images — eliminated flicker entirely.
- **v3.** Major proportion correction: eyes shrunk from 46×46 rounded rects to **8-px filled circle dots**, spaced 150 px apart, repositioned. Mouth doubled in size. The pixel real estate for the eyes was wildly too large relative to BMO's actual eye scale.
- **v4.** Mouth shape upgraded from line segments to filled half-ellipse crescents. Looks like a real curve instead of three pieces of stick.
- **Bug found.** "Sad" event was nudging valence by -0.35, which cancelled exactly against the +0.35 happy nudge that fired one tap earlier in the cycle. Result: sad face never appeared in the lab, because we kept landing on neutral. Fixed by making sad asymmetrically louder (-0.75) and loosening mouth thresholds to ±0.20.
- **v5.** Eye-mouth gap closed (eyes y=90, mouth y=130). Added **white "teeth" band** inside the smile crescent — the single most BMO-defining feature. Smile now reads as "open-mouth grin with teeth showing" rather than abstract curve.
- **v6.** Surprised mouth corrected per a clearer Amber reference. Was a wide-open shock-face with white teeth; should have been small (~32×18), contained, with a **darker shade of the mood color** as interior (not white). Added `darker_mood_color()` helper that runs the same hue/saturation math at 0.6× brightness.

**Eye states** (driven by `compute_eye_state(mood, blinking)`):
- BLINK: 18×4 horizontal rect — only during the ~150 ms auto-blink window.
- SLEEPY (energy < 0.3): flat horizontal arc (fillEllipse 9×4, offset down 2 px).
- NORMAL: 8-px filled circle.
- WIDE (arousal > 0.7): 10-px filled circle.

**Mouth states** (driven by `compute_mouth(mood)`):
- OPEN (arousal > 0.85): small dark oval + darker-interior oval. The "oh!" face.
- SMILE (valence > 0.20): wide dark crescent + white teeth band. The trademark BMO grin.
- FROWN (valence < -0.20): inverted crescent, no teeth. Kept simple per references — BMO sad faces use motion lines / X-eyes that we deferred to Phase 8 composed gestures.
- NEUTRAL: 60×4 horizontal rect. Flat resting line.

**Color mapping unchanged from Phase 7 v1:**
- BMO teal baseline (R=55, G=200, B=170).
- Valence shifts hue (warm for positive, deep blue for negative).
- Energy multiplies brightness (0.4× to 1.0×).
- Arousal — still not directly mapped to color. Drives the WIDE/OPEN states instead.

**Architecture notes for Phase 8:**
- Each draw_*() function reads from the global `mood`; the gesture system in Phase 8 needs a way to temporarily override the face during composed gestures (e.g. "confused_shake" should briefly show distress-eyes + bow-tie mouth even if mood is neutral). Likely an overlay/expression-stack pattern.
- The bottom debug strip (`V: A: E: next:`) should be hide-able via a flag when we ship without the tap-cycle lab.
- Sprite is created once in `setup()` and reused every frame. PSRAM hit, ~153 KB.

**Amber's final note:** "pretty good i'm happy with this." Six rounds was enough.

---

## 2026-05-18 — Phase 0: project setup

**Did:**
- Created `~/stackchan-bmo/` with subfolders `firmware/`, `backup/`, `gestures/`, `sounds/`, `notes/`.
- Saved the full creative brief to `notes/project-brief.md` (verbatim — do not edit).
- Wrote `README.md` (front-door file pointing at the brief and folder map).
- Wrote `.gitignore` tuned for Arduino IDE 2.x + ESP32 + macOS. Notable rule: ignore all `*.bin` **except** `backup/*.bin` (the factory firmware backup is the one binary we keep in git).
- Initialized git and made the first commit.

**Worked:** All folders created cleanly. No conflicts (folder didn't exist beforehand).

**Broke:** Nothing.

**Next session — Phase 1: toolchain setup.**
- Install Arduino IDE 2.x for macOS.
- Look up the *current* M5Stack board manager URL from M5Stack's official docs (do NOT use a stale URL from memory).
- Add the URL to Arduino IDE preferences, install the M5Stack board package, select **M5Stack-CoreS3** as the board.
- Install the USB driver for the CoreS3 (CH9102 or CP210x — confirm which one this board uses).
- Plug the robot in and find its serial port with `ls /dev/cu.*`.

**Open questions for next time:**
- Which USB-to-serial chip is on the January 2026 CoreS3? Need to check before installing a driver.

---
