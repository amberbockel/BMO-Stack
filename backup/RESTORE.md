# How to restore the M5Stack CoreS3 to its factory firmware

This folder contains a complete, byte-for-byte copy of the factory firmware that was on the StackChan when the project started. If anything ever goes wrong — a bad flash, a corrupted partition, a custom build that won't boot — you can wipe the robot back to this state in about 25 minutes.

> **Do not delete `factory-firmware-2026-05-18.bin`.** It is the *only* copy of this robot's factory firmware. M5Stack publishes generic factory images, but the per-device NVS partitions (Wi-Fi creds, calibration data, MAC-tied state) live inside this file and can't be re-downloaded.

---

## What's in this backup

| Field             | Value                                              |
|-------------------|----------------------------------------------------|
| Captured on       | 2026-05-18                                         |
| Source robot MAC  | `44:1b:f6:e1:f6:d4`                                |
| Chip              | ESP32-S3 (QFN56), revision v0.2                    |
| Flash size        | 16 MB (16,777,216 bytes — full chip)               |
| File              | `factory-firmware-2026-05-18.bin`                  |
| SHA-256           | `d4c9ba8d48c069a08c9dd6154867a1d514742e0ec6715121ecfbb4f5f6ddee35` |
| Captured with     | `esptool v5.1.0` (from M5Stack Arduino package)    |

---

## Before restoring — verify the backup file is intact

If this file has been sitting on disk for a while, recompute the hash and confirm it still matches.

```
cd ~/stackchan-bmo
shasum -a 256 backup/factory-firmware-2026-05-18.bin
```

The output must be **exactly**:

```
d4c9ba8d48c069a08c9dd6154867a1d514742e0ec6715121ecfbb4f5f6ddee35  backup/factory-firmware-2026-05-18.bin
```

If the hash differs, **do not flash this file** — it is corrupted. Restore from git (`git checkout backup/factory-firmware-2026-05-18.bin`) or from your other backups.

---

## Before restoring — confirm you're restoring to the right robot

A backup from one robot will technically flash onto another StackChan and the robot will boot, **but the MAC address and Wi-Fi calibration will be wrong** (because they're hard-coded into the dump). To check: read the running robot's MAC first.

```
~/Library/Arduino15/packages/m5stack/tools/esptool_py/5.1.0/esptool \
  --chip esp32s3 --port <PORT> chip-id
```

If the MAC esptool reports does **not** match `44:1b:f6:e1:f6:d4`, you are looking at a different physical robot — do not flash this backup to it.

---

## How to restore — step by step

### 1. Plug the robot in via USB-C (data-capable cable)

### 2. Close Arduino IDE if it's open

esptool needs exclusive access to the USB serial port. If Arduino IDE has it open you'll see "port busy" errors.

### 3. Find the current serial port

The trailing number can change between sessions.

```
ls /dev/cu.*
```

The robot will show up as `/dev/cu.usbmodem<some-number>` (e.g. `/dev/cu.usbmodem1101`). Note that exact path — call it `<PORT>` below.

### 4. Run the restore command

Substitute `<PORT>` with what you found in step 3:

```
~/Library/Arduino15/packages/m5stack/tools/esptool_py/5.1.0/esptool \
  --chip esp32s3 --port <PORT> \
  write-flash 0x0 ~/stackchan-bmo/backup/factory-firmware-2026-05-18.bin
```

What this does:

- `write-flash` — write bytes to the chip's flash.
- `0x0` — starting at address 0 (very beginning of flash), which is correct for a full-image restore.
- The path is the 16 MB backup file.

### 5. Wait

Writes are typically faster than reads but still expect **5–25 minutes** depending on USB speed. The screen will be black. **Do not unplug. Do not sleep the Mac.** A partially-written restore can soft-brick the chip (recoverable, but more steps).

### 6. Confirm the robot boots normally

When esptool prints `Hard resetting via RTS pin...` and exits, the robot's screen should come back with the factory screensaver. If it does — restore is complete.

If the screen stays black: try unplugging and replugging the robot. If still black after a fresh power-on, the chip is likely stuck in bootloader mode — see Troubleshooting below.

---

## Troubleshooting

**`A fatal error occurred: Could not open <port>, the port doesn't exist`**
The port name changed since you ran `ls /dev/cu.*`. Re-run that command. Don't assume `usbmodem1101` is permanent.

**`A fatal error occurred: Could not open <port>, [Errno 16] Resource busy`**
Something else has the port open. Quit Arduino IDE, quit any Serial Monitor, then retry.

**`A fatal error occurred: Failed to connect to ESP32-S3: Wrong boot mode detected (0xXX)`**
Chip didn't enter download mode. Try again — esptool usually handles this on its own via USB. If repeatedly failing, hold the side button on the StackChan while plugging in the cable (forces ROM bootloader), then run the command.

**Robot won't boot after a successful-looking restore**
Verify file hash matches (above). If it does, re-run the restore — sometimes a flash write succeeds at the protocol level but a bit got corrupted; second write fixes it.

**Restore appears to hang at 0% for more than a minute**
First write of the session has extra setup overhead. Wait at least 2 full minutes before considering it stuck.

---

## A future-you cheat sheet

For the impatient version, assuming everything is normal:

```
cd ~/stackchan-bmo
shasum -a 256 backup/factory-firmware-2026-05-18.bin    # must match hash above
ls /dev/cu.*                                            # note the usbmodem*
~/Library/Arduino15/packages/m5stack/tools/esptool_py/5.1.0/esptool \
  --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  write-flash 0x0 ~/stackchan-bmo/backup/factory-firmware-2026-05-18.bin
```
