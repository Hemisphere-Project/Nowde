# Nowde protocol reference

Three wires, three contracts. All of this is the **v1.2 baseline** as extracted
from MillluBridge; v2 additions are marked. Nothing in the baseline changes
byte layout, ever: new features add commands, message types or trailing fields.

1. [Host ↔ node over USB-MIDI: SysEx `0x7D`](#1-sysex-host--node)
2. [Node ↔ node over ESP-NOW](#2-esp-now-mesh-messages)
3. [Slave node → host: standard MIDI](#3-midi-output-contract-slave--host)

Source of truth: `src/nowde_config.h`, `src/sysex.cpp`, `src/midi.cpp`,
`src/sender_mode.cpp`, `src/receiver_mode.cpp`.

## 1. SysEx (host ↔ node)

Framing: `F0 7D <cmd> <payload…> F7`. Manufacturer id `0x7D` (the
non-commercial id). Every payload byte must be `< 0x80`; multi-byte binary
fields are **7-bit encoded** (below) unless noted as "raw ASCII".

### 7-bit encoding

Each run of up to 7 raw bytes becomes 8 bytes: one MSB byte whose bit *i* is the
top bit of raw byte *i*, followed by the 7 raw bytes with their top bit cleared.
A trailing run of *n* < 7 bytes becomes *n* + 1 bytes. Sizes used below:
4 → 5, 6 → 7, 8 → 10, 16 → 19, 36 → 42.

```
raw:     b0 b1 b2 b3 b4 b5 b6 | b7 b8
encoded: M  b0' b1' … b6'     | M' b7' b8'      M = Σ (bi>>7) << i
```

### Host → node, direct (`0x01`–`0x0F`)

| Cmd | Name | Frame | Effect |
|-----|------|-------|--------|
| `01` | QUERY_CONFIG | `F0 7D 01 F7` | Enables **sender mode** if not active. Node answers `HELLO`, then `CONFIG_STATE` 50 ms later. This is the handshake a host sends on connect. |
| `02` | PUSH_FULL_CONFIG | `F0 7D 02 rfSim(1) delayHi(1) delayLo(1) F7` | RF-simulation knobs for bench testing (random 0..delay ms added to each media-sync send). 14-bit delay. Enables sender mode. Answers `CONFIG_STATE`. |
| `03` | QUERY_RUNNING_STATE | `F0 7D 03 F7` | Sender: answers one `RUNNING_STATE` frame per known slave (throttled to 2 Hz). *(v2)* If the host had been silent for 5 s, a `HELLO` comes first, whatever the role — this is how a host learns a node's role **without** `QUERY_CONFIG`, which would turn a legacy node into a sender. Silent on a v1.2 receiver, so it doubles as a harmless keepalive. |
| `04` | ENTER_BOOTLOADER | `F0 7D 04 F7` | Deprecated, no-op. Use OTA. |
| `05` | OTA_BEGIN | `F0 7D 05 size(4→5) F7` | Sender only. `size` = firmware length, big-endian u32. Starts an `Update` session on the app partition. |
| `06` | OTA_DATA | `F0 7D 06 data(7-bit) F7` | Decoded chunk ≤ 256 bytes, written in order. Errors abort the session with `ERROR_REPORT`. |
| `07` | OTA_END | `F0 7D 07 F7` | Verifies the received size, finalizes, reboots after 2 s. |
| `08` | SET_ROLE *(v2)* | `F0 7D 08 role(1) F7` | `0` slave, `1` master. Stored in NVS, applied on the spot. |
| `09` | SET_LOCAL_LAYER *(v2)* | `F0 7D 09 layer(raw ASCII ≤15) F7` | Sets *this* node's subscribed layer (the `0x11` command targets a remote slave). Stored in NVS. |

### Host → slaves, relayed by the master (`0x10`–`0x1F`)

| Cmd | Name | Frame | Effect |
|-----|------|-------|--------|
| `10` | MEDIA_SYNC | `F0 7D 10 layer(16 raw ASCII, NUL-padded) index(1) position(4→5) state(1) F7` — 27 bytes | Master stamps `meshMillis()` and unicasts a `MediaSyncPacket` to every *connected* slave on `layer`. `position` = milliseconds, big-endian u32 before encoding. `state`: `0` stopped, `1` playing. Send at ~10 Hz while playing; one frame with `state=0` to stop. |
| `11` | CHANGE_RECEIVER_LAYER | host form: `F0 7D 11 mac(6→7) layer(16→19) F7` — 29 bytes | Master looks the slave up by MAC and forwards the mesh form (below). `ERROR_REPORT 05` if the MAC is unknown. |

### Node → host (`0x20`–`0x3F`)

| Cmd | Name | Frame |
|-----|------|-------|
| `20` | HELLO | `F0 7D 20 version(8→10) uptimeMs(4→5) bootReason(1) F7` — 20 bytes. Sent on boot (after USB enumeration) and on every `QUERY_CONFIG`. `version` is the NUL-padded `NOWDE_VERSION` string; `bootReason` is `esp_reset_reason() & 0x7F`. *(v2 appends `role(1) board(1)`; parsers must accept longer frames, the MillluBridge one does.)* |
| `21` | CONFIG_STATE | `F0 7D 21 rfSim(1) delayHi(1) delayLo(1) F7` — 7 bytes. *(v2 appends `role(1) board(1) layerLen(1) layer(ASCII…)`.)* |
| `22` | RUNNING_STATE | per chunk: `F0 7D 22 uptimeMs(4→5) meshSynced(1) totalSlaves(1) chunkIndex(1) chunkCount(1) slavesInChunk(1) [slave(36→42)] F7`. One slave per chunk. Slave record, raw: `mac(6) layer(16) version(8) lastSeenMs(4 BE) active(1) mediaIndex(1)`. Only *connected* slaves are listed. |
| `23` | OTA_ACK | reserved, unused in v1.2 |
| `30` | ERROR_REPORT | `F0 7D 30 code(1) ctxLen(1) ctx(≤32) F7`. Codes: `01` CONFIG_INVALID, `02` SYSEX_PARSE_ERROR, `03` ESPNOW_SEND_FAILED, `04` MESH_CLOCK_LOST_SYNC, `05` RECEIVER_TIMEOUT, `FF` UNKNOWN. |

Unknown commands answer `ERROR_REPORT 02` with the command byte as context.
Frames whose second byte is not `7D` (universal SysEx, MTC full-frame…) are
ignored silently.

### USB descriptors

VID `0x303A`, PID `0x8000`, manufacturer `Hemisphere`, product `Nowde - XXXXXX`
(last three MAC bytes, upper-case hex). The version is **not** in the descriptor
(macOS caches MIDI names); read it from `HELLO`.

## 2. ESP-NOW mesh messages

All nodes are in `WIFI_STA` mode, disconnected, with the broadcast peer added.
The first payload byte selects the message. Structs are `packed`, integers are
native little-endian.

| Type | Struct | Direction | Cadence |
|------|--------|-----------|---------|
| `MCK…` | `MeshClockPacket` (3-byte magic + 56-bit µs) | any → broadcast | ESPNowMeshClock, ~1 s ± 10 %. Handled first by `meshClock.handleReceive()`. |
| `0x01` | `SenderBeacon { type }` | master → broadcast | 1 s. A slave registers the sender as a peer; sender entries time out after 5 s. |
| `0x02` | `ReceiverInfo { type, layer[16], version[8], mediaIndex }` | slave → each known master, unicast | 1 s + 0–200 ms jitter. The master marks a slave *missing* after 5 s and drops it after 10 s. |
| `0x03` | `MediaSyncPacket { type, layer[16], mediaIndex u8, positionMs u32, state u8, meshTimestamp u32 }` | master → each connected slave on the layer, unicast | on every host `MEDIA_SYNC`, i.e. ~10 Hz |
| `0xF0…` | SysEx frame, mesh form of `CHANGE_RECEIVER_LAYER`: `F0 7D 11 layer(raw ASCII, unpadded) F7` | master → one slave | on demand. The slave stores the layer in NVS and re-announces itself. |
| `0x04` | `MidiEvent` *(v2, reserved)* | master → slaves | Note / CC / PC relay scheduled on mesh time |

### Slave-side handling of `MediaSyncPacket`

1. Drop unless `layer` matches the subscribed layer *(v2: or the subscribed layer is `*`)*.
2. `delta = meshMillis() − meshTimestamp`. If `|delta| > 200 ms` the mesh clock is not
   trusted yet: drop the packet (logged once per second).
3. If playing and `delta > 0`, `position += delta` (the packet aged in flight).
4. Store index / position / state, reset the local clock base, clear *link lost*.
5. Index changed (and ≠ 0) → send `CC#100 = index`. Playing → stopped → send `CC#100 = 0`.
   While playing, re-send `CC#100 = index` every 1 s.

Between packets the slave advances the position on its own clock and emits MTC
at 30 fps. No packet for **10 s** while playing → *link lost*: stop the clock and
send `CC#100 = 0` (`stopOnLinkLost`), or keep freewheeling.

### Timing constants (`nowde_config.h`)

| Constant | Value |
|----------|-------|
| `SENDER_BEACON_INTERVAL_MS` / `RECEIVER_BEACON_INTERVAL_MS` | 1000 |
| `SENDER_TIMEOUT_MS` / `RECEIVER_TIMEOUT_MS` | 5000 (missing), 10 000 (removed) |
| `MTC_FRAMERATE` | 30 fps |
| `CC100_REPEAT_INTERVAL_MS` | 1000 (0 disables) |
| `LINK_LOST_TIMEOUT_MS` | 10 000 |
| `CLOCK_DESYNC_THRESHOLD_MS` | 200 |
| `TRANSMISSION_DELAY_US` | 1300 (ESPNowMeshClock one-way estimate) |
| `ESPNowMeshClock(...)` | interval 1000 ms, slew α 0.25, large step 10 ms, timeout 5 s, jitter 10 % |
| `MAX_SENDERS` / `MAX_RECEIVERS` | 10 / 10 |

## 3. MIDI output contract (slave → host)

- **`CC#100`, channel 1**: media index `1..127`, `0` = stop. Repeated every second
  while playing.
- **MTC quarter-frames** (`F1 0n`…`F1 7n`), 30 fps non-drop (rate code 3): all eight
  pieces are sent back to back every 1/30 s, so a host that reassembles on piece 7
  gets a full timecode 30 times a second. Hours wrap at 24.
- *(v2)* **MTC full-frame** `F0 7F 7F 01 01 hh mm ss ff F7` on start and on jumps.
- *(v2)* **MIDI Start** (`FA`) on stopped → playing, **Stop** (`FC`) on playing → stopped.
- *(v2)* Relayed **Note / CC / Program Change** from the master host, on their
  original channel, emitted at their scheduled mesh time.

## Firmware tasks

| Task | Core | Priority | Period | Job |
|------|------|----------|--------|-----|
| `MIDI_Task` | 0 | `configMAX_PRIORITIES − 1` | 1 ms | USB-MIDI read / SysEx reassembly (512-byte buffer) |
| `ESPNOW_Task` | 1 | 10 | 10 ms | beacons, tables, link-lost, MTC generation, `meshClock.loop()` |

NVS namespace `nowde`: `layer` (string). *(v2: `role` u8 — 0 slave, 1 master, 0x7F auto.)*

## v2 role resolution (boot)

1. `role` from NVS if it is 0 or 1 (`SET_ROLE` stores it).
2. Else the build's `NOWDE_ROLE_DEFAULT` if forced (`atoms3-master` / `atoms3-slave` envs).
3. Else the board, detected by M5Unified: AtomS3 (LCD) → **master**, AtomS3 Lite → **slave**,
   DevKit / unknown → **legacy** (= exact v1.2 behaviour: receiver at boot, sender on `QUERY_CONFIG`).

A resolved **master** enables sender mode at boot and beacons immediately. A resolved
**slave** never becomes a sender: `QUERY_CONFIG` / `PUSH_FULL_CONFIG` only answer.
`HELLO` and `CONFIG_STATE` report the resolved role (0 slave, 1 master, 2 legacy) and
the board id (1 devkit, 2 atoms3, 3 atoms3-lite).

## v2 board UI

- **AtomS3 LCD**, page 0: role banner (coloured like the LED code below), mesh state,
  slaves / masters seen, host link, layer, media index + position, version + channel.
  Page 1 (click): board, role source (auto / nvs), MAC, uptime, mesh age, free heap.
- **AtomS3 Lite LED**: blue booting · purple master without host · yellow blink no
  peer (master: no slave; slave: no master) · cyan synced idle · green synced playing ·
  red link lost while playing · orange mesh clock lost · magenta OTA.
- **Button**: click = next page; hold 3 s = clear NVS (layer + role back to defaults) and
  reboot. Role changes are SysEx-only on purpose (`SET_ROLE`).
