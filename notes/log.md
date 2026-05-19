# Session log

Newest entries on top. After every working session, append a new block: what we did, what worked, what broke, what to do next.

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
