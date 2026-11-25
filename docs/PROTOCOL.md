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
| `05` | OTA_BEGIN | `F0 7D 05 size(4→5) F7` | Any role, over the node's own USB only (2.0.3 dropped the sender gate; 2.4 refuses raw SysEx over the air but `0x11`). `size` = firmware length, big-endian u32. Starts an `Update` session on the app partition. |
| `06` | OTA_DATA | `F0 7D 06 data(7-bit) F7` | Decoded chunk ≤ 256 bytes, written in order. Errors abort the session with `ERROR_REPORT`. |
| `07` | OTA_END | `F0 7D 07 F7` | Verifies the received size, finalizes, reboots. *(2.4)* Answers with its verdict, `OTA_ACK 7F status dups` (below), and restarts 500 ms after a validated one — from the MIDI task once the verdict and the log are out, no longer a `delay(2000)` that froze that task, so a host saw only a port that vanished. |
| `08` | SET_ROLE *(v2)* | `F0 7D 08 role(1) F7` | `0` slave, `1` master. Stored in NVS, applied on the spot. |
| `09` | SET_LOCAL_LAYER *(v2)* | `F0 7D 09 layer(raw ASCII ≤15) F7` | Sets *this* node's subscribed layer (the `0x11` command targets a remote slave). Stored in NVS. |
| `0A` | SET_LOG *(v2)* | `F0 7D 0A on(1) F7` | `1`: stream the node's own log to the host as `LOG` frames; `0`: stop. Off at boot; the first `1` also delivers the boot log kept since power-up (8 KB). Not stored. Bench / journal aid: `nowde-cli watch` turns it on, HPlayer2 with `nowde-nodelog`. |
| `0B` | SET_LR *(2.0.3)* | `F0 7D 0B mode(1) F7` | Long-range PHY switch. *(2.4)* `mode`: `0` off, `1` LR 250k (what 2.0.3's `on` meant, so a stored `1` keeps its meaning), `2` LR 500k; anything else answers `ERROR_REPORT 01`. Default stays off. Stored in NVS, then the node **restarts** to apply it (the protocol is set before the channel). All-or-nothing across the mesh. Answers `HELLO` before restarting. |
| `0C` | OTA_DATA_ACKED *(2.0.3)* | `F0 7D 0C seq(1) len(1) data(7-bit) F7` | Stop-and-wait `OTA_DATA`: written only if the decoded length matches `len`, answered by `OTA_ACK seq status`. *(2.4)* A chunk with the same `seq` and `len` as the one just written is a resend after a lost ack: answered `0`, **not written** (before 2.4 it was written twice and `OTA_END` refused the image). The host never sends `seq+1` before `seq`'s ack, so a repeat can only be that. |
| `0D` | SET_LOSS_POLICY *(v2.2)* | `F0 7D 0D stop(1) F7` | `1` stop, `0` freewheel, when the `MEDIA_SYNC` stream dies. Stored in NVS and applied **live** — no restart, unlike `SET_LR`. Answers `CONFIG_STATE`. Which one a node wants is a property of where it stands, so it stopped being a build flag. |
| `70` | SET_NEIGHBOURS *(2.4, bench builds only)* | `F0 7D 70 [n × mac(6), 7-bit packed] F7`, n ≤ 8 | Only in builds with `-DNOWDE_BENCH_NEIGHBOURS` (envs `devkit-meshmask`, `atoms3-meshmask`): every ESP-NOW frame from a MAC not listed — the clock's `MCK` included — is dropped before dispatch, so three nodes on one desk play a relay chain. `n = 0` accepts everyone. RAM only; answers `HELLO`. Every other build answers `ERROR_REPORT 02` with the command byte as context. Never on an installation. |
| `0E` | SET_ORIGIN *(v2.2)* | `F0 7D 0E [mac(6→7)] F7` | With a MAC: **pins** the origin lock to that master and holds it through any silence. Without one (`F0 7D 0E F7`, 4 bytes): releases the lock, and the next `MediaSyncPacket` heard adopts its sender. Answers `CONFIG_STATE`. Not stored — a lock is a live fact, not a config. |

### Host → slaves, relayed by the master (`0x10`–`0x1F`)

| Cmd | Name | Frame | Effect |
|-----|------|-------|--------|
| `10` | MEDIA_SYNC | `F0 7D 10 layer(16 raw ASCII, NUL-padded) index(1) position(4→5) state(1) [volume(1) flags(1)] F7` — 27 bytes, 29 with the *(2.0.4)* volume tail | Master stamps `meshMillis()`, mints a `seq`, and sends **one broadcast** `MediaSyncPacket` *(v2.2 — it was one unicast per connected slave on `layer`)*. `position` = milliseconds, big-endian u32 before encoding. `state`: `0` stopped, `1` playing. `volume` 0..100 absolute, carried only when `flags` bit0 is set; a 27-byte frame means *no volume* and nothing is relayed for it. Send at ~10 Hz while playing; one frame with `state=0` to stop. |
| `11` | CHANGE_RECEIVER_LAYER | host form: `F0 7D 11 mac(6→7) layer(16→19) F7` — 29 bytes | Master looks the slave up by MAC and forwards the mesh form (below). `ERROR_REPORT 05` if the MAC is unknown. |

### Node → host (`0x20`–`0x3F`)

| Cmd | Name | Frame |
|-----|------|-------|
| `20` | HELLO | `F0 7D 20 version(8→10) uptimeMs(4→5) bootReason(1) F7` — 20 bytes. Sent on boot (after USB enumeration) and on every `QUERY_CONFIG`. `version` is the NUL-padded `NOWDE_VERSION` string; `bootReason` is `esp_reset_reason() & 0x7F`. Trailers are appended in order and each needs the ones before it; payload offsets counted after the command byte: `[16] role` `[17] board` *(v2)*, `[18] syncQuality` *(2.0.1)*, `[19] lr` *(2.0.3)*, `[20] linkLost(0/1)` `[21..22] msSinceLastMediaSync` (14-bit, `hi7 lo7`, capped at `0x3FFF`; also `0x3FFF` when no MEDIA_SYNC was ever accepted — a master reports that) `[23] watchdogRestarts` (since power-on, capped at 127) *(2.0.5)* — 28 bytes on the wire, `[24..25] syncGaps` (14-bit, `hi7 lo7`) *(v2.2)* — 30 bytes, `[26..35] buildId(8→10)` *(v2.1)* — 40 bytes, `[36] gen` `[37..42] nodeId(6)` `[43] rssi` *(2.4)* — 48 bytes in all. *(2.4)* `lr` `[19]` is the mode: `0` off, `1` LR 250k, `2` LR 500k (a reader before 2.4 reads nonzero as on, which stays true). `rssi` is −dBm of the last frame this node heard straight from its locked master, `0` = unknown (none for 5 s, never, or the node is a master). Parsers must accept longer frames; the MillluBridge one does. A slave never sends `RUNNING_STATE`, so `syncGaps` here is how its **own** host reads the delivery signal. *v2.2 first put `syncGaps` at `[20..21]`; it moved behind the 2.0.5 tail (nowde#t-047) because the garden fleet already sends 2.0.5 and no v2.2 build ever ran in the field. v2.1's `buildId` lands behind `syncGaps` the same way (nowde#t-014): trailers are ordered by when they land, not by version number — as with `MEDIA_SYNC`'s `seq`.* `buildId` is the first 8 bytes of the app ELF SHA-256 the build stamps into the app descriptor (`esp_app_desc_t.app_elf_sha256`): `version` is only a release string, so a bench build and a tagged one read alike, and this is what tells them apart. The same 8 bytes sit at offset `0xB0` of the `.bin` (`0x100B0` in a merged image), so a host checks a unit against a file without flashing it: `nowde-cli build-id bin/firmware-atoms3.bin` against `nowde-cli hello`. *(2.4)* `gen` is the protocol generation: `2` for the 2.x line, `3` for Nowde 3. `nodeId` is the MAC's last three bytes as six upper-case hex ASCII characters, raw (already 7-bit) — the suffix of the USB name `Nowde - XXXXXX`, so a host keys a node without trusting its port name. `version` stays 8 bytes and is read up to its first NUL; ReceiverInfo and RUNNING_STATE NUL-terminate it at `[7]`, so a release string is at most 7 characters (`2.4-rc1`). |
| `21` | CONFIG_STATE | `F0 7D 21 rfSim(1) delayHi(1) delayLo(1) F7` — 7 bytes. *(v2 appends `role(1) board(1) layerLen(1) layer(ASCII…)`; v2.2 appends `stopOnLinkLost(1) originState(1) originMac(6→7)` after the variable-length layer.)* `originState`: `0` following nobody yet, `1` adopted, `2` pinned by `SET_ORIGIN`. This is the read-back for both runtime switches. |
| `22` | RUNNING_STATE | per chunk: `F0 7D 22 uptimeMs(4→5) meshSynced(1) totalSlaves(1) chunkIndex(1) chunkCount(1) slavesInChunk(1) [slave(39→45)] F7`. One slave per chunk. Slave record, raw: `mac(6) layer(16) version(8) lastSeenMs(4 BE) active(1) mediaIndex(1)` *(2.0.1 appends `syncQuality(1)`; v2.2 appends `syncGaps(2 BE)`; 2.4 appends `rssi(1)`, the slave's reading as −dBm, 0 = unknown — 40 raw → 46 encoded)*. Only *connected* slaves are listed. One record per chunk, so a host that reads the older 36/37-byte record simply stops short of the tail. *(v2.1)* The **empty-table chunk** (`slavesInChunk = 0`) appends `alone(1)` — 15 bytes, was 14: `1` when no slave is listed **and** the mesh clock has not heard a single peer since boot. `meshSynced = 0` alone cannot say why; `alone` splits *nobody to be in sync with* (a lone master — a normal single-node rack, paint it neutral) from *had peers, lost them* (`meshSynced = 0, alone = 0` — a real fault). So `alone = 1` implies `meshSynced = 0`. A chunk carrying a record never carries it: a master listing a slave is not alone, and the record stays the frame's tail so it can keep growing. |
| `23` | OTA_ACK *(2.0.3)* | Chunk ack: `F0 7D 23 seq(1) status(1) F7` — 6 bytes, `status` `0` written (or a duplicate, 2.4), `1` bad length (dropped, resend), `2` write failed (session aborted), `3` no session. *(2.4)* `OTA_END`'s verdict: `F0 7D 23 7F status(1) dups(2, hi7 lo7) F7` — 8 bytes, `status` `3` no session, `4` validated (restarting in 500 ms), `5` refused: size mismatch, `6` refused: the image did not validate; `dups` = duplicate chunks acked without being written. A host waits for this frame, not for the port to vanish. An older host reading only `seq status` sees `7F 4` and ignores it. |
| `30` | ERROR_REPORT | `F0 7D 30 code(1) ctxLen(1) ctx(≤32) F7`. Codes: `01` CONFIG_INVALID, `02` SYSEX_PARSE_ERROR, `03` ESPNOW_SEND_FAILED, `04` MESH_CLOCK_LOST_SYNC, `05` RECEIVER_TIMEOUT, `FF` UNKNOWN. |
| `31` | LOG *(v2)* | `F0 7D 31 text(ASCII ≤100) F7`. One log line, only after `SET_LOG 1`. Non-ASCII bytes arrive as `?`. |

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
| `0x02` | `ReceiverInfo { type, layer[16], version[8], mediaIndex }` *(2.0.1 appends `syncQuality u8`; v2.2 appends `syncGaps u16`; 2.4 appends `rssi i8`, dBm of the last frame straight from the slave's locked master, 0 = unknown)* | slave → each known master, unicast | 1 s + 0–200 ms jitter. The master marks a slave *missing* after 5 s and drops it after 10 s. |
| `0x03` | `MediaSyncPacket { type, layer[16], mediaIndex u8, positionMs u32, state u8, meshTimestamp u32 }` — 27 B, *(2.0.4 appends `volume u8`, `flags u8` → 29 B; v2.2 appends `seq u16` → 31 B, **in that order**: 2.0.4 shipped first, so its offsets are fixed and `seq` goes after)* | master → **broadcast** *(v2.2 — it was one unicast per connected slave on the layer)* | on every host `MEDIA_SYNC`, i.e. ~10 Hz |
| `0xF0…` | SysEx frame, mesh form of `CHANGE_RECEIVER_LAYER`: `F0 7D 11 layer(raw ASCII, unpadded) F7` | master → one slave | on demand. The slave stores the layer in NVS and re-announces itself. *(2.4)* The only SysEx a node runs from the air: a `0xF0` frame is executed only if it is `F0 7D 11 …` **and** comes from the master this slave is origin-locked to; anything else (OTA, `SET_ROLE`, `SET_LR`, config…) from anyone in earshot is dropped and logged. Before 2.4 any command ran. |
| `0x04` | `MidiEventFrame` *(v2.3 — byte layout frozen below)* | master → broadcast | Note / CC / PC relay scheduled on mesh time. Batched, see [the event frame](#the-v23-event-frame-0x04). |
| `0x05` | `MeshRelayHeader { origin[6], seq u16, hop u8 }` (10 B) + the original packet verbatim *(v2.2 — #t-024)* | any node → broadcast | controlled flooding for out-of-range / moving slaves. A **new type**, not a trailer on `0x03`: a v1.2 slave ignores an unknown type but *acts* on a longer `0x03`, and it cannot origin-lock. |

### Slave-side handling of `MediaSyncPacket`

1. Drop unless `layer` matches the subscribed layer *(v2: or the subscribed layer is `*`)*.
1b. *(v2.2 **ORIGIN LOCK**)* Drop unless the packet's **origin** is the master this node
   follows. The first one heard is adopted; after that, another master is refused until the
   held one has been silent for `ORIGIN_LOCK_RELEASE_MS`, or forever if `SET_ORIGIN` pinned
   it. Under broadcast delivery nothing else keeps two installations in earshot apart — the
   master's receiver table used to. The origin is a *parameter* of this path, not the recv
   callback's `src_addr`: on `0x05` it is the envelope's, so a relayed copy of the locked
   master's packet is still his.
1c. *(v2.2)* Count the gap in `seq` against the last one accepted from this origin, unless it
   exceeds `MEDIASYNC_MAX_GAP` (a master reboot or a lock switch — re-anchor, do not count).
   The total goes back to the master in `ReceiverInfo.syncGaps`. Broadcast has no MAC-layer
   ACK, so this counter, not `espnowTxFail`, is what names a slave that hears nothing.
2. `delta = meshMillis() − meshTimestamp`. If `|delta| > 200 ms` the mesh clock is not trusted:
   accept the packet **without compensation** and mark it *coarse* (logged once per second).
   *(Dropping it here was the −69 s bug, fixed in 2.0.1: the master's index / position / state
   are authoritative whatever the clocks say — a node loses precision, never correctness.)*
3. If playing and `delta > 0`, `position += delta` (the packet aged in flight).
4. Store index / position / state, reset the local clock base, clear *link lost*.
5. Index changed (and ≠ 0) → send `CC#100 = index`. Playing → stopped → send `CC#100 = 0`.
   While playing, re-send `CC#100 = index` every 1 s. *(v2)* First packet since boot with
   index 0 / stopped → `CC#100 = 0` + Stop once, so a host follows the master's state from
   the first contact (a player that started its own content at boot goes silent).
5b. *(2.0.4)* Frame ≥ 29 B and `flags` bit0 set → send `CC#7 = volume` on change, then every
   1 s while it holds. A master that carries no volume leaves the host's level alone.

Between packets the slave advances the position on its own clock and emits MTC
at 30 fps. No packet for **10 s** while playing → *link lost*: stop the clock and
send `CC#100 = 0` (`stopOnLinkLost`), or keep freewheeling. *(v2.2: which one is a stored
choice, set live with `SET_LOSS_POLICY` and read back in `CONFIG_STATE`; the build flag
`NOWDE_STOP_ON_LINK_LOST` is now only the default for a node whose NVS has never been told.)*

*(2.0.5)* Freewheel is **bounded**: after `NOWDE_FREEWHEEL_MAX_MS` (180 s) without a
packet the slave stops the host exactly like a master-stopped edge (`CC#100 = 0` + Stop),
stops MTC and returns to state 0 (`[LINK] freewheel > 180 s -> stop`); the stream's return is
then a fresh start edge (`CC#100 = index`, full-frame, Start). *Link lost* stays flagged until
a packet clears it. And **while stopped** — once a stop has actually been sent — the slave
repeats `CC#100 = 0` every second to a linked host, the mirror of the `CC#100 = index` repeat
while playing, so a host that missed the stop edge (USB gap) learns it within a second. A node
that never heard its master still says nothing.

### Controlled flooding (`0x05` `MeshRelayHeader`, v2.2 — #t-024)

Relays `MediaSyncPacket` only (v2.2 scope; see the #t-020 note for why beacons/`ReceiverInfo`
are excluded — both key off `info->src_addr`, which a relayed frame does not preserve).
Any node, whatever its role, that hears a `0x03` (hop implicitly 0) or a `0x05` (hop = the
envelope's) may re-broadcast it wrapped one hop further, **unless**:

- it is the packet's own origin (never relay your own traffic);
- this exact `(origin, seq)` has already been forwarded, scheduled, or suppressed — the dedup
  key, taken from the inner packet's own `seq` (never re-minted on relay, so it doubles as the
  receiver's gap-counter key); or
- forwarding would stamp `hop > MAX_HOPS`.

A scheduled forward waits a random `MESH_RELAY_DELAY_MIN_MS`–`MESH_RELAY_DELAY_MAX_MS` ms
window, and is **cancelled** if another node's copy of the same `(origin, seq)` is heard first
("if you hear someone else forward it first, drop yours" — topology note §(2)). The inner
packet is copied **byte-verbatim**, never re-stamped: `meshTimestamp` stays the origin's, so a
relayed copy's own compensation arithmetic (step 2 above) absorbs the relay delay for free —
see the #t-020 note, *"Why the inner packet is copied verbatim"*. Dedup + hop limit make a
flooding loop impossible by construction.

### Timing constants (`nowde_config.h`)

| Constant | Value |
|----------|-------|
| `SENDER_BEACON_INTERVAL_MS` / `RECEIVER_BEACON_INTERVAL_MS` | 1000 |
| `SENDER_TIMEOUT_MS` / `RECEIVER_TIMEOUT_MS` | 5000 (missing), 10 000 (removed) |
| `MTC_FRAMERATE` | 30 fps — *(2.4)* scheduled by frame on `micros()`; before, `last = now` on the 10 ms loop sent 25/s |
| `CC100_REPEAT_INTERVAL_MS` | 1000 (0 disables) |
| `CC7_REPEAT_INTERVAL_MS` *(2.0.4)* | 1000 (0 disables) |
| `LINK_LOST_TIMEOUT_MS` | 10 000 |
| `NOWDE_FREEWHEEL_MAX_MS` *(2.0.5)* | 180 000 (freewheel bound, overridable per env) |
| `CLOCK_DESYNC_THRESHOLD_MS` | 200 |
| `ORIGIN_LOCK_RELEASE_MS` *(v2.2)* | 2000 (silence before an adopted lock may switch master) |
| `MEDIASYNC_MAX_GAP` *(v2.2)* | 100 (largest `seq` jump still read as loss; 10 s at 10 Hz) |
| `MAX_HOPS` *(v2.2, #t-024)* | 3 — frozen at the ratified range's upper bound (topology note §(2): "2–3"); small fleet, moving-node reach favoured over the extra ≤30 ms worst-case latency |
| `MESH_RELAY_DELAY_MIN_MS` / `MESH_RELAY_DELAY_MAX_MS` *(v2.2, #t-024)* | 5 / 30 — random forward delay, unaffected by #t-009 (LR decided OFF 15/09, so the 1 Mbps default the ~6 % airtime figure was computed against still stands) |
| `TRANSMISSION_DELAY_US` | 1300 (ESPNowMeshClock one-way estimate) |
| `ESPNowMeshClock(...)` | interval 1000 ms, slew α 0.25, large step 10 ms, timeout 5 s, jitter 10 % |
| `MAX_SENDERS` / `MAX_RECEIVERS` *(2.4)* | 4 / 15 — was 10 / 10. Each entry is an ESP-NOW peer, as is the broadcast address, and ESP-NOW holds 20 unencrypted peers: 10 + 10 + 1 = 21 failed the last add silently. A room has one master, two during a hand-over, so 4 senders is headroom; receivers are what grows. 4 + 15 + 1 = 20. `MAX_RELAY_ORIGINS` follows `MAX_SENDERS`. |

### The v2.3 event frame (`0x04`)

Type `0x04` was **reserved and left unimplemented** by v2.0 (charter §(b)), so it has no
installed base and no compat constraint — and §(b) freezes it the moment one fielded node
parses it. The layout below is therefore fixed *before* any firmware exists, which is the only
moment it is free. Nothing in this section runs yet: the frame is built by #t-037, scheduled
by #t-038, its lead derived by #t-039 and benched by #t-040.

One master frame carries **many MIDI events**, batched over a 5 ms window. Batching, not
addressing, is where the airtime saving comes from: 14 events in one 226-byte frame instead of
14 separate 37-byte frames, on a radio whose real cost is the per-frame overhead.

#### Header — 30 bytes, then the records

| Off | Field | Type | Meaning |
|-----|-------|------|---------|
| 0 | `type` | u8 | `0x04` |
| 1 | `hdrLen` | u8 | bytes from `type` to the first record. **30** in v2.3. |
| 2 | `flags` | u8 | bit 0 `SNAPSHOT`; bits 1–7 reserved, sent as 0 |
| 3 | `recLen` | u8 | bytes per record, all records. **7** in v2.3. |
| 4 | `groupMask` | u16 | channel mask, bit *N* = MIDI channel *N*+1. `0xFFFF` = every channel. |
| 6 | `seq` | u16 | the origin's **frame** counter for `0x04`, free-running, wraps |
| 8 | `fireAt` | u32 | mesh-time ms at which record 0 fires (see below) |
| 12 | `newCount` | u8 | records that are new in this frame, ≤ `14` |
| 13 | `carryCount` | u8 | records repeated from the previous frame, ≤ `14` |
| 14 | `layer` | char[16] | NUL-padded, exactly as `MediaSyncPacket` and `ReceiverInfo` |

`newCount` records come first, in ascending `offset`; the `carryCount` repeated ones follow.

#### Record — 7 bytes

| Off | Field | Type | Meaning |
|-----|-------|------|---------|
| 0 | `status` | u8 | a MIDI **channel-voice** status byte, `0x80`–`0xEF`, channel in the low nibble |
| 1 | `data1` | u8 | 0–127 |
| 2 | `data2` | u8 | 0–127; **0** for the one-data-byte statuses `0xCn` (Program Change) and `0xDn` (Channel Pressure) |
| 3 | `evSeq` | u16 | the origin's **event** counter, free-running, wraps |
| 5 | `offset` | i16 | ms relative to the frame's `fireAt`; **negative** on a carried record |

System real-time (`0xF8`–`0xFF`), SysEx and MTC are **never** relayed here: transport state
already travels in `MediaSyncPacket.state` and MTC is regenerated locally by each slave from
the mesh clock. A record whose `status` falls outside `0x80`–`0xEF` is skipped; the rest of the
frame is still processed.

#### The one rule that keeps this extensible

**A receiver locates record 0 at `hdrLen` and strides by `recLen` — never by its own compiled
`sizeof`.** That is what lets §(b) hold on a frame whose tail is a variable array: a later
generation appends header fields after `layer` (growing `hdrLen`) or record fields after
`offset` (growing `recLen`), and a v2.3 node still finds every field it knows. Unknown `flags`
bits are ignored, never a reason to drop a frame.

Validation, in order — the same *reject-only-too-short* posture as the SysEx parsers:

1. `len < 30`, or `hdrLen < 30`, or `recLen < 7` → drop.
2. `len < hdrLen + (newCount + carryCount) × recLen` → drop, the frame is truncated.
3. `len` greater than that → **accept** and ignore the tail.

#### `fireAt`, and why events are scheduled rather than played on arrival

`fireAt` is mesh time in ms (`meshMillis()`), the same clock and the same unit as
`MediaSyncPacket.meshTimestamp`; it wraps at 2^32 ms ≈ 49.7 days, so compare it as
`(int32_t)(fireAt − meshMillis())` and never as an unsigned difference. A slave **schedules**
each record for `fireAt + offset` instead of emitting it on arrival, which is what makes a
chord land together and keep doing so across a relay hop. Millisecond granularity costs
nothing here: every record of a chord shares one `fireAt + offset`, so they leave in the same
tick — the quantisation moves the whole chord, never its notes apart.

The sender computes `fireAt = <capture mesh time> + MIDI_EVENT_LEAD_MS`. **The lead is not a
wire field**, so it can change without touching this frame. #t-039 derived it from the
flooding parameters #t-024 froze: `MAX_HOPS` (3) × `MESH_RELAY_DELAY_MAX_MS` (30 ms) =
**90 ms** of worst-case relay latency before clock error (`src/nowde_config.h`, overridable
per env). A lead shorter than that makes relayed events fire late, silently, and only on the
far side of a hop.

With `newCount == 0` — a pure carry-over frame, or a `SNAPSHOT` — `fireAt` is the origin's
`meshMillis()` at mint.

#### Carry-over: depth 1, and what the 250 B actually buys

Instead of retries, **each frame repeats the previous frame's new records** with their
`offset` re-based on the current `fireAt` (so they arrive negative). A slave that missed a
frame still gets its events one frame later; `evSeq` is what stops the node that received
both from firing them twice.

**The depth is 1 frame, and it carries only the previous frame's *new* records — never its
carried ones.** Repeating repeats would grow a frame geometrically and put an unbounded
history on a bounded radio. That single choice is what bounds the frame, and with it the
budget closes:

| Quantity | Value | Where it comes from |
|----------|-------|---------------------|
| ESP-NOW payload MTU | 250 B | the radio |
| relay envelope | 10 B | `0x05` `MeshRelayHeader { type, origin[6], seq u16, hop u8 }`, built by #t-024 and `static_assert`ed at 10 B. A forward re-wraps the inner packet and never nests envelopes, so it costs 10 B at any hop count |
| header | 30 B | table above |
| record | 7 B | table above |
| **new records per frame** | **14** | `(250 − 10 − 30) ÷ 7 ÷ (1 + depth)` = 15, taken down one: 15 lands on 250 B exactly, leaving no margin should the envelope ever grow |
| records on the wire | ≤ 28 | `newCount + carryCount` |
| **frame size** | ≤ **226 B** | `30 + 28 × 7`; **236 B** wrapped in the relay envelope, 14 B spare |

A `0x04` frame **must** survive that envelope — a chord is expected to land at two hops — so
250 B is not the budget, 240 B is. This is the reconciliation the design note owed: §(4) wrote
*"≤ 40 events per frame"* before the per-event size existed and before flooding had an
envelope, and 40 does not fit — `30 + 40 × 7 + 10 = 320 B`. **40 becomes 14 new + ≤ 14
carried.** `tools/selftest.py` asserts the arithmetic both ways, so the day #t-024 widens the
envelope the gate says so instead of the fleet.

The cap bounds a *frame*, never the stream: a 5 ms window that produces more than 14 events
emits a second frame immediately, in the same tick. **Nothing is ever dropped for the cap** —
which is the only reason a number this small is safe to freeze. Sustained, it is 2 800
events/s, some 23× the densest stream this link is known to carry (the 120 msg/s MTC flood of
`docs/usb-in-stall-plan-2026-09-17.md`).

#### Relaying a `0x04` frame: three things the v2.2 relay does not do yet

The v2.2 relay (#t-024) carries `MediaSyncPacket` only, and three of its choices depend on
that. None of them changes this frame. All three are #t-037's to change before any node wraps
a `0x04` frame in `0x05`:

1. **The `0x05` receive path never reads the inner `type`.** It hands every inner packet to
   the `MediaSync` parser. A node subscribed to `*` would accept a relayed `0x04` frame as a
   `MediaSync` from the master it follows, and read byte 17 (the fourth character of the
   frame's `layer`) as the media index. A `0x05` receiver must dispatch on the inner `type`
   and ignore a type it does not play. This is cheapest in the first relay build that reaches
   the fleet: every node fielded without that check has to be reflashed before any master
   relays a `0x04` frame.
2. **Relay dedup keeps one `lastSeq` per origin.** `MediaSyncPacket.seq` and this frame's
   `seq` are independent counters from the same origin, so `0x04` needs the key
   `(origin, type, seq)` or a table of its own. Sharing one entry either forwards duplicates
   or suppresses a frame that nobody forwarded.
3. **A pending forward holds `sizeof(MediaSyncPacket)` (31 B)**, and `handleFloodCandidate`
   clamps the inner packet to that size. A `0x04` frame needs 226 B; truncated, it fails
   validation step 2 at every node downstream.

#### Slave-side handling of a `0x04` frame

1. Drop unless `layer` matches the subscribed layer (or the subscribed layer is `*`) — the
   same filter as `MediaSyncPacket`, and in v2.3 the *only* filter: see `groupMask` below.
2. Drop unless the frame's origin is the master this node follows — the origin lock that
   arrived with broadcast `MediaSync` (#t-021) applies unchanged, and for the same reason:
   under broadcast delivery nothing else keeps two installations in earshot apart.
3. For each record, skip it if `evSeq` was already fired (dedup), else schedule it at
   `fireAt + offset`.
4. A record whose time has already passed by more than `MIDI_EVENT_MAX_STALE_MS` is dropped;
   one inside that window fires **immediately**. A Note On 90 ms late still beats a Note On
   lost, which is the entire point of carry-over. 150 ms is the stated default: 60 ms above
   the 90 ms lead, below the 200 ms `CLOCK_DESYNC_THRESHOLD_MS` at which the mesh clock stops
   being trusted at all. It is slave-side policy, not a wire field: #t-039 set the lead and
   left this unchanged, and it moves with the lead if the lead ever does.
   *(#t-038)* Rules 3–4 schedule on a **trusted** clock only. A frame whose clock this node
   does not trust plays on arrival and nothing in it is stale — see *The schedule* below.
5. Re-emit on the record's original channel (a slave-side remap is configuration, never wire).

**Where the scheduler has to live.** `ESPNOW_Task` runs on a **10 ms** period, which is coarser
than the accuracy this frame exists to deliver — a chord scheduled there lands with ±10 ms of
tick jitter and `fireAt` buys nothing. The due-record check belongs on **`MIDI_Task`'s 1 ms
tick**, which is also the only context allowed to write TinyUSB (see *one writer, one active
IN endpoint*). Relayed events reach the host through `usbOutMidi()` like everything else.

#### The schedule *(#t-038)*

What the slave does between a record's arrival and its fire time. All of it is slave-side
policy: none of it is on the wire, and a master never sees it.

- **Two contexts, one queue.** The ESP-NOW receive callback (WiFi task) runs rules 1–4 and
  hands each surviving record, with the mesh time it fires at, to `MIDI_Task` through a
  FreeRTOS queue (`MIDI_EVENT_HANDOFF_DEPTH`, two full frames). `MIDI_Task` owns the schedule
  outright, no lock, and on every 1 ms tick — *before* the USB pump, so a due record leaves in
  the same tick — takes what was handed over and fires everything due. A hand-off found full
  means `MIDI_Task` is starved; that record plays on arrival rather than being lost.
- **Fire order is time order, and stable.** Records due the same millisecond leave in the
  order they arrived, which is the order the master captured them: a chord stays together,
  and a Note Off followed by a Note On on one key stays a retrigger instead of becoming a
  silent key. Times compare as `(int32_t)` differences, so the 2^32 ms wrap is harmless.
- **Depth: 266 records, derived.** `MIDI_EVENT_SCHED_DEPTH = (lead ÷ batch + 1) × 14` — every
  record the frame cap can carry across one lead plus one batch window, `(90 ÷ 5 + 1) × 14`,
  2.1 KB. The frame caps bound a frame and never the stream (see *Carry-over*), so the depth
  is sized off the cap's rate, not off one frame.
- **Full: the earliest fires early, nothing is dropped.** A record arriving at a full schedule
  makes the earliest held record — or itself, if it is due sooner — fire at once. Precision is
  lost, the event never is, and what is held still leaves in time order. Logged once a second.
- **The clock check, once per frame.** The frame does not carry its mint time, but it states
  it: `fireAt − lead` for a frame with new records, `fireAt` itself for a pure carry-over or a
  `SNAPSHOT`. When that differs from this node's `meshMillis()` by more than
  `CLOCK_DESYNC_THRESHOLD_MS` (200 ms, the threshold MediaSync uses, transit included as it is
  there) the mesh clock is not trusted for this frame: its records **play on arrival**, as
  #t-037 played every record, and **none is stale** — lateness measured on a clock that is
  itself in doubt says nothing about the record. This is MediaSync's rule 2 for the same
  reason: without it, a node 300 ms ahead of its master would drop every event as stale, and a
  node stranded 69 s behind (the 2026-09-07 failure) would hold every event for 69 s. Logged
  once a second as `COARSE`. The estimate uses this node's own `MIDI_EVENT_LEAD_MS`; a master
  built on another lead moves it by the difference, well inside 200 ms.
- **A `SNAPSHOT` yields to the stream.** Snapshot records still play at once — except for a
  controller with a scheduled record still waiting: the master sends a snapshot only after its
  live events are out, so that waiting record is the newer value of the two, and playing the
  snapshot first would jump the controller ahead by the lead, then back.

`tools/midi_schedule_test.cpp` runs the schedule (`src/midi_schedule.h`) on the host, under
`tools/selftest.py`: order, stability, the wrap, the full case, the clock check at 300 ms ahead
and 69 s behind, and the stale boundary.

#### `groupMask`, and the assumption v2.3 ships instead of a decision

The field is on the wire from the first frame; **v2.3 does not use it.** Masters send `0xFFFF`
and slaves ignore it, filtering `0x04` on the layer they already carry, exactly as they filter
`MediaSync`. That is a *written, reversible assumption* — no job in the hub today wants the
MIDI split to differ from the layer split — and not a ruling that they never should.

Reversing it is additive on the wire and the frame does not change, which is why the field
ships now. But it is **not transparent on the mesh**: a v2.3-vintage node ignores the mask, so
a later master that narrows it to one group would have that node fire events addressed to
another. Turning group addressing on therefore costs a reflash of **every node on the affected
layer**, not one row of firmware on the master. Cheap, and worth knowing before the day it is
wanted rather than after.

#### `SNAPSHOT` (`flags` bit 0)

A periodic (≈1 Hz) full CC state dump for the layer, so a node that joined after the
carry-over window closed converges instead of waiting for the next change. Records are applied
as **current state, immediately** — `offset` is 0 and nothing is scheduled — and a snapshot
carries controller messages only: replaying a Note On from a snapshot would retrigger it.
Built by #t-037 with the frame (the master walks its CC table about once a second,
≤ 14 records per frame, one batch window apart; a slave plays each controller record at once);
the bit is frozen here so the frame does not have to change to gain it. *(#t-038)* At once,
unless that controller has a scheduled record still waiting — see *The schedule*.

#### Frame `seq` and record `evSeq` are two counters, on purpose

`seq` identifies a **frame** and is what #t-024's relay envelope dedups on, so a flooded copy
is forwarded once. `evSeq` identifies an **event** and is what carry-over dedups on, so a
repeated record fires once. Two layers, two jobs: collapse them into one counter and you lose
either the repeat (the carried copy looks like a duplicate frame) or the relay suppression (a
frame arriving twice by two paths looks like new events).

#### What a node that does not know `0x04` does

Nothing: `onDataRecv()` switches on the first byte and an unrecognised type falls through
`default:`. So a v1.2 node, and every v2.0–v2.2 node, ignores a `0x04` frame rather than
mis-parsing it — by construction, not by a length check. This is the half of the charter that
lets v2.3 masters share a mesh with fielded slaves.

## 3. MIDI output contract (slave → host)

- **`CC#100`, channel 1**: media index `1..127`, `0` = stop. Repeated every second
  while playing.
- *(2.0.4)* **`CC#7`, channel 1**: the master's absolute volume, `0..100`, only when the
  frame carries one. Sent on change, repeated every second.
- **MTC quarter-frames** (`F1 0n`…`F1 7n`), 30 fps non-drop (rate code 3): all eight
  pieces are sent back to back every 1/30 s, so a host that reassembles on piece 7
  gets a full timecode 30 times a second. Hours wrap at 24.
- *(v2)* **MTC full-frame** `F0 7F 7F 01 01 hh mm ss ff F7` on start and on position jumps > 1 s (loop wrap, seek).
- *(v2)* **MIDI Start** (`FA`) on stopped → playing, **Stop** (`FC`) on playing → stopped.
- *(v2.3)* Relayed **channel-voice messages** from the master host —
  Note, CC, Program Change, Channel Pressure, Pitch Bend — on their original channel, emitted
  at their scheduled mesh time, in the order the master's host sent them. Earlier only in the
  two degraded cases [*The schedule*](#the-schedule-t-038) names — a full schedule, a mesh
  clock this node does not trust — where a late or early event beats a lost one. The transport is
  [the `0x04` event frame](#the-v23-event-frame-0x04). A host sees ordinary MIDI: there is no
  new message for it to learn, and **nothing above changes** — `CC#100` and MTC keep their
  meaning and their cadence, so a player written against the v1.2 contract is not touched by
  v2.3 arriving on the same port.

### USB: one writer, one active IN endpoint *(v2)*

Every packet to the host — MIDI events, SysEx replies, LOG frames — is queued and written by
the MIDI task alone (`src/usb_out.*`); the ESP-NOW callback and the ESP-NOW task never touch
TinyUSB, so a slow host can not stall the MTC generator. The composite CDC interface is kept
for the 1200-bps flash touch but carries no data: on the bench (2026-09-04) the prebuilt
TinyUSB/DWC2 lost IN transfers whenever the CDC and MIDI endpoints were both active (log
silent until reboot, or the hub dropping the device). `-DNOWDE_LOG_CDC` puts the log back on
the CDC for debugging.

### Serial host link *(Nowde 3 alpha, #t-050 — not yet run on a board)*

A node built with a serial host link (envs `devkitc-uart`, `devkitc-cdc`) carries **the same bytes**
as the USB-MIDI packets' payload, nothing added: SysEx frames `F0 7D … F7` and short MIDI
messages, status first. The node never sends running status and accepts it on input; real-time
bytes may arrive anywhere, a status byte inside a frame drops that frame. `devkitc-uart`: the
DevKitC-1's UART port (its USB-UART bridge), **921600 8N1** (`NOWDE_HOST_BAUD`), the log as LOG
frames (`0x31`) on the same link while `SET_LOG` is on. `devkitc-cdc`: the S3's native port as
hardware CDC (no TinyUSB, no MIDI endpoint), the log on the UART port. Any byte from the host is
the link's heartbeat, as any packet is on USB. A port nobody drains for 200 ms drops what is
queued, the USB rule. Host side: open the port with DTR/RTS held off (an open resets a
DevKit-style board), find the node by its HELLO, never by « first port ».

While no host reads the MIDI port the TinyUSB FIFO fills and the queue behind it stalls; after
200 ms of stall the queue is dropped, so a host opening the port later never receives a ghost
stream of stale MTC / `CC#100` (it gets at most the FIFO's 16 packets, and a fresh `CC#100`
within a second while playing). A momentary stall (a `RUNNING_STATE` burst) keeps its order.

`meshMillis()` is read through `meshMillisStable()` (two agreeing reads): the library's
64-bit offset is slewed by another task and a torn read comes back off by 2^32 µs.

## v2.1 · Generic MIDI in (master, not yet run on a board)

A master node also accepts **plain MIDI** from its host, so a DAW, QLab, Millumin without
the Bridge, or a keyboard can drive the mesh without speaking `0x7D` SysEx:

| Host sends | Node does |
|------------|-----------|
| MTC quarter-frames (`F1 0n`…`F1 7n`) | position = decoded timecode + 2 frames (the 8-piece sequence spans two frames), running = true; 24 / 25 / 29.97 / 30 fps from the rate bits |
| MTC full-frame `F0 7F 7F 01 01 hh mm ss ff F7` | position jump |
| `CC#100 = N` (any channel) | media index N; `0` = stop |
| Start `FA` | position 0, running |
| Continue `FB` / Stop `FC` | running / stopped |
| no quarter-frame for 250 ms | stopped (the host paused its MTC) |

The node emits the usual `MediaSyncPacket` stream from that (100 ms running, 1 s idle,
immediately on any change) through the same v2.2 broadcast as the SysEx path — same `seq`
counter, `volume`/`flags` = 0 (MIDI in carries no volume) — on **its own layer**
(`SET_LOCAL_LAYER`; the AtomS3 default `*` addresses every slave — a packet tagged `*`
matches any subscription on a v2.1 slave; a 2.0.x slave follows it only when itself
subscribed to `*`). **SysEx keeps
priority**: for 2 s after any `MEDIA_SYNC` frame, MIDI in is ignored, so a MillluBridge /
HPlayer2 host is never disturbed by stray MIDI. Other CCs, notes and program changes travel
in [the v2.3 event frame](#the-v23-event-frame-0x04); `CC#100` does not — it is the media index,
which every slave already gets inside `MEDIA_SYNC` and plays to its host from there (2.4 rc,
nowde#t-049: on the frame too, a slave's host would get two `CC#100` streams on their own timing).

## Firmware tasks

| Task | Core | Priority | Period | Job |
|------|------|----------|--------|-----|
| `MIDI_Task` | 0 | `configMAX_PRIORITIES − 1` | 1 ms | USB-MIDI read / SysEx reassembly (512-byte buffer) |
| `ESPNOW_Task` | 1 | 10 | 10 ms | beacons, tables, link-lost, MTC generation, `meshClock.loop()` |

NVS namespace `nowde`: `layer` (string). *(v2: `role` u8 — 0 slave, 1 master, 0x7F auto.)*

## Radio watchdog *(2.0.5)*

`src/watchdog.cpp`. Restarts a node whose radio is provably dead — the field saw a master whose
every `esp_now_send()` returned `ESP_ERR_ESPNOW_NO_MEM` for 19 h while USB-MIDI stayed healthy,
recovered by a plain `esp_restart()` — and nothing else. WiFi power save is also off since 2.0.5
(`esp_wifi_set_ps(WIFI_PS_NONE)`, logged at boot).

| Role | Trigger | Log |
|------|---------|-----|
| master (sender) | every relay send failed `NO_MEM` for 30 s with the host still feeding it (one successful send ends the streak; since v2.2 the controlled-flooding forwards go through the same `relaySend()` and count too) | `[WATCHDOG] relay NO_MEM for N s -> esp_restart` |
| master (sender) | the table held ≥ 1 connected slave this boot and has been empty for 120 s | `[WATCHDOG] no slave for N s -> esp_restart` |
| slave | a master was heard this boot and none for 300 s | `[WATCHDOG] no sender for N s -> esp_restart` |

Bounds: nothing fires in the first 120 s of a boot, never under an OTA, and a restart is only
allowed once the boot has lived a **backoff** kept in RTC memory (survives `esp_restart()`, not
a power-on): 5 min, doubled at each watchdog restart of a boot that never had a healthy radio
for 60 s or that fires within 10 min of the previous one, capped at 60 min, back to 5 min once
the radio is healthy for 60 s (master: ≥ 1 connected slave; slave: ≥ 1 master heard). Until
then the trigger is *held* and logs so once a minute. A node that never had a peer this boot
never fires, so a fleet switched off around it costs it one restart at most. The boot after a
watchdog restart logs `[WATCHDOG] boot after watchdog restart #n: <reason>`; `bootReason` in
`HELLO` is `SW` (3), the restart count rides in the `HELLO` trailer.

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
