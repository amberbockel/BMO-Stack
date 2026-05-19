# Session log

Newest entries on top. After every working session, append a new block: what we did, what worked, what broke, what to do next.

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
