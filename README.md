# stackchan-bmo

Custom personality firmware for an M5Stack StackChan (CoreS3 / ESP32-S3) that behaves like BMO from Adventure Time.

The full creative goal, voice rules, gesture library, mood model, and phase-by-phase plan live in [`notes/project-brief.md`](notes/project-brief.md). Read that first.

## Folder map

| Folder      | What lives here                                                          |
|-------------|--------------------------------------------------------------------------|
| `firmware/` | Arduino sketches we write and flash to the robot.                        |
| `backup/`   | Factory firmware backup (`.bin`) and `RESTORE.md` with the recovery cmd. |
| `gestures/` | C++ source for the named gesture library (curious_tilt, snap_look, …).   |
| `sounds/`   | Chiptune `.wav` files for emotional punctuation.                         |
| `notes/`    | `project-brief.md` (the original prompt) and `log.md` (running journal). |

## Where we are

Phase 0 — project setup. See `notes/log.md` for the running session journal.

## Rules

- **Tune over ship.** Feel matters more than features.
- **The factory firmware backup is sacred.** Never overwrite it. Never run a flash erase without confirming the backup exists.
- **Commit after every meaningful step**, with messages that describe what changed.
- **Log every session** in `notes/log.md` — what worked, what broke, what's next.

## Hardware

- M5Stack StackChan (official, January 2026 release)
- CoreS3 controller, ESP32-S3, 16 MB flash, 8 MB PSRAM
- 2" touchscreen, 0.3 MP camera, dual mics, 1 W speaker
- Pan + tilt servos, 12 RGB LEDs, IMU, IR, NFC

## Host

macOS. Beginner-friendly workflow: every command is explained in plain English before it runs.
