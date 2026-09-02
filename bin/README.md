# Nowde firmware binaries

Built binaries, one per PlatformIO env, refreshed by `copy_firmware.py` on every build:

| File | Board | Notes |
|------|-------|-------|
| `firmware-esp32-s3-devkitc-1.bin` (+ legacy `firmware.bin`) | ESP32-S3 DevKitC-1 | MillluBridge baseline; `firmware.bin` is the name the Bridge OTA uploader expects |
| `firmware-atoms3.bin` | M5Stack AtomS3 / AtomS3 Lite | one binary, role from the board (LCD → master, Lite → slave) |
| `firmware-atoms3-master.bin`, `firmware-atoms3-slave.bin` | same | role forced at build time |

Flash with esptool at `0x10000` (app partition), or over USB-MIDI with the
MillluBridge OTA uploader. Committed bins track tagged releases only.
