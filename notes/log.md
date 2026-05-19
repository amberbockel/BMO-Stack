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
