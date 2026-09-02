# Nowde

**ESP32-S3 sync nodes with a USB-MIDI face and an ESP-NOW mesh underneath.**

A Nowde is a small ESP32-S3 box that plugs into a media player, a laptop or any
MIDI host over USB and keeps a group of such hosts in sync over the air. One node
is the **master**: it takes the playback state of its host (media index, position,
play/stop) and broadcasts it on an ESP-NOW mesh. The other nodes are **slaves**:
each regenerates a MIDI Timecode clock and control messages for its own host from
the shared mesh clock ([ESPNowMeshClock](https://github.com/Hemisphere-Project/ESPNowMeshClock),
64-bit microsecond clock, slewed, self-healing), so every host follows the master
within a few milliseconds, with no Wi-Fi network, no router and no IP.

Nowde started life as the firmware folder of
[MillluBridge](https://github.com/Hemisphere-Project/MillluBridge) (Millumin →
ESP-NOW → MIDI / DALI). It is now a standalone component so it can serve other
hosts: the first production use outside Millumin is
[HPlayer2](https://github.com/Hemisphere-Project/HPlayer2) media players.

```
host (master) ──USB-MIDI──▶ Nowde master ──ESP-NOW mesh──▶ Nowde slave ──USB-MIDI──▶ host (slave)
                              beacons,                        ×N            MTC 30 fps
                              MediaSync @10 Hz                              CC#100 index
                              mesh clock                                    Start/Stop
```

Status: **v1.2** (tag `v1.2.0`) is the firmware extracted from MillluBridge,
unchanged, in production on the AnnaTV player fleet. **v2.0** (September 2026, this
branch) adds the M5Stack AtomS3 / AtomS3 Lite target, a board-carried master/slave
role, the layer wildcard, LCD / LED status and MIDI Start/Stop + full-frame on the
slave output. See [Roadmap](#roadmap).

## Roles

| Role   | What it does | Default board |
|--------|--------------|---------------|
| master | Enables *sender mode*: beacons on the mesh, keeps a table of slaves, relays the host's media state to every slave on the same layer. | AtomS3 (with LCD) |
| slave  | *Receiver mode*: subscribes to a layer, locks the mesh clock, regenerates MTC + CC#100 for its host. | AtomS3 Lite (RGB LED), ESP32-S3 DevKitC-1 |

In v1.2 every node boots as a receiver and becomes a sender when its host sends
the `QUERY_CONFIG` SysEx handshake (this is what the MillluBridge GUI does). v2
lets the board carry the role: the same `atoms3` binary detects an AtomS3 and boots
as master, an AtomS3 Lite and boots as slave. A host can override it (`SET_ROLE`,
stored in NVS) and the DevKit keeps the v1.2 handshake behaviour.

**Layers.** Every slave subscribes to a layer name (16 chars, stored in NVS). The
master tags each media-sync packet with a layer and only slaves on that layer
follow it, so several groups can share one mesh. v2 adds the wildcard layer `*`
(follow any layer), the default for slave builds.

## Host contract (what a slave Nowde sends to its host)

This is the contract a player has to implement to be driven by a Nowde. It is
what HPlayer2 speaks (`core/interfaces/nowde.py`) and what the MillluBridge
receivers emit.

- **USB-MIDI port name** `Nowde - XXXXXX` (last three bytes of the MAC). Class
  compliant, no driver. VID `0x303A`, PID `0x8000`.
- **CC#100 on channel 1 = media index.** `1..127` selects a clip, `0` stops. The
  index is repeated every second while playing so a host that boots late catches
  up. What an index maps to is the host's business (HPlayer2 plays the file whose
  name starts with that number: `7_intro.mp4`, `07_intro.mp4`, `007_intro.mp4`).
- **MTC quarter-frames at 30 fps** carry the position while playing. The clock is
  regenerated locally from the mesh clock, so it keeps running smoothly between
  the master's 10 Hz updates and freezes only on link loss.
- **Link lost** (no media-sync packet for 10 s while playing): the node stops the
  clock and sends `CC#100 = 0` (configurable: freewheel instead).
- v2 adds **MTC full-frame** SysEx (`F0 7F 7F 01 01 hh mm ss ff F7`) on start and
  jumps, and **MIDI Start / Stop** on state edges.

The host side of the picture, with the HPlayer2 chase-lock servo that turns MTC
into playback-speed trims, is in [docs/HPLAYER2.md](docs/HPLAYER2.md).

## Host contract (what a master Nowde expects from its host)

A master is driven over SysEx, manufacturer id `0x7D`, 7-bit encoded payloads.
The full command table is in [docs/PROTOCOL.md](docs/PROTOCOL.md). The minimum
a host needs:

| Direction | Message | Purpose |
|-----------|---------|---------|
| host → node | `QUERY_CONFIG` `F0 7D 01 F7` | handshake; enables sender mode on v1.2 nodes; node answers `HELLO` + `CONFIG_STATE` |
| host → node | `MEDIA_SYNC` `F0 7D 10 layer(16) index(1) position(5) state(1) F7` | media index, position in ms, state (0 stopped / 1 playing) — send at ~10 Hz |
| node → host | `HELLO` | on boot and on every `QUERY_CONFIG`: version, uptime, boot reason (v2: role, board) |
| host → node | `QUERY_RUNNING_STATE` `F0 7D 03 F7` | node answers with its slave table and mesh sync state |
| host → node | `CHANGE_RECEIVER_LAYER` | reassign a slave's layer over the mesh |
| host → node | `OTA_BEGIN / OTA_DATA / OTA_END` | firmware update over USB-MIDI |

Any host that can send SysEx can be a master: the MillluBridge Python GUI, an
HPlayer2 in master mode, a script built on `mido`.

## Hardware

- **ESP32-S3 DevKitC-1** — the original board. Native USB port for MIDI, UART
  port for flashing and logs. Env `esp32-s3-devkitc-1`.
- **M5Stack AtomS3** (0.85" LCD, button) — master, v2. Env `atoms3`.
- **M5Stack AtomS3 Lite** (RGB LED, button) — slave, v2. Env `atoms3-lite`.

The AtomS3 family has a single USB-C on the native USB PHY. v2 builds it as a
composite **MIDI + CDC** device, so the same cable carries MIDI, the debug log and
esptool flashing.

## Build and flash

Requires [PlatformIO](https://platformio.org/) (Python 3.10–3.13).

```sh
git clone https://github.com/Hemisphere-Project/Nowde.git
cd Nowde
pio run -e esp32-s3-devkitc-1                # build
pio run -e esp32-s3-devkitc-1 -t upload      # flash (DevKit: use the UART port)
pio device monitor -b 115200                 # logs
```

Every build copies its binary to `bin/firmware-<env>.bin` (the DevKit env also keeps
`bin/firmware.bin`, the name the MillluBridge OTA uploader expects).

AtomS3 / AtomS3 Lite (env `atoms3`, one binary for both):

```sh
pio run -e atoms3 -t upload --upload-port /dev/ttyACM0   # first flash: hold the button
                                                         # while plugging in (download mode)
pio device monitor -p /dev/ttyACM0                       # log over the same cable (CDC)
```

Once the v2 firmware runs, the node enumerates as `Nowde - XXXXXX` with a MIDI port
and a CDC serial port; re-flashing goes through the CDC with esptool's auto-reset.
If a build crashes before USB comes up, hold the button while plugging in again.
`atoms3-master` / `atoms3-slave` are the same build with the role forced.

DevKit notes: flash through the **UART** USB port, plug the **native** USB port
into the host. If upload fails, hold BOOT while plugging. macOS caches MIDI
device names; after a firmware update clear them with
`sudo rm -rf ~/Library/Preferences/com.apple.audio.midi*` and replug.

## MillluBridge compatibility

The `0x7D` SysEx protocol, the ESP-NOW packet layouts and the `bin/firmware.bin`
OTA flow are frozen as the compatibility baseline. New features only ever add
commands, message types or trailing fields; they never change an existing byte.
A MillluBridge GUI drives a v2 node without modification, and v1.2 and v2 nodes
share a mesh. Tag `v1.2.0` is the last firmware that lived inside MillluBridge.

## Repository layout

```
platformio.ini      build envs (one per board)
src/                firmware (main, sysex, midi, sender_mode, receiver_mode, storage)
bin/                built binaries, consumed by the OTA uploader
docs/PROTOCOL.md    SysEx commands, ESP-NOW packets, MIDI contract, byte layouts
docs/HPLAYER2.md    how HPlayer2 uses a Nowde (slave and master legs)
copy_firmware.py    post-build hook that fills bin/
```

## Roadmap

- **v2.0** — AtomS3 / AtomS3 Lite envs with composite USB, board-carried role,
  `SET_ROLE` / `SET_LOCAL_LAYER` commands, layer wildcard, LCD / LED status,
  MTC full-frame and MIDI Start/Stop on slave outputs. First deployment: the
  six synchronized outdoor players of Biennale de Lyon 2026 (HPlayer2 master).
- **v2.1** — generic MIDI in on the master (MTC, transport, CC#100 from any DAW
  or QLab), Note / CC relay to every slave scheduled on mesh time, a `paused`
  state, a standalone free-running master clock, `tools/nowde-cli`.

## License

GNU General Public License v3.0 or later. Copyright (C) 2025–2026 maigre,
Hemisphere Project.
