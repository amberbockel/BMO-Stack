# Session log

Newest entries on top. After every working session, append a new block: what we did, what worked, what broke, what to do next.

---

## NEXT SESSION — RESUME HERE

> Pinned section. Read this first when you come back. Last updated 2026-05-19, end of session that built Phases 0–7 + drawn-face expressions.

### Where we are (one paragraph)

Eleven commits in git, latest is `5ec4a25`. The robot has: factory firmware safely backed up (`backup/factory-firmware-2026-05-18.bin` + RESTORE.md), a 10-gesture body language vocabulary (`firmware/GestureLab/`), a 9-sound procedural chiptune voice (also in GestureLab), a mood state machine with decay-toward-neutral over 30–120s (`firmware/MoodLab/`), and a drawn BMO face on the MoodLab screen with white teeth on smile, dark dot eyes, auto-blinks every 3–7s, and a color that shifts with valence and energy. MoodLab v6 is what's currently on the chip.

### First three things to do when resuming

1. **Re-orient.** Read `notes/project-brief.md` (the original goal), this resume section, and the most recent chronological log entry below. ~5 min.
2. **Verify the toolchain still works.** Plug the robot in. Run `ls /dev/cu.*` — note the new port name (the trailing digits can change between sessions). Re-upload MoodLab to confirm flow:
   ```
   CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
   cd ~/stackchan-bmo
   "$CLI" compile --fqbn m5stack:esp32:m5stack_cores3 firmware/MoodLab
   "$CLI" upload  --fqbn m5stack:esp32:m5stack_cores3 --port /dev/cu.usbmodemXXXX firmware/MoodLab
   ```
3. **Start Phase 8** (the BMO core — idle behavior loop). Design below.

### Phase 8 starting plan (draft — expect tuning)

- New sketch: `firmware/IdleLab/IdleLab.ino`. Copies Mood + face rendering from MoodLab, adds an idle behavior loop on top.
- Quiet-detection: track `last_interaction_ms` on every touch; fire an idle behavior when `millis() - last_interaction_ms` exceeds a mood-modulated threshold (15–25 s when aroused, 40–60 s when low-energy, ~30 s baseline).
- Idle behavior pool (new, to be designed):
  - `hum_short` — 4-note chiptune phrase, composing from existing tones.
  - `look_around` — pan L → pause → pan R → pause → recenter.
  - `talk_to_corner` — turn head to a corner, play babbling phrase (multiple chiptune tones).
  - `practice_speech` — small head movements + repeating phrases (rehearsing for a future conversation).
  - `idle_blink` — single slow_blink, nothing else.
  - Plus occasional weighted-random picks of the existing emotion gestures.
- Weight table is mood-biased:
  - High arousal → shorter quiet timeouts, more bouncy picks.
  - Low energy → slow/sleepy picks (sigh, slow_blink), longer timeouts.
  - High valence → happy picks (excited_wiggle, happy_bounce, hum_short).
  - Low valence → quiet picks (sigh, sad_droop, freeze).
- **The real work is tuning the weights and timings** so it feels alive without being annoying. The brief says: *"Spend real time tuning weights and behaviors here."* Expect 30+ minutes of watching the robot and adjusting numbers.

### Phase 9 decisions to make before writing any networking code

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
