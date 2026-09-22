# Master-driven absolute volume over the Nowde mesh — plan (2026-09-22)

Goal: one slider on the master (W3) sets the software volume of every player in the garden.
Absolute only (Thomas, 22/09): every packet carries the master's level, so a lost frame costs
nothing — the next one, 100 ms later, says the same thing. No delta, no per-peer state on the wire.

## 1. Wire changes (both directions, both backward compatible)

**Host → master node, SysEx `MEDIA_SYNC` (0x10)** — today 27 bytes on the wire
(`F0 7D 10 layer[16] index pos[5] state F7`; `sysex.cpp` accepts `length >= 27`, reads `data[25]`
as state). Append two 7-bit bytes before `F7`:

| offset | byte | meaning |
|---|---|---|
| `data[26]` | `volume` | 0–100 (HPlayer2's `volume` setting; 127 = "not carried") |
| `data[27]` | `flags` | bit0 = volume valid, bit1 = mute (reserved, phase 2) |

A 2.0.3 node (`length >= 27`) parses the old fields and ignores the tail; a 2.0.4 node treats the
volume as absent when `length < 29`. HPlayer2 `build_media_sync(layer, index, pos, playing,
volume=None)` emits the tail only when a volume is given.

**Master node → slaves, ESP-NOW `MediaSyncPacket`** — today 27 bytes packed (`type, layer[16],
mediaIndex, positionMs, state, meshTimestamp`). Append at the END: `uint8_t volume; uint8_t flags;`
(29 bytes). `receiver_mode.cpp` keeps `len >= 27` as the acceptance test and reads the tail only
when `len >= 29`. A 2.0.3 slave receiving 29 bytes parses the first 27 and ignores the rest (its
check is `len < sizeof(old struct)`), so the order of OTA does not matter.

**Slave node → Pi, MIDI**: `CC#7` (channel 1, standard "channel volume") with the value 0–100,
sent on change and repeated every second alongside the existing CC#100 repeat
(`CC100_REPEAT_INTERVAL_MS`), so a Pi that boots or relinks catches up within a second. Master node:
nothing to its Pi (the master applies its own slider locally as today).

## 2. Firmware (Nowde 2.0.4, both `atoms3` and `atoms3-bcast` envs)

1. `nowde_config.h`: `NOWDE_VERSION "2.0.4"`; `MediaSyncPacket` + `volume`, `flags`;
   `MEDIASYNC_FLAG_VOLUME 0x01`, `MEDIASYNC_FLAG_MUTE 0x02`; `MediaSyncState` + `currentVolume`
   (255 = unknown), `lastSentVolume`, `lastCC7SendTime`.
2. `sysex.cpp` MEDIA_SYNC case: read `data[26]`, `data[27]` when `length >= 29`; fill
   `syncPacket.volume/flags`; `masterRelay.volume` for the LCD (optional line on the AtomS3 UI).
3. `receiver_mode.cpp` `processMediaSyncPacket`: after the state handling, if `len >= 29 &&
   (flags & VOLUME)`: `if (volume != currentVolume) { currentVolume = volume; midiSendCC(7,
   volume); lastCC7SendTime = now; }`; in the periodic block: resend CC#7 every 1000 ms while
   `currentVolume != 255` (same place as the CC#100 repeat).
4. `midi.cpp`/`midi.h`: `midiSendCC(uint8_t cc, uint8_t value)`; `midiSendCC100` becomes a call to it.
5. Nothing in HELLO / RUNNING_STATE changes. Bump the version string only.
6. Bench (see §5) before any OTA.

## 3. HPlayer2 (`biennale` branch)

1. `core/interfaces/nowde.py`:
   - `build_media_sync(..., volume=None)`: append `[volume & 0x7F, 0x01]` when `volume is not None`.
   - DEFAULTS: `'nowde-volume-link': 'off'` (master: `off | absolute`), `'nowde-volume-follow':
     False` (slave: obey CC#7). Both default off → no behaviour change anywhere else.
   - Master `_master_send()`: `vol = int(settings['volume']) if link == 'absolute' else None`;
     include `vol` in `changed` (a slider move sends at once and logs once: `MEDIA_SYNC … vol=NN`);
     pass it to `build_media_sync`.
   - Slave MIDI handler: `elif message.control == 7 and not self.isMaster() and
     self._cfg('nowde-volume-follow')`: `self._apply_linked_volume(message.value)`.
   - `_apply_linked_volume(v)`: if `v == current` return; apply live (`hplayer.settings.set('volume',
     v)` is what the http2 slider does today — it persists on every call: **debounce the persist**:
     apply to the player immediately, write the cfg only after 2 s without a new value. A slider drag
     from the master otherwise means tens of card writes per second on five slaves).
2. `profiles/biennale.py`: reuse the existing `volume-link` setting and select (`off | absolute |
   relative`): on a Nowde master, `absolute` now means "over the mesh" (the Zyre branch stays for
   the wall); `relative` is refused on a Nowde master (log a line, keep `off`). On a slave,
   `volume-follow` is set from the profile when the Nowde role resolves to slave (so nothing to
   configure per player: a garden slave follows by construction; a solo player never sees a node).
3. Web UI (`full.html`/`script.js`): on a following slave, grey the volume slider and show
   "linked to master" — optional, cosmetic; the master's 1 s repeat overrides a local move anyway.
4. Persist through reboots: a slave keeps the last received volume in its cfg (debounced), so it
   plays at yesterday's level until the master's first packet (≤ 1 s after link).

## 4. Behaviour to expect

- Drag the master's slider: every slave follows at 10 Hz (packet cadence while playing; 1 Hz while
  stopped — a change while stopped lands within a second).
- Lost frames: irrelevant (absolute, repeated). Slave reboot/relink: caught up by the CC#7 repeat
  within 1 s. Master reboot: it resends its own cfg volume.
- The master's own level: unchanged behaviour (local slider), it is one of the six.
- The hardware USB level stays pinned at 0 dB by `usbvol` — this is HPlayer2's software gain only.
- W2 today sits at 55 like the others; per-player trims are NOT part of this (absolute = everyone
  at the same software level; trims live in the hardware chain or come later as a per-peer offset).
- "Duck for events" (hplayer2#t-051) becomes: set the master's volume to N; no stop, no restart.

## 5. Bench and rollout

1. **Bench needs two nodes**: the spare AtomS3 (`D74A58`, master) + one Lite. If no second node is
   available at home, the canary happens on site on a closed day (Tuesday) with W6.
   Bench protocol: master node on the spare Pi (or the laptop sending SysEx with `nowde-*.py`
   tools extended for the volume tail), slave node on a second Pi/laptop with HPlayer2; move the
   master slider → slave cfg follows (`emit volume=NN` on the master, `tap` on the slave); kill
   the slave's HPlayer2 → restart → level restored within 1 s; drop the slave node's power → back
   → level restored; run 30 min at 10 Hz with the `journal-export` on → no cfg write storm (count
   writes on `/data/hplayer2-biennale.cfg`).
2. **Order on site (closed day)**: HPlayer2 bundle to all six via `hp-deploy` (restarts HPlayer2,
   slaves first, master last — with defaults off nothing changes); OTA **slaves first** (2.0.4 plain
   bin, `nowde-ota.py … acked`, ~2 min each, node keeps state), **master last** (2.0.4 **bcast**
   bin); then on W3 `emit volume-link=absolute`; test with `emit volume=40` then `55`; verify each
   slave by `audit` (cfg line) as Thomas passes by, or by ear.
3. **Fallback**: `emit volume-link=off` on W3 (instant, no wire change); OTA back to 2.0.3 per node
   if a node misbehaves; HPlayer2 bundle back to `c7db36f`.
4. Bundle this OTA round with the USB-stall firmware work (2.0.4-diag / the endpoint fix) so the
   garden's nodes are flashed once, not twice.

## 6. Effort and risks

Firmware ~3 h, HPlayer2 ~3 h, bench ½ day, on site ~1.5 h. Risks: the 2.0.3 SysEx length check is
`>= 27` (verified), so a longer frame is safe; ESP-NOW frame is 29 B (limit 250); CC#7 is not
consumed by anything else in HPlayer2 (grep clean). The one real trap is the cfg-write storm on the
slaves during a drag — hence the debounce.
