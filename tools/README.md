# Nowde tools

Python, run with [uv](https://docs.astral.sh/uv/) (dependencies are declared inline):

```sh
uv run tools/nowde-cli.py ports          # what is plugged in
uv run tools/nowde-cli.py hello          # handshake, read version / role / board
uv run tools/nowde-cli.py watch          # see MTC, CC#100, Start/Stop coming out of a slave + the node's own log (v2)
uv run tools/nowde-cli.py play 3 -d 20 -t 30   # be the master host: stream index 3, 20 s loop, for 30 s (--no-stop: leave without the stop frame)
uv run tools/nowde-cli.py slaves         # the sender's slave table
uv run tools/nowde-cli.py ota bin/firmware-atoms3.bin
uv run tools/nowde-bench.py nodes        # 2.4 desk acceptance: every node's version, build id, gen, lr, id
uv run tools/nowde-bench.py accept --master 26B8F8 --slave CD83FC   # the whole show + 0x04 test, PASS/FAIL + JSON
uv run tools/nowde-sim.py slave --autoplay   # fake slave node for an HPlayer2 with no hardware
uv run tools/nowde-sim.py master             # fake master node: HPlayer2 takes the master leg
```

- `nowde-cli.py` — drive a real node: handshake, probe, watch, slave table, role / layer,
  MEDIA_SYNC streaming, RF simulation, OTA over USB-MIDI. `-p REGEX` picks the port.
- `nowde-bench.py` — the 2.4 desk acceptance as one tool: `nodes`, `accept` (one host) or
  `drive` + `listen` (two hosts), `lr` (LR checks 1-4, 6), `relay` (meshmask builds), `ota
  --inject-dup`, `lr-set` and `walk` (the range walk's CSV logger). Each run writes a JSON
  report and ends on a PASS/FAIL screen; `selftest.py` pins its parsers.
- `nowde-sim.py` — a software node on ALSA virtual MIDI ports (`Nowde - SIM`), master or
  slave (`--legacy` = a v1.2 receiver that never says HELLO). Lets a Pi or laptop run
  `biennale.py` / `25-annatv.py` against a node that does not exist yet.
- `nowde_sysex.py` — the wire helpers both use; byte-exact with `src/sysex.cpp` and the
  MillluBridge Bridge (`docs/PROTOCOL.md`).
- `nowde_mesh.py` — the same for the *other* wire: the packed little-endian ESP-NOW structs
  nodes exchange over the air, byte-exact with `src/nowde_config.h`. Currently `0x04
  MIDI_EVENT` only, which is here before any firmware is because the charter freezes that
  frame the moment a fielded node parses it — so `selftest.py` checks the layout and the
  250-byte arithmetic instead of a comment claiming they add up.

Without uv: `pip install mido python-rtmidi` and run the scripts with `python3`.
