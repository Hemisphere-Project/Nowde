# Nowde ↔ HPlayer2

How an [HPlayer2](https://github.com/Hemisphere-Project/HPlayer2) media player
works with a Nowde node. Two legs:

- **slave leg** — a Nowde slave drives the player (in production on AnnaTV since
  2025, HPlayer2 `master` branch, `core/interfaces/nowde.py`);
- **master leg** — the player drives a Nowde master with its own playback state
  (landing with Nowde v2 for Biennale de Lyon 2026).

Both use the same interface class; the node's `HELLO` tells it which leg to run.

## Slave leg: the node drives the player

Profile wiring (`profiles/25-annatv.py`, and self-activating in `profiles/biennale.py`):

```python
hplayer = HPlayer2(config=True, datadir='/data', mediaPath=['media', 'usb'])
player  = hplayer.addPlayer('mpv', 'mpv')
nowde   = hplayer.addInterface('nowde', player)   # binds the interface to this player
```

`NowdeInterface` needs the `mido`, `python-rtmidi` and `timecode` packages (they
are in HPlayer2's `pyproject.toml`).

**Port discovery.** It opens the first MIDI input whose name matches `^Nowde`
(override with `port_name`). A reconnection loop (lookup every 5 s, health check
every 2 s) survives the node being unplugged and replugged: no restart needed on
an install left running.

**Media selection — CC#100.** `0` stops the player. `N` in `1..127` plays the
first media whose file name starts with that number, zero padding tolerated:
`N` builds the glob `(00N_*|0N_*|N_*)`, so `7_intro.mp4`, `07_intro.mp4` and
`007_intro.mp4` all answer index 7. The pattern is remembered for the loop
restart; an index that matches nothing logs a warning and stops.

**Position — MTC.** The listener decodes both quarter-frames (reassembled every
8 pieces) and full-frame SysEx into a timecode and feeds `handle_timecode()`,
which is the shared **`Drifter`** chase-lock servo (`core/engine/drifter.py`):

- **dead zone with hysteresis** (enter ±25 ms, exit ±80 ms): inside it the player
  coasts at 1.0×, so a locked player is never nudged;
- **progressive speed trims** outside the dead zone, smoothed over 3 samples,
  faster when late than when ahead;
- **hard seek** only beyond the servo's reach (2 s ahead, 10 s late on the nowde
  leg), with `jumpFix` latency compensation (~500 ms, tuned per box);
- **stall hook**: the local clip ended while the master clock keeps running →
  replay the remembered pattern (timecode-loop restart);
- **kick-start grace** masks the first ticks after a (re)launch while mpv spins up.

The player's own state machine is otherwise untouched: `CC#100 = 0` sets a stopped
flag that mutes MTC until the next non-zero index.

## Master leg: the player drives the node

*Nowde v2, HPlayer2 `master` branch.* The same interface opens the matching MIDI
**output** as well and, in `mode='auto'` (the default), probes the node with
`QUERY_RUNNING_STATE` every 2 s: a v2 node answers `HELLO` with its role, a v1.2
receiver stays silent and is assumed to be a slave after 6 s (never `QUERY_CONFIG`
on an unknown node: that would turn a v1.2 receiver into a sender). Once the role
is master (or with `mode='master'`) the interface:

1. sends `QUERY_CONFIG` (the Bridge handshake) and again on every fresh `HELLO` (node reboot);
2. every 100 ms sends `MEDIA_SYNC` with
   - `index` = the numeric prefix of the current media's file name (`0` when
     stopped or unnumbered; a `nowde-index-default` setting can substitute a fixed
     index for unnumbered content),
   - `position` = `player.position()` in ms,
   - `state` = `1` when playing and not paused, else `0`,
   - `layer` = the `nowde-layer` setting (default `hplayer2`);
3. polls `QUERY_RUNNING_STATE` once a second, emits `nowde.receivers` for the UI,
   and assigns its layer to any slave still on the factory `-` layer (v1.2 nodes)
   through `CHANGE_RECEIVER_LAYER`.

Only the index travels: which file a slave plays for index 7 is that slave's own
media folder. Keep the same index on every player for the same cue, and the same
duration if the content loops seamlessly on the master.

**Node log.** With the `nowde-nodelog` setting on (http2 panel), the interface asks a v2
node for its log (`SET_LOG`) and prints every `LOG` frame as `node| …` in its own log —
the ESP-NOW side of a problem lands in the player's journal. Off by default: no extra
traffic on the wire.

## Deployment notes

- The profile decides the behaviour, not the branch: the interface is loaded
  unconditionally and idles until a `Nowde…` port shows up; the node's role
  selects the leg. One `hplayer2@<profile>` instance serves master and slaves.
- The node enumerates with PID `0x8000` and product `Nowde - XXXXXX`; HPlayer2's
  serial scanners (radar, teleco2, dmx) key on other ids and leave its CDC port
  alone.
- Pause on the master reads as stop on the slaves in v2.0 (the wire carries
  `state` 0/1). A `paused` state is a v2.1 extension.
