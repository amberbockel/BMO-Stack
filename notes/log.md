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
