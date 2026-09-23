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
| `0A` | SET_LOG *(v2)* | `F0 7D 0A on(1) F7` | `1`: stream the node's own log to the host as `LOG` frames; `0`: stop. Off at boot; the first `1` also delivers the boot log kept since power-up (8 KB). Not stored. Bench / journal aid: `nowde-cli watch` turns it on, HPlayer2 with `nowde-nodelog`. |
| `0B` | SET_LR *(2.0.3)* | `F0 7D 0B on(1) F7` | Long-range PHY switch. Stored in NVS, then the node **restarts** to apply it (the protocol is set before the channel). All-or-nothing across the mesh. Answers `HELLO` before restarting. |
| `0C` | OTA_DATA_ACKED *(2.0.3)* | `F0 7D 0C seq(1) len(1) data(7-bit) F7` | Stop-and-wait `OTA_DATA`: written only if the decoded length matches `len`, answered by `OTA_ACK seq status`. |
| `0D` | SET_LOSS_POLICY *(v2.2)* | `F0 7D 0D stop(1) F7` | `1` stop, `0` freewheel, when the `MEDIA_SYNC` stream dies. Stored in NVS and applied **live** — no restart, unlike `SET_LR`. Answers `CONFIG_STATE`. Which one a node wants is a property of where it stands, so it stopped being a build flag. |
| `0E` | SET_ORIGIN *(v2.2)* | `F0 7D 0E [mac(6→7)] F7` | With a MAC: **pins** the origin lock to that master and holds it through any silence. Without one (`F0 7D 0E F7`, 4 bytes): releases the lock, and the next `MediaSyncPacket` heard adopts its sender. Answers `CONFIG_STATE`. Not stored — a lock is a live fact, not a config. |

### Host → slaves, relayed by the master (`0x10`–`0x1F`)

| Cmd | Name | Frame | Effect |
|-----|------|-------|--------|
| `10` | MEDIA_SYNC | `F0 7D 10 layer(16 raw ASCII, NUL-padded) index(1) position(4→5) state(1) [volume(1) flags(1)] F7` — 27 bytes, 29 with the *(2.0.4)* volume tail | Master stamps `meshMillis()`, mints a `seq`, and sends **one broadcast** `MediaSyncPacket` *(v2.2 — it was one unicast per connected slave on `layer`)*. `position` = milliseconds, big-endian u32 before encoding. `state`: `0` stopped, `1` playing. `volume` 0..100 absolute, carried only when `flags` bit0 is set; a 27-byte frame means *no volume* and nothing is relayed for it. Send at ~10 Hz while playing; one frame with `state=0` to stop. |
| `11` | CHANGE_RECEIVER_LAYER | host form: `F0 7D 11 mac(6→7) layer(16→19) F7` — 29 bytes | Master looks the slave up by MAC and forwards the mesh form (below). `ERROR_REPORT 05` if the MAC is unknown. |

### Node → host (`0x20`–`0x3F`)

| Cmd | Name | Frame |
|-----|------|-------|
| `20` | HELLO | `F0 7D 20 version(8→10) uptimeMs(4→5) bootReason(1) F7` — 20 bytes. Sent on boot (after USB enumeration) and on every `QUERY_CONFIG`. `version` is the NUL-padded `NOWDE_VERSION` string; `bootReason` is `esp_reset_reason() & 0x7F`. Trailers are appended in order and each needs the ones before it — *(v2)* `role(1) board(1)`, *(2.0.1)* `syncQuality(1)`, *(2.0.3)* `lr(1)`, *(v2.2)* `syncGaps(2, 14-bit hi/lo)`. Parsers must accept longer frames; the MillluBridge one does. A slave never sends `RUNNING_STATE`, so `syncGaps` here is how its **own** host reads the delivery signal. |
| `21` | CONFIG_STATE | `F0 7D 21 rfSim(1) delayHi(1) delayLo(1) F7` — 7 bytes. *(v2 appends `role(1) board(1) layerLen(1) layer(ASCII…)`; v2.2 appends `stopOnLinkLost(1) originState(1) originMac(6→7)` after the variable-length layer.)* `originState`: `0` following nobody yet, `1` adopted, `2` pinned by `SET_ORIGIN`. This is the read-back for both runtime switches. |
| `22` | RUNNING_STATE | per chunk: `F0 7D 22 uptimeMs(4→5) meshSynced(1) totalSlaves(1) chunkIndex(1) chunkCount(1) slavesInChunk(1) [slave(39→45)] F7`. One slave per chunk. Slave record, raw: `mac(6) layer(16) version(8) lastSeenMs(4 BE) active(1) mediaIndex(1)` *(2.0.1 appends `syncQuality(1)`; v2.2 appends `syncGaps(2 BE)`)*. Only *connected* slaves are listed. One record per chunk, so a host that reads the older 36/37-byte record simply stops short of the tail. |
| `23` | OTA_ACK | reserved, unused in v1.2 |
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
| `0x02` | `ReceiverInfo { type, layer[16], version[8], mediaIndex }` *(2.0.1 appends `syncQuality u8`; v2.2 appends `syncGaps u16`)* | slave → each known master, unicast | 1 s + 0–200 ms jitter. The master marks a slave *missing* after 5 s and drops it after 10 s. |
| `0x03` | `MediaSyncPacket { type, layer[16], mediaIndex u8, positionMs u32, state u8, meshTimestamp u32 }` — 27 B, *(2.0.4 appends `volume u8`, `flags u8` → 29 B; v2.2 appends `seq u16` → 31 B, **in that order**: 2.0.4 shipped first, so its offsets are fixed and `seq` goes after)* | master → **broadcast** *(v2.2 — it was one unicast per connected slave on the layer)* | on every host `MEDIA_SYNC`, i.e. ~10 Hz |
| `0xF0…` | SysEx frame, mesh form of `CHANGE_RECEIVER_LAYER`: `F0 7D 11 layer(raw ASCII, unpadded) F7` | master → one slave | on demand. The slave stores the layer in NVS and re-announces itself. |
| `0x04` | `MidiEventFrame` *(v2.3 — byte layout frozen below, **not yet implemented**)* | master → broadcast | Note / CC / PC relay scheduled on mesh time. Batched, see [the event frame](#the-v23-event-frame-0x04). |
| `0x05` | `MeshRelay { origin[6], seq u16, hop u8 }` + the original packet verbatim *(v2.2, reserved — #t-024)* | any node → broadcast | controlled flooding for out-of-range slaves. A **new type**, not a trailer on `0x03`: a v1.2 slave ignores an unknown type but *acts* on a longer `0x03`, and it cannot origin-lock. |

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

### Timing constants (`nowde_config.h`)

| Constant | Value |
|----------|-------|
| `SENDER_BEACON_INTERVAL_MS` / `RECEIVER_BEACON_INTERVAL_MS` | 1000 |
| `SENDER_TIMEOUT_MS` / `RECEIVER_TIMEOUT_MS` | 5000 (missing), 10 000 (removed) |
| `MTC_FRAMERATE` | 30 fps |
| `CC100_REPEAT_INTERVAL_MS` | 1000 (0 disables) |
| `CC7_REPEAT_INTERVAL_MS` *(2.0.4)* | 1000 (0 disables) |
| `LINK_LOST_TIMEOUT_MS` | 10 000 |
| `CLOCK_DESYNC_THRESHOLD_MS` | 200 |
| `ORIGIN_LOCK_RELEASE_MS` *(v2.2)* | 2000 (silence before an adopted lock may switch master) |
| `MEDIASYNC_MAX_GAP` *(v2.2)* | 100 (largest `seq` jump still read as loss; 10 s at 10 Hz) |
| `TRANSMISSION_DELAY_US` | 1300 (ESPNowMeshClock one-way estimate) |
| `ESPNowMeshClock(...)` | interval 1000 ms, slew α 0.25, large step 10 ms, timeout 5 s, jitter 10 % |
| `MAX_SENDERS` / `MAX_RECEIVERS` | 10 / 10 |

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
wire field** and is deliberately not numbered here: #t-039 derives it from the frozen flooding
parameters of §(2) (a random 5–30 ms forward delay on each of `MAX_HOPS` 2–3 → 60–90 ms
worst-case relay latency before clock error), which #t-024 fixes. A lead shorter than that
makes relayed events fire late, silently, and only on the far side of a hop.

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
| relay envelope | 10 B | `0x05 MeshRelay { origin[6], seq u16, hop u8 }` + type, reserved for #t-024 |
| header | 30 B | table above |
| record | 7 B | table above |
| **new records per frame** | **14** | `(250 − 10 − 30) ÷ 7 ÷ (1 + depth)` = 15, taken down one: 15 lands on 250 B exactly, with no margin for the envelope #t-024 has not built yet |
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
   lost, which is the entire point of carry-over. 150 ms is the stated default: above §(2)'s
   60–90 ms worst-case relay latency with margin, below the 200 ms `CLOCK_DESYNC_THRESHOLD_MS`
   at which the mesh clock stops being trusted at all. It is slave-side policy, not a wire
   field, so #t-039 may revise it with the lead.
5. Re-emit on the record's original channel (a slave-side remap is configuration, never wire).

**Where the scheduler has to live.** `ESPNOW_Task` runs on a **10 ms** period, which is coarser
than the accuracy this frame exists to deliver — a chord scheduled there lands with ±10 ms of
tick jitter and `fireAt` buys nothing. The due-record check belongs on **`MIDI_Task`'s 1 ms
tick**, which is also the only context allowed to write TinyUSB (see *one writer, one active
IN endpoint*). Relayed events reach the host through `usbOutMidi()` like everything else.

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
Built by #t-038; the bit is frozen here so the frame does not have to change to gain it.

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
- *(v2.3, not yet implemented)* Relayed **channel-voice messages** from the master host —
  Note, CC, Program Change, Channel Pressure, Pitch Bend — on their original channel, emitted
  at their scheduled mesh time and never earlier. The transport is
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

While no host reads the MIDI port the TinyUSB FIFO fills and the queue behind it stalls; after
200 ms of stall the queue is dropped, so a host opening the port later never receives a ghost
stream of stale MTC / `CC#100` (it gets at most the FIFO's 16 packets, and a fresh `CC#100`
within a second while playing). A momentary stall (a `RUNNING_STATE` burst) keeps its order.

`meshMillis()` is read through `meshMillisStable()` (two agreeing reads): the library's
64-bit offset is slewed by another task and a torn read comes back off by 2^32 µs.

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
