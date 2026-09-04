# Bench script — Biennale 2026 outdoor sync (v2.0)

The order to run things the first time the boards are on the desk, with what a good
result looks like at each step. Dates from the hub: boards 03/09, firmware frozen 06/09,
series bench 07/09, install 08/09.

Kit: 1 × AtomS3, 1 × AtomS3 Lite (the rest of the Lites wait), USB-C data cables, a laptop
with PlatformIO and `uv`, one RPi from the new batch on RastaOS with HPlayer2 ≥ `52cd319`.
Tools: `uv run tools/nowde-cli.py …` (laptop), `extra/utils/nowde-pi-check.sh` (Pi).

## 1 · Flash and enumerate (both boards, 20 min)

```sh
pio run -e atoms3 -t upload --upload-port /dev/ttyACM0     # hold the button while plugging in the first time
pio device monitor -p /dev/ttyACM0 -b 115200
```

Expect on the CDC log, in order:

```
NOWDE ESP-NOW v2.0 / board: atoms3
[INIT] Board id 2, role stored 127 -> MASTER        (AtomS3)     | Board id 3 ... -> SLAVE (Lite)
[HELLO] Sending 22 bytes
[INIT] WiFi STA mode configured, channel 1
[INIT] ESP-NOW initialized
[INIT] Receiver mode on / Subscribed Layer: *
[INIT] Sender mode on (master role)                 (AtomS3 only)
```

- [ ] `lsusb` shows `303a:8000 Hemisphere Nowde - XXXXXX`; `aconnect -l` lists the MIDI port; a `/dev/ttyACM*` appears.
- [ ] **Board id is 2 on the AtomS3 and 3 on the Lite.** If M5Unified gets it wrong, flash `atoms3-master` / `atoms3-slave` instead and note it: that is the fallback the plan carries.
- [ ] AtomS3 LCD shows the role banner (purple: master, no host yet); Lite LED blinks yellow (slave, alone).
- [ ] Re-flash without touching the button: `pio run -e atoms3 -t upload` (esptool auto-reset through the CDC).
- [ ] `uv run tools/nowde-cli.py hello` on each: HELLO reports `version 2.0`, the role, the board.

## 2 · Two-node mesh (10 min)

Power both. Within ~10 s:

- [ ] AtomS3 LCD: `mesh SYNCED`, `slaves 1`. Lite LED: cyan (synced, idle).
- [ ] `nowde-cli slaves` on the AtomS3 lists the Lite with `layer=*`, `v2.0`, seen < 2 s ago.
- [ ] Power-cycle the Lite: gone from the table after 10 s, back within 5 s of boot.
- [ ] Power-cycle the AtomS3: Lite LED to orange/yellow, then cyan again once the master beacons.

## 3 · Laptop as master host (15 min)

AtomS3 on the laptop, Lite on the laptop too (second port) or on the Pi.

```sh
uv run tools/nowde-cli.py -p "Nowde - <AtomS3>" play 3 -d 20      # index 3, 20 s loop, 10 Hz
uv run tools/nowde-cli.py -p "Nowde - <Lite>" watch               # on the Lite's port
```

- [ ] Lite output: `CC#100 = 3`, `MTC full-frame`, `MIDI START`, then MTC counting at 30 fps, `CC#100 = 3` again every second.
- [ ] MTC on the Lite tracks the streamed position (compare the two counters; mesh + USB add well under 100 ms).
- [ ] Stop the stream (Ctrl-C): `CC#100 = 0`, `MIDI STOP`; LED cyan. Wrap at 20 s: MTC goes back to 00:00:00.
- [ ] Unplug the AtomS3 while playing: after 10 s the Lite sends `CC#100 = 0` + STOP, LED red. Replug and play again: recovers.

## 4 · Pi as slave (20 min)

Lite on the Pi. Media on the Pi named `3_something.mp4` (and a `1_…`, `2_…` to switch).

```sh
sudo /opt/HPlayer2/extra/utils/nowde-pi-check.sh      # READY expected
journalctl -fu hplayer2@biennale | grep -i nowde
```

- [ ] Log: `connecting to 'Nowde - …'`, then `HELLO v2.0 … role=slave board=atoms3-lite`, then `role: SLAVE (HELLO)`.
- [ ] From the laptop, `play 3`: the Pi plays `3_…`, `nowde.qf` ticks, Drifter locks (log `drift` lines shrink into the dead zone within ~10 s).
- [ ] `play 1`: clip switches within a second. Stop: player stops.
- [ ] Loop wrap on the laptop stream: the Pi rides the wrap without a restart (mpv loop=inf set on `player.playing`).
- [ ] Tune if needed from http2: `nowde-jumpfix` (RPi/mmal is not a laptop), `nowde-dance`.

## 5 · Pi as master (20 min)

AtomS3 on a second Pi (or on the first Pi, Lite on another).

- [ ] Master Pi log: `HELLO … role=master board=atoms3`, `role: MASTER (HELLO)`, then `MEDIA_SYNC layer=hplayer2 index=N state=playing`.
- [ ] AtomS3 LCD: `host LINK`, `slaves N`, media line `> N mm:ss` following the master player.
- [ ] Slave Pi follows: same index, locked. Change the master's clip from http2: slaves follow.
- [ ] Reboot the master Pi: slaves stop after 10 s, resume when it is back (no hands).
- [ ] Reboot a slave Pi: rejoins and locks; no effect on the others.

## 6 · Fault and range matrix (30 min, note the numbers)

| Test | Expect | Measured |
|------|--------|----------|
| cold start, 2 nodes → SYNCED | < 10 s | |
| slave lock from clip start (Drifter in dead zone) | < 15 s | |
| residual drift after lock (drifter log) | < 40 ms p95 | |
| master unplug → slaves stop | 10 s | |
| master back → slaves playing | < 5 s | |
| distance AtomS3 ↔ Lite, line of sight, still SYNCED | ≥ 30 m | |
| same, through the player enclosure + a wall | ≥ 10 m | |
| Pi hotspot on next to the nodes (channel 1) | still SYNCED | |

## 7 · Freeze (06/09)

- [ ] `git tag v2.0.0` on the commit the bench ran, bins committed (`bin/firmware-atoms3.bin`, `bin/firmware-esp32-s3-devkitc-1.bin`), CI release attached.
- [ ] README flash procedure re-read against what actually happened on the button.
- [ ] HPlayer2: propose the `biennale` fast-forward (Thomas runs it).

## 8 · Series (07/09, Thomas)

- [ ] Flash 1 × AtomS3 + 5 × Lite with the tagged `firmware-atoms3.bin`; label each with its MAC suffix (LCD info page / `nowde-cli hello`).
- [ ] Mount inside the player enclosures; USB run strain-relieved (the fragile link, per the radar-box lesson).
- [ ] 6-node soak ≥ 1 h: AtomS3 LCD `slaves 5`, every Pi locked, no red LED.

## Bench log

### 2026-09-04 — laptop, 1 AtomS3 (D19268) + 1 AtomS3 Lite (99B52C)

Toolchain: PlatformIO 6.1.18 in a Python 3.13 venv (`~/.platformio/penv313`, see README:
the pioarduino platform refuses the 3.14 the laptop now runs). Nowde `main` + this day's
commits; HPlayer2 `master`.

- **Step 1 ✅** Stock boards flashed through their ROM port (`303a:1001`, no button). Both
  enumerate as `303a:8000 Hemisphere Nowde - XXXXXX`, MIDI + CDC. M5Unified tells the models
  apart: HELLO `role=master board=atoms3` on the AtomS3, `role=slave board=atoms3-lite` on the
  Lite. Boot log as expected (`Board id 2/3, role stored 127 -> MASTER/SLAVE`). Re-flash through
  the node's CDC works **only with** `board_upload.use_1200bps_touch` +
  `wait_for_upload_port` (now in `platformio.ini`): without them esptool's DTR/RTS dance
  sends the node into ROM mode and loses the port. Recovery from ROM mode: esptool
  `--before default_reset --after hard_reset` (a bare hard reset does not bring it back).
- **Step 2 ✅** `slaves` on the AtomS3: `mesh clock SYNCED · 1 slave`, layer `*`, v2.0, seen
  < 1 s. The Lite registers the master's beacon within 1 s of the master booting.
- **Step 3 ✅** `play 3 -d 20` from the laptop: the Lite emits `CC#100 = 3`, MTC full-frame,
  MIDI Start, then MTC at 30 fps (reads 11.63 s at 11.7 s of stream), `CC#100 = 3` every
  second, relay at exactly 10 Hz, mesh compensation +1..2 ms. Loop wrap at 20 s clean
  (MTC 00:00:19:29 → 00:00:20:00 → back to 0). Stop frame → `CC#100 = 0` + MIDI Stop. Master
  rebooted mid-stream and streaming again → the Lite re-arms (CC, full-frame, Start).
  Firmware change from this: a full-frame is now also sent on position jumps > 1 s.
- **Observed once**: `PACKET DISCARDED - Clock desync! Delta=-4294966 ms` on the Lite —
  exactly 2^32 µs: a torn 64-bit read of `_offset` in ESPNowMeshClock (`meshMicros()` is not
  atomic against `_adjust()` running in another task). One packet dropped, harmless, to fix
  in the library.
- **USB link deaths → fixed.** Twice the Lite's CDC log went silent at the first MIDI burst of
  a stream while MIDI kept working, and its last log lines came out at the *next* session:
  the prebuilt TinyUSB/DWC2 loses IN transfers when the CDC and the MIDI endpoints are both
  active. Behind two cascaded hubs the same fault escalated to `disabled by hub (EMI?)` and
  both boards dropped off the bus (chips alive on ESP-NOW). Fix: one USB writer (the MIDI
  task, `src/usb_out.*`) and one active IN endpoint — the CDC stays for the 1200-bps flash
  touch but carries nothing; the node log travels as `LOG` SysEx frames on request
  (`SET_LOG`, `nowde-cli watch`, HPlayer2 `nowde-nodelog`). A queue that nobody drains for
  200 ms is dropped (no ghost stream when a host reopens the port; verified: 0 stale packets
  after 20 s unread). After that: 3/3 streams clean on direct ports, boot log delivered late
  on the first `watch`, 0 kernel USB errors. **Bench the Atoms on the laptop's own ports or a
  powered hub**, never on a bus-powered chain.
- **Link lost ✅** stream without stop frame, master rebooted: 10 s after the last packet the
  Lite logs `LINK LOST`, sends `CC#100 = 0` + Stop; the next stream re-arms it (CC, full-frame,
  Start). Torn mesh-clock reads are now guarded (`meshMillisStable()`): 0 discards since.
- **Step 4 ✅ (player-000, RastaOS 7.x, kernel 6.18, HPlayer2 biennale@122acef)**: Lite on the Pi,
  `HELLO … role=slave board=atoms3-lite` → `role: SLAVE (HELLO)`. `play 1` from the laptop: the Pi
  plays `01_mire.mp4`, the Drifter locks from clip start in ~8 s. With an artificial 20 s loop
  the wrap is a hard seek: jumpFix 500 (the interface default) overshot by 360 ms, 200 lands in
  the dead zone (now the profile default). With the loop at the clip's real length (120 s) the
  wrap goes through mpv's own loop, no seek, re-trimmed within 4 s. Stop → `CC#100=0` → stopped.
  Found on the way: a HELLO before app-run was dropped (role fell back to "assuming v1 node";
  fixed in HPlayer2), a slave that boots before its master played its own content (fixed: the
  node sends `CC#100=0` on first contact with a stopped master, the profile stops on slave role).
- Bench tooling: `nowde-cli play -t SEC` (no signals: `timeout` + `uv` deliver SIGINT twice),
  `--no-stop` for the link-lost test; a CDC capture helper that reboots the node through the
  1200-bps touch and grabs the port before the banner (DTR must be up, or nothing is logged).
- HPlayer2: the loopback tests grabbed the real boards (`^Nowde` matched `Nowde - D19268`
  first) — they now pin `Nowde - SIM`. `profiles/biennale.py`: a Nowde slave ignores its own
  schedule/radar; play0 / the master's role hook respect `schedule.isOpen()` (boot at night
  stays silent).
