#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["mido>=1.3", "python-rtmidi>=1.5"]
# ///
"""nowde-bench — the 2.4 desk acceptance as one tool: every check writes a JSON report and ends
on a one-screen PASS/FAIL summary (exit 0 = all PASS, 1 = a FAIL, 2 = could not run).

  nowde-bench nodes                                        every Nowde port: version, build id, gen, role, board, lr, node id
  nowde-bench accept --master P --slave P [--slave P ...] [--second-master P]
                                                           one host: a scripted show + the 0x04 test, both halves at once
  nowde-bench drive --master P                             the master half of `accept` (rig: the master's host)
  nowde-bench listen --slave P [--slave P ...]             the slave half of `accept` (rig: the slave's host)
  nowde-bench lr --a P --b P [--c P]                       LR checks 1, 2, 3, 4, 6 (proposal 6.5); leaves LR off
  nowde-bench relay --chain P1 P2 P3                       meshmask builds: master P1 -> P2 -> P3 only (#t-040)
  nowde-bench ota --node P --image F [--inject-dup]        the hardened OTA; HELLO's build id must match the image
  nowde-bench lr-set --port P off|500k|250k                store an LR mode (the node restarts) and read it back
  nowde-bench walk --master P --lr off|500k|250k [--csv F] the range walk's logger: one CSV row per receiver per second

P = a regex on the MIDI port name (`CD83FC` picks `Nowde - CD83FC`). `--short` scales the show
down for a smoke run. Two hosts (the rig, LR across hosts): `lr-set` on each, then `drive` on
one and `listen` on the other — `listen` judges delivery, lock and gaps on its own clock; timing
numbers (MTC offsets, fire spread) are a one-host measure and only `accept` / `lr` give them.

The firmware does not count stale 0x04 drops (they are silent by design), so a stale drop shows
here as a lost event; NO_MEM is read from the node's own log (SET_LOG is on for the run).

Every node a show check touches is put on the show's layer (`nowde-bench`) and put back on its
own after: a slave follows MEDIA_SYNC only on its layer (or "*"), and a master stamps its 0x04
events with its own (25/11 rig: a slave left on "-" heard nothing). `walk` streams on "*", since
the far node is out of the tool's reach. `ota` refuses to start while this host's usbfix watchdog
is armed (`touch /run/usbfix.inhibit` first: it re-enumerates a node mid-transfer).
"""
import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowde_sysex as nx  # noqa: E402

SYNC_LOCKED = 2
MTC_RATES = (24.0, 25.0, 29.97, 30.0)
SHOW_INDEX = 3            # the scripted show plays index 3 ...
INTRUDER_INDEX = 5        # ... and the second master streams 5: a slave showing 5 changed origin
EVENT_CC = 20             # the 0x04 sweep's controller (CC#100 is the index, never on the radio)
EVENT_CH = 0
MS_RATE_HZ = 10
SHOW_LAYER = 'nowde-bench'   # every show check runs here; take() moves the nodes, _restore() back
WALK_LAYER = '*'             # the walk's far node is out of reach: the wildcard reaches any layer
OTA_MAX_RESENDS = 50         # a node that acks late on every chunk: stop and say so, never crawl


class CouldNotRun(Exception):
    """Exit 2: no port, no MIDI stack, an unsafe host -- the run never judged a node (1 = a FAIL)."""


class NoPort(CouldNotRun):
    pass


# ---- pure helpers (tools/selftest.py) -----------------------------------------------

def mtc_fps(rate_bits):
    return MTC_RATES[rate_bits & 3]


def mtc_qf_ms(qf):
    """The eight quarter-frame nibbles (index = frame_type) -> (position ms, fps)."""
    fps = mtc_fps((qf[7] >> 1) & 3)
    fr = qf[0] | (qf[1] << 4)
    sec = qf[2] | (qf[3] << 4)
    mn = qf[4] | (qf[5] << 4)
    hr = qf[6] | ((qf[7] & 1) << 4)
    return ((hr * 60 + mn) * 60 + sec) * 1000.0 + fr * 1000.0 / fps, fps


def mtc_full_ms(data):
    """MTC full-frame SysEx data (7F 7F 01 01 hh mm ss ff) -> position ms, or None."""
    if len(data) != 8 or tuple(data[0:4]) != (0x7F, 0x7F, 1, 1):
        return None
    fps = mtc_fps((data[4] >> 5) & 3)
    return (((data[4] & 0x1F) * 60 + data[5]) * 60 + data[6]) * 1000.0 + data[7] * 1000.0 / fps


def stats(xs):
    """mean / p95 / max of |x| — the shape every timing line in the report takes."""
    xs = [float(x) for x in xs]
    if not xs:
        return {'n': 0, 'mean': None, 'p95': None, 'max': None}
    a = sorted(abs(x) for x in xs)
    p95 = a[min(len(a) - 1, int(round(0.95 * (len(a) - 1))))]
    return {'n': len(xs), 'mean': round(sum(xs) / len(xs), 2), 'p95': round(p95, 2), 'max': round(a[-1], 2)}


def show_script(play_s, idle_s, seek_to_ms=120000):
    """The accept show: play SHOW_INDEX from 0, seek at half time, stop at play_s, idle_s of
    silence. Segments (t0, t1, playing, position at t0) on the show's own clock."""
    half = play_s / 2.0
    return [(0.0, half, True, 0.0), (half, float(play_s), True, float(seek_to_ms)),
            (float(play_s), float(play_s + idle_s), False, 0.0)]


def show_position(script, t):
    """(playing, expected position ms) at show time t."""
    for t0, t1, playing, p0 in script:
        if t0 <= t < t1:
            return playing, (p0 + (t - t0) * 1000.0 if playing else 0.0)
    return False, 0.0


def event_plan(n_cc=128, notes=range(60, 73)):
    """The 0x04 test's events, in send order: a CC#EVENT_CC sweep 0..n_cc-1, then the notes as
    note-on/off pairs. Each event is unique by its key, which is what the ledger matches on."""
    plan = [('control_change', EVENT_CH, EVENT_CC, v) for v in range(n_cc)]
    for n in notes:
        plan += [('note_on', EVENT_CH, n, 100), ('note_off', EVENT_CH, n, 0)]
    return plan


def event_key(msg_type, channel, a, b):
    if msg_type == 'note_on' and b == 0:   # running-status note-off spelling
        msg_type = 'note_off'
    return (msg_type, channel, a, b)


def ledger(plan, captures):
    """captures = {slave: [(t, key), ...]}. Per slave: lost / duplicated; across the first two
    slaves: the fire-time spread (ms) of the events both received once."""
    want = {event_key(*k) for k in plan}
    out = {}
    first = {}
    for slave, caps in captures.items():
        seen = {}
        for t, k in caps:
            if k in want:
                seen.setdefault(k, []).append(t)
        out[slave] = {'sent': len(want), 'received': sum(1 for k in want if k in seen),
                      'lost': sum(1 for k in want if k not in seen),
                      'duplicated': sum(len(v) - 1 for v in seen.values())}
        first[slave] = {k: v[0] for k, v in seen.items() if len(v) == 1}
    names = list(captures)
    spread = None
    if len(names) >= 2:
        a, b = first[names[0]], first[names[1]]
        spread = stats([(a[k] - b[k]) * 1000.0 for k in a if k in b])
    return out, spread


def mtc_offsets(script, t_start, mtc, settle_s=2.0):
    """mtc = [(host time of quarter frame 0, decoded ms)]. Offset = decoded − expected, skipping
    `settle_s` after a start or a seek (a slave re-anchors there by design)."""
    edges = [seg[0] for seg in script if seg[2]]
    offs = []
    for t, pos in mtc:
        st = t - t_start
        playing, want = show_position(script, st)
        if not playing or any(e <= st < e + settle_s for e in edges):
            continue
        offs.append(pos - want)
    return offs


def booted_since(info, t_sent, now):
    """True when a HELLO comes from a node that restarted after t_sent: its uptime is shorter than
    the time since. SET_LR answers with a HELLO that already carries the new mode BEFORE it restarts,
    so the mode alone proves nothing was applied."""
    up = (info or {}).get('uptime_ms')
    return up is not None and up / 1000.0 < now - t_sent


def hello_row(port, info):
    info = info or {}
    return {'port': port, 'version': info.get('version'), 'build_id': info.get('build_id'),
            'gen': info.get('gen'), 'role': info.get('role'), 'board': info.get('board'),
            'lr': info.get('lr_mode'), 'node_id': info.get('node_id'),
            'sync': info.get('sync_quality'), 'gaps': info.get('sync_gaps'), 'rssi': info.get('rssi')}


def walk_rows(t, elapsed, rs, note=''):
    """One CSV row per receiver of a RUNNING_STATE (or one empty row: nobody heard)."""
    recs = (rs or {}).get('receivers') or [{}]
    return [[time.strftime('%H:%M:%S', time.localtime(t)), '%.1f' % elapsed, r.get('mac', ''),
             1 if r.get('sync_quality') == SYNC_LOCKED else 0 if r else '',
             r.get('sync_gaps', ''), r.get('rssi') if r.get('rssi') is not None else '',
             r.get('last_seen_ms', ''), note] for r in recs]


WALK_HEADER = ['time', 'elapsed_s', 'mac', 'locked', 'gaps', 'rssi_dbm', 'last_seen_ms', 'note']


def summary(report):
    """(lines, all_pass) from report['checks'] = [{'name', 'pass': bool|None, 'detail'}].
    pass None = INFO: a number the spec gives no pass line for — his read, never a FAIL."""
    lines, ok = [], True
    for c in report.get('checks', []):
        v = c.get('pass')
        tag = 'PASS' if v else ('INFO' if v is None else 'FAIL')
        ok = ok and v is not False
        lines.append(f"  {tag:4}  {c['name']}" + (f" — {c['detail']}" if c.get('detail') else ''))
    lines.append(('ALL PASS' if ok else 'FAIL') + f"  ({report.get('command', '?')})")
    return lines, ok


# ---- ports ------------------------------------------------------------------------

def _mido():
    try:
        import mido
    except ImportError:
        raise CouldNotRun("nowde-bench needs mido + python-rtmidi: `uv run tools/nowde-bench.py ...` "
                          "(or pip install mido python-rtmidi)")
    return mido


def nowde_ports():
    return sorted({n for n in _mido().get_input_names() if n.startswith('Nowde')})


class Port:
    """One Nowde, opened by a regex on its name; every message timestamped on arrival."""

    def __init__(self, pattern, wait_s=0.0):
        mido = _mido()
        end = time.time() + wait_s
        while True:
            ins = [n for n in mido.get_input_names() if re.search(pattern, n)]
            outs = [n for n in mido.get_output_names() if re.search(pattern, n)]
            if ins and outs:
                break
            if time.time() >= end:
                raise NoPort(f"no MIDI port matching /{pattern}/ (inputs: {mido.get_input_names()})")
            time.sleep(0.5)
        self.pattern = pattern
        self.name = ins[0]
        self.lock = threading.Lock()
        self.msgs, self.mtc, self.logs = [], [], []
        self.qf = [0] * 8
        self.info = {}
        self.inp = mido.open_input(ins[0], callback=self._on)
        self.out = mido.open_output(outs[0])

    def _on(self, msg):
        t = time.time()
        with self.lock:
            self.msgs.append((t, msg))
            if msg.type == 'quarter_frame':
                self.qf[msg.frame_type] = msg.frame_value
                if msg.frame_type == 7:
                    pos, fps = mtc_qf_ms(self.qf)
                    self.mtc.append((t - 7 * 250.0 / fps / 1000.0, pos))
            elif msg.type == 'sysex' and msg.data and msg.data[0] == nx.MANUFACTURER and len(msg.data) > 1 \
                    and msg.data[1] == nx.CMD_LOG:
                self.logs.append((t, nx.parse(list(msg.data))[1]['text']))

    def close(self):
        self.inp.close()
        self.out.close()

    def send(self, payload):
        self.out.send(_mido().Message('sysex', data=payload))

    def mark(self):
        with self.lock:
            return len(self.msgs)

    def since(self, mark):
        with self.lock:
            return list(self.msgs[mark:])

    def wait_for(self, cmd, timeout=2.0, mark=None):
        mark = self.mark() if mark is None else mark
        end = time.time() + timeout
        while time.time() < end:
            for _, m in self.since(mark):
                if m.type == 'sysex' and len(m.data) >= 2 and m.data[0] == nx.MANUFACTURER and m.data[1] == cmd:
                    return nx.parse(list(m.data))[1]
            time.sleep(0.02)
        return None

    def hello(self):
        """QUERY_RUNNING_STATE first, never a bare QUERY_CONFIG (it turns a legacy node into a
        sender). A sender -- a master, or a legacy node a host already turned -- answers a linked
        host with its table only, and HPlayer2 polls every 2 s, so the link never drops (25/11,
        B under bench-02's player: RUNNING_STATE, no HELLO). That table proves the node is a
        sender already: QUERY_CONFIG then changes nothing and always answers HELLO."""
        mark = self.mark()
        self.send(nx.query_running_state())
        info = self.wait_for(nx.CMD_HELLO, 2.0, mark)
        if not info and self.wait_for(nx.CMD_RUNNING_STATE, 0.1, mark):
            mark = self.mark()
            self.send(nx.query_config())
            info = self.wait_for(nx.CMD_HELLO, 2.0, mark)
        if info:
            self.info = info
        return info

    def listed(self):
        """Is the port still on this host? (No = the node left USB: a reset, or a watchdog.)"""
        return any(re.search(self.pattern, n) for n in _mido().get_input_names())

    def running_state(self, timeout=1.0):
        mark = self.mark()
        self.send(nx.query_running_state())
        end, chunks = time.time() + timeout, {}
        while time.time() < end:
            for _, m in self.since(mark):
                if m.type == 'sysex' and len(m.data) >= 2 and m.data[1] == nx.CMD_RUNNING_STATE:
                    rs = nx.parse(list(m.data))[1]
                    chunks[rs['chunk']] = rs
            if chunks and len(chunks) >= max(c['chunks'] for c in chunks.values()):
                break
            time.sleep(0.02)
        if not chunks:
            return None
        head = dict(chunks[min(chunks)])
        head['receivers'] = [r for k in sorted(chunks) for r in chunks[k]['receivers']]
        return head

    def captures(self, since_t=0.0):
        """(t, key) of every Note/CC except CC#100, plus the CC#100 values and MIDI Stops."""
        events, index, stops = [], [], []
        for t, m in self.since(0):
            if t < since_t:
                continue
            if m.type == 'control_change' and m.control == 100:
                index.append((t, m.value))
            elif m.type == 'control_change':
                events.append((t, event_key('control_change', m.channel, m.control, m.value)))
            elif m.type in ('note_on', 'note_off'):
                events.append((t, event_key(m.type, m.channel, m.note, m.velocity)))
            elif m.type == 'stop':
                stops.append(t)
        return events, index, stops

    def log_count(self, pattern):
        with self.lock:
            return sum(1 for _, s in self.logs if re.search(pattern, s))


def send_event(port, ev):
    mido = _mido()
    kind, ch, a, b = ev
    if kind == 'control_change':
        port.out.send(mido.Message('control_change', channel=ch, control=a, value=b))
    else:
        port.out.send(mido.Message(kind, channel=ch, note=a, velocity=b))


def node_id_of(port):
    if port.info.get('node_id'):
        return port.info['node_id'].upper()
    m = re.search(r'([0-9A-Fa-f]{6})\s*$', port.name.split(':')[0])
    return m.group(1).upper() if m else None


def check(report, name, ok, detail=''):
    report['checks'].append({'name': name, 'pass': ok, 'detail': detail})


def finish(report, path):
    report['finished'] = time.strftime('%Y-%m-%dT%H:%M:%S')
    lines, ok = summary(report)
    with open(path, 'w') as f:
        json.dump(report, f, indent=2, default=str)
    print('\n'.join(lines))
    print(f"report: {path}")
    return 0 if ok else 1


# ---- the show (accept / drive / listen) -----------------------------------------------

def run_show(master, script, plan, second=None, layer=SHOW_LAYER, grab_at=None):
    """Stream the script into `master` at MS_RATE_HZ; send `plan` across the second play
    segment at ~100 events/s; promote `second` for the middle of the first segment, streaming
    INTRUDER_INDEX; read the master's RUNNING_STATE at `grab_at` s. Returns (t0, table)."""
    t0 = time.time()
    end = script[-1][1]
    next_ms, ev_i = 0.0, 0
    ev_from = script[1][0] + 1.0
    sec_on, sec_off = script[0][1] * 0.25, script[0][1] * 0.75
    sec_state, table = None, None
    while True:
        st = time.time() - t0
        if st >= end:
            break
        if st >= next_ms:
            playing, pos = show_position(script, st)
            master.send(nx.media_sync(layer, SHOW_INDEX if playing else 0, pos, playing))
            if second and sec_state == 'master':
                second.send(nx.media_sync(layer, INTRUDER_INDEX, pos, True))
            next_ms += 1.0 / MS_RATE_HZ
        if second and sec_state is None and st >= sec_on:
            second.send(nx.set_role('master'))
            sec_state = 'master'
        if second and sec_state == 'master' and st >= sec_off:
            second.send(nx.media_sync(layer, 0, 0, False))
            second.send(nx.set_role('slave'))
            sec_state = 'off'
        if ev_i < len(plan) and st >= ev_from + ev_i * 0.01:
            send_event(master, plan[ev_i])
            ev_i += 1
        if grab_at is not None and table is None and st >= grab_at:
            table = master.running_state() or {}
        time.sleep(0.002)
    return t0, table


def judge_slave(report, label, port, script, t0, intruder, timing=True):
    play_s = script[1][1]
    _, index, stops = port.captures(t0)
    rel = [(t - t0, v) for t, v in index]
    first3 = next((t for t, v in rel if v == SHOW_INDEX), None)
    check(report, f"{label}: index {SHOW_INDEX} on CC#100 at start", first3 is not None and first3 < 3.0,
          f"first at {first3:.2f} s" if first3 is not None else 'never')
    stop_cc = next((t for t, v in rel if v == 0 and t >= play_s), None)
    stop_rt = next((t - t0 for t in stops if t - t0 >= play_s), None)
    check(report, f"{label}: the stop (CC#100=0 + MIDI Stop)",
          stop_cc is not None and stop_rt is not None and stop_cc - play_s < 1.5,
          f"CC#100=0 +{(stop_cc or 0) - play_s:.2f} s, Stop {'seen' if stop_rt is not None else 'missing'}")
    idle_s = script[2][1] - script[2][0]
    repeats = sum(1 for t, v in rel if v == 0 and t >= play_s + 1.5)
    check(report, f"{label}: the 1 Hz level stop (CC#100=0 repeated)", repeats >= idle_s - 3,
          f"{repeats} repeats in {idle_s:.0f} s")
    if intruder:
        wrong = sum(1 for _, v in rel if v == INTRUDER_INDEX)
        check(report, f"{label}: a second master does not take the origin (#t-022)", wrong == 0,
              f"{wrong} CC#100={INTRUDER_INDEX}")
    if timing:
        with port.lock:
            mtc = list(port.mtc)
        st = stats(mtc_offsets(script, t0, mtc))
        report.setdefault('mtc', {})[label] = st
        check(report, f"{label}: MTC flows through the show", st['n'] > 0, f"{st['n']} timecodes")
        check(report, f"{label}: MTC offset vs the show", None,
              f"mean {st['mean']} / p95 {st['p95']} / max {st['max']} ms (receive time, one host clock)")


def judge_lock(report, label, rs, node_id, port=None):
    rec = next((r for r in (rs or {}).get('receivers', []) if node_id and r['mac'].replace(':', '').endswith(node_id)), None)
    check(report, f"{label}: in the master's table, LOCKED", bool(rec) and rec['sync_quality'] == SYNC_LOCKED,
          f"gaps {rec['sync_gaps']}, rssi {rec['rssi']} dBm" if rec else 'not in the table')
    return rec


def _open(patterns):
    return [Port(p) for p in patterns]


_layers_before = {}   # port pattern -> the layer the node stood on before take() moved it


def take(p, role, layer=SHOW_LAYER):
    """Put p in `role` on the show's layer, remembering the layer it stood on for _restore. SET_ROLE
    answers CONFIG_STATE (the layer before); SET_LOCAL_LAYER answers it again (the layer now),
    which is what this returns. The layer is stored in NVS: it survives an LR restart."""
    mark = p.mark()
    p.send(nx.set_role(role))
    cs = p.wait_for(nx.CMD_CONFIG_STATE, 1.0, mark) or {}
    _layers_before.setdefault(p.pattern, cs.get('layer'))
    mark = p.mark()
    p.send(nx.set_local_layer(layer))
    return (p.wait_for(nx.CMD_CONFIG_STATE, 1.0, mark) or {}).get('layer')


def take_all(report, pairs):
    """take() each (port, role); one check per node: a node off the layer hears no show."""
    for p, role in pairs:
        got = take(p, role)
        check(report, f"{p.name}: {role} on layer {SHOW_LAYER}", got == SHOW_LAYER,
              f"was {_layers_before.get(p.pattern)!r}, now {got!r}")


def _restore(ports):
    for p in ports:
        p.send(nx.set_log(False))
        p.send(nx.set_role('auto'))
        before = _layers_before.pop(p.pattern, None)
        if before and before != SHOW_LAYER:
            p.send(nx.set_local_layer(before))
    time.sleep(0.3)


def cmd_nodes(a):
    report = {'command': 'nodes', 'nodes': [], 'checks': []}
    for name in nowde_ports():
        p = Port(re.escape(name))
        row = hello_row(name, p.hello())
        p.close()
        report['nodes'].append(row)
        print(f"  {name:24} {row['version'] or '-':8} build {row['build_id'] or '-':16} gen {row['gen']}"
              f"  {row['role'] or '-':6} {row['board'] or '-':11} lr {row['lr'] or '-':4} id {row['node_id']}")
        check(report, f"{name}: answers HELLO with gen + node id", row['gen'] is not None,
              f"version {row['version']}")
    if not report['nodes']:
        check(report, 'a Nowde port is plugged', False, 'none found')
    return finish(report, a.report)


def _script(a):
    return show_script(6, 6) if a.short else show_script(a.play, a.idle)


def cmd_accept(a):
    report = {'command': 'accept', 'checks': []}
    master = Port(a.master)
    slaves = _open(a.slave)
    second = Port(a.second_master) if a.second_master else None
    allp = [master] + slaves + ([second] if second else [])
    report['nodes'] = [hello_row(p.name, p.hello()) for p in allp]
    take_all(report, [(master, 'master')] + [(p, 'slave') for p in slaves + ([second] if second else [])])
    for p in allp:
        p.send(nx.set_log(True))
    time.sleep(1.0)
    script, plan = _script(a), event_plan(32 if a.short else 128)
    print(f"show: {script[1][1]:.0f} s play (seek at {script[0][1]:.0f} s), {script[2][1] - script[2][0]:.0f} s idle; "
          f"{len(plan)} events; {len(slaves)} slave(s){' + a second master' if second else ''}")
    t0, table = run_show(master, script, plan, second, grab_at=script[1][1] - 1.0)
    for p in slaves:
        judge_slave(report, p.name, p, script, t0, second is not None)
        judge_lock(report, p.name, table, node_id_of(p))
    caps = {p.name: p.captures(t0)[0] for p in slaves}
    per, spread = ledger(plan, caps)
    report['events'] = {'per_slave': per, 'spread_ms': spread}
    for name, r in per.items():
        check(report, f"{name}: 0x04 events (lost / duplicated)", r['lost'] == 0 and r['duplicated'] == 0,
              f"{r['received']}/{r['sent']}, lost {r['lost']}, dup {r['duplicated']}")
    if spread:
        check(report, 'fire-time spread between two slaves', None,
              f"mean {spread['mean']} / p95 {spread['p95']} / max {spread['max']} ms over {spread['n']} events")
    nomem = sum(p.log_count('NO_MEM|enqueue FAILED') for p in allp)
    check(report, 'no NO_MEM in any node log', nomem == 0, f"{nomem} line(s)")
    _restore(allp)
    for p in allp:
        p.close()
    return finish(report, a.report)


def cmd_drive(a):
    report = {'command': 'drive', 'checks': []}
    master = Port(a.master)
    report['nodes'] = [hello_row(master.name, master.hello())]
    take_all(report, [(master, 'master')])
    master.send(nx.set_log(True))
    time.sleep(1.0)
    script, plan = _script(a), event_plan(32 if a.short else 128)
    print(f"driving {script[1][1]:.0f} s show + {len(plan)} events — start `listen` on the slave host first")
    _, table = run_show(master, script, plan, grab_at=script[1][1] - 1.0)
    report['table'] = (table or {}).get('receivers')
    check(report, 'the master hears at least one slave LOCKED',
          any(r['sync_quality'] == SYNC_LOCKED for r in report['table'] or []),
          ', '.join(f"{r['mac'][-8:]} q{r['sync_quality']} gaps {r['sync_gaps']} {r['rssi']} dBm"
                    for r in report['table'] or []) or 'empty table')
    nomem = master.log_count('NO_MEM|enqueue FAILED')
    check(report, 'no NO_MEM in the master log', nomem == 0, f"{nomem} line(s)")
    _restore([master])
    master.close()
    return finish(report, a.report)


def cmd_listen(a):
    """Delivery only: lock, index, stop, 1 Hz, gaps, events — on this host's clock alone."""
    report = {'command': 'listen', 'checks': []}
    slaves = _open(a.slave)
    take_all(report, [(p, 'slave') for p in slaves])
    for p in slaves:
        p.send(nx.set_log(True))
    before = {p.name: (p.hello() or {}).get('sync_gaps') for p in slaves}
    script, plan = _script(a), event_plan(32 if a.short else 128)
    total = script[-1][1] + a.wait
    print(f"listening {total:.0f} s — start `drive` on the master host now")
    t_open = time.time()
    first = {}
    while time.time() - t_open < total:
        for p in slaves:
            if p.name not in first:
                _, idx, _ = p.captures(t_open)
                hit = next((t for t, v in idx if v == SHOW_INDEX), None)
                if hit:
                    first[p.name] = hit
        time.sleep(0.05)
    for p in slaves:
        t0 = first.get(p.name)
        if t0 is None:
            check(report, f"{p.name}: the show arrived", False, f'no CC#100={SHOW_INDEX} in {total:.0f} s')
            continue
        judge_slave(report, p.name, p, script, t0, False, timing=False)
        caps = p.captures(t0 - 1.0)[0]
        per, _ = ledger(plan, {p.name: caps})
        r = per[p.name]
        check(report, f"{p.name}: 0x04 events (lost / duplicated)", r['lost'] == 0 and r['duplicated'] == 0,
              f"{r['received']}/{r['sent']}, lost {r['lost']}, dup {r['duplicated']}")
        h = p.hello() or {}
        g0, g1 = before.get(p.name), h.get('sync_gaps')
        check(report, f"{p.name}: gaps over the run", g0 is not None and g1 is not None,
              f"{g0} -> {g1}, rssi {h.get('rssi')} dBm, lr {h.get('lr_mode')}")
    _restore(slaves)
    for p in slaves:
        p.close()
    return finish(report, a.report)


# ---- LR ------------------------------------------------------------------------------

def set_lr(pattern, mode, port=None, timeout=40.0):
    """SET_LR stores the mode and restarts the node. The read-back counts only from a HELLO the
    node sent AFTER that restart (booted_since): 25/11 the tool read the HELLO SET_LR answers
    before restarting, and a master's table, and called that a result. Returns (port, lr mode);
    mode None = no HELLO from after a restart within `timeout` (the port stays open)."""
    p = port or Port(pattern)
    p.send(nx.set_lr(mode))
    t_sent = time.time()
    time.sleep(0.3)
    p.close()
    end, q = t_sent + timeout, None
    while time.time() < end:
        time.sleep(1.0)
        q = Port(pattern, wait_s=max(0.0, end - time.time()))   # the old port may still be listed
        info = q.hello()
        if info and booted_since(info, t_sent, time.time()):
            # The boot HELLO leaves before setup() loads the stored mode (src/main.cpp): it says
            # the build's default (25/11, B: up=1057 ms lr=off, then 2269 ms lr=250k). Ask again.
            return q, (q.hello() or info).get('lr_mode')
        if time.time() < end:
            q.close()
            q = None
    return q or Port(pattern, wait_s=5.0), None


def stream(master, seconds, events=None, layer=SHOW_LAYER):
    """MEDIA_SYNC at MS_RATE_HZ for `seconds`; `events` = a plan sent back to back each 5 ms."""
    t0, nxt, ei = time.time(), 0.0, 0
    while time.time() - t0 < seconds:
        st = time.time() - t0
        if st >= nxt:
            master.send(nx.media_sync(layer, SHOW_INDEX, st * 1000.0, True))
            nxt += 1.0 / MS_RATE_HZ
        if events:
            send_event(master, events[ei % len(events)])
            ei += 1
            time.sleep(0.005)
        else:
            time.sleep(0.01)
    master.send(nx.media_sync(layer, 0, 0, False))
    return t0, ei


def cmd_lr(a):
    report = {'command': 'lr', 'checks': []}
    lr_on = a.mode
    A, la = set_lr(a.a, lr_on)
    B, lb = set_lr(a.b, 'off')
    C = None
    take_all(report, [(B, 'master'), (A, 'slave')])
    # 1 + 6: an LR node and a plain one must not hear each other, and the LR one shows it.
    stream(B, a.settle)
    rs = B.running_state()
    ha = A.hello() or {}
    heard = heard_in(rs, node_id_of(A))
    check(report, f"1. LR {lr_on} vs plain: they do not hear each other",
          not heard and ha.get('sync_quality') != SYNC_LOCKED,
          f"A sync {ha.get('sync_quality')}, A in B's table: {heard}")
    check(report, "6. the lone LR node says so (HELLO lr + link lost)",
          ha.get('lr_mode') == lr_on and bool(ha.get('link_lost', True)),
          f"lr {ha.get('lr_mode')}, link_lost {ha.get('link_lost')}")
    # 2: both LR lock, the clock included.
    B, lb = set_lr(a.b, lr_on, B)
    B.send(nx.set_role('master'))
    stream(B, a.settle)
    rs = B.running_state()
    ha = A.hello() or {}
    rec = judge_lock(report, f"2. both LR {lr_on}: A", rs, node_id_of(A))
    check(report, "2. A reports LOCKED itself (the clock's broadcast peer flows in LR)",
          ha.get('sync_quality') == SYNC_LOCKED, f"sync {ha.get('sync_quality')}, rssi {ha.get('rssi')} dBm")
    # 3: airtime under MEDIA_SYNC 10 Hz + a max-density CC sweep, 30 s per rate.
    A.send(nx.set_log(True))
    B.send(nx.set_log(True))
    sweep = event_plan(128, notes=())
    for mode in ('250k', '500k'):
        if mode != lr_on:
            A, _ = set_lr(a.a, mode, A)
            B, _ = set_lr(a.b, mode, B)
            B.send(nx.set_role('master'))
            A.send(nx.set_log(True))
            B.send(nx.set_log(True))
            stream(B, a.settle)
        g0 = (A.hello() or {}).get('sync_gaps') or 0
        m0 = A.mark()
        t0, sent = stream(B, a.load, sweep)
        time.sleep(1.0)
        g1 = (A.hello() or {}).get('sync_gaps') or 0
        got = sum(1 for _, m in A.since(m0) if m.type == 'control_change' and m.control == EVENT_CC)
        nomem = A.log_count('NO_MEM|enqueue FAILED') + B.log_count('NO_MEM|enqueue FAILED')
        report.setdefault('airtime', {})[mode] = {'gaps': g1 - g0, 'cc_sent': sent, 'cc_received': got,
                                                  'no_mem': nomem, 'started': t0}
        check(report, f"3. airtime at {mode}: gaps, CC delivered, NO_MEM", None,
              f"gaps +{g1 - g0}, CC {got}/{sent} (lost = stale or never sent), NO_MEM {nomem} — numbers, his read")
    # 4: MTC offset between two slaves on this host, LR off vs on.
    if a.c:
        C = Port(a.c)
        take_all(report, [(C, 'slave')])
        res = {}
        for mode in ('off', lr_on):
            for pat, prt in ((a.a, A), (a.b, B), (a.c, C)):
                np, _ = set_lr(pat, mode, prt)
                if pat == a.a:
                    A = np
                elif pat == a.b:
                    B = np
                else:
                    C = np
            B.send(nx.set_role('master'))
            stream(B, a.settle)
            since = time.time()
            stream(B, a.load)
            with A.lock:
                ta = [x for x in A.mtc if x[0] >= since]
            with C.lock:
                tc = [x for x in C.mtc if x[0] >= since]
            res[mode] = stats(pair_offsets(ta, tc))
        report['mtc_between_slaves'] = res
        check(report, f"4. MTC A vs C, LR off vs {lr_on}", None,
              f"off p95 {res['off']['p95']} ms, {lr_on} p95 {res[lr_on]['p95']} ms — numbers, his read")
    else:
        check(report, "4. MTC between two slaves", None, 'needs --c')
    # leave LR off everywhere, roles auto.
    for pat, prt in ((a.a, A), (a.b, B)) + (((a.c, C),) if C else ()):
        q, mode = set_lr(pat, 'off', prt)
        check(report, f"{q.name}: left with LR off", mode == 'off', f"lr {mode}")
        _restore([q])
        q.close()
    return finish(report, a.report)


def heard_in(rs, node_id):
    """True when node_id is in a master's RUNNING_STATE table."""
    return any(node_id and r['mac'].replace(':', '').endswith(node_id) for r in (rs or {}).get('receivers', []))


def pair_offsets(ta, tc, max_dt=0.05):
    """MTC of two slaves on one host: for each A sample (t, pos), the C sample closest in host
    time (within max_dt), carried to t at play speed -> posA − posC(t), in ms."""
    out, j = [], 0
    for t, p in ta:
        while j + 1 < len(tc) and abs(tc[j + 1][0] - t) <= abs(tc[j][0] - t):
            j += 1
        if tc and abs(tc[j][0] - t) <= max_dt:
            out.append(p - (tc[j][1] + (t - tc[j][0]) * 1000.0))
    return out


# ---- relay (meshmask builds) ----------------------------------------------------------

def cmd_relay(a):
    report = {'command': 'relay', 'checks': []}
    P = _open(a.chain)
    if len(P) != 3:
        raise CouldNotRun('relay needs exactly three ports: master, relay, end')
    for p in P:
        p.send(nx.set_neighbours())
    take_all(report, [(p, 'slave') for p in P])
    macs = {}
    for k in (0, 1):   # P1's table names P2 and P3, P2's names P1: full MACs without QUERY_CONFIG
        P[k].send(nx.set_role('master'))
        stream(P[k], a.settle)
        for r in (P[k].running_state() or {}).get('receivers', []):
            for p in P:
                if node_id_of(p) and r['mac'].replace(':', '').endswith(node_id_of(p)):
                    macs[p.name] = r['mac']
        P[k].send(nx.set_role('slave'))
    if len(macs) < 3:
        check(report, 'the three MACs are known', False, f"{len(macs)} of 3 heard")
        _restore(P)
        return finish(report, a.report)
    m1, m2, m3 = (macs[p.name] for p in P)
    masks = ((m2,), (m1, m3), (m2,))
    for p, mask in zip(P, masks):
        p.send(nx.set_neighbours(mask))
        if p.wait_for(nx.CMD_ERROR_REPORT, 0.3):
            check(report, f"{p.name}: SET_NEIGHBOURS accepted", False, 'ERROR_REPORT: not a meshmask build')
            _restore(P)
            return finish(report, a.report)
        p.send(nx.set_log(True))
    P[0].send(nx.set_role('master'))
    script, plan = _script(a), event_plan(32 if a.short else 128)
    t0, _ = run_show(P[0], script, plan)
    judge_slave(report, f"{P[2].name} (through the relay)", P[2], script, t0, False)
    h3 = P[2].hello() or {}
    check(report, f"{P[2].name}: LOCKED through the relay", h3.get('sync_quality') == SYNC_LOCKED,
          f"sync {h3.get('sync_quality')}, gaps {h3.get('sync_gaps')}")
    per, _ = ledger(plan, {P[2].name: P[2].captures(t0)[0]})
    r = per[P[2].name]
    check(report, f"{P[2].name}: 0x04 events within the lead (late = lost)", r['lost'] == 0 and r['duplicated'] == 0,
          f"{r['received']}/{r['sent']}, lost or late {r['lost']}, dup {r['duplicated']}")
    for p in P:
        p.send(nx.set_neighbours())
    _restore(P)
    for p in P:
        p.close()
    return finish(report, a.report)


# ---- OTA ------------------------------------------------------------------------------

def usbfix_armed():
    """This host's usbfix watchdog is live and not inhibited: it re-enumerates a node whose host
    logs no HELLO -- in the middle of an OTA (pi-tools usbfix/README: 22/09 at 88 %; again 25/11)."""
    if os.path.exists('/run/usbfix.inhibit') or not shutil.which('systemctl'):
        return False
    return subprocess.run(['systemctl', 'is-active', '--quiet', 'usbfix.timer']).returncode == 0


def cmd_ota(a, chunk=100, ack_timeout=0.5, retries=5):
    """nowde-cli's acked path step for step (the one proven on 2.4 nodes, 25/11), plus the
    injected duplicate: nothing between the port opening and OTA_BEGIN, the CLI's 5 resends."""
    if usbfix_armed():
        raise CouldNotRun("usbfix is armed on this host: `sudo touch /run/usbfix.inhibit` first "
                          "(and `sudo rm` it after) -- it re-enumerates a node mid-transfer")
    report = {'command': 'ota', 'checks': []}
    data = open(a.image, 'rb').read()
    want = nx.bin_build_id(data)
    p = Port(a.node)
    p.send(nx.ota_begin(len(data)))
    time.sleep(0.5)
    dup_at = min(10, (len(data) - 1) // chunk) if a.inject_dup else None
    resends = 0
    t0 = time.time()

    def acked(seq, mark):
        for _, m in p.since(mark):
            if m.type == 'sysex' and len(m.data) >= 2 and m.data[1] == nx.CMD_OTA_ACK:
                x = nx.parse(list(m.data))[1]
                if not x.get('end') and x.get('seq') == seq:
                    return x
        return None

    def send_chunk(k, part):
        nonlocal resends
        seq = k % 127
        for _ in range(retries + 1):
            mark = p.mark()
            p.send(nx.ota_data_acked(seq, part))
            end, ack = time.time() + ack_timeout, None
            while time.time() < end and ack is None:
                ack = acked(seq, mark)
                time.sleep(0.002)
            if ack and ack['status'] == 0:
                return True
            if ack and ack['status'] in (2, 3):
                return False
            resends += 1
        return False

    ok, why = True, ''
    for k, i in enumerate(range(0, len(data), chunk)):
        part = data[i:i + chunk]
        ok = send_chunk(k, part) and (k != dup_at or send_chunk(k, part))   # the duplicate, on purpose
        if ok and resends > OTA_MAX_RESENDS:
            ok, why = False, f"; stopped: {resends} resends by chunk {k}, the node acks late (USB starved?)"
        if not ok:
            why = why or (f"; chunk {k} at {i} unacked, " +
                          ('port still listed: the node went silent' if p.listed()
                           else 'the port left USB: a reset or a watchdog'))
            break
    check(report, 'every chunk acked', ok, f"{len(data)} B in {time.time() - t0:.1f} s, {resends} resent{why}")
    verdict = None
    if ok:
        mark = p.mark()
        p.send(nx.ota_end())
        end = time.time() + 15.0
        while time.time() < end and verdict is None:
            for _, m in p.since(mark):
                if m.type == 'sysex' and len(m.data) >= 2 and m.data[1] == nx.CMD_OTA_ACK:
                    x = nx.parse(list(m.data))[1]
                    if x.get('end'):
                        verdict = x
            time.sleep(0.05)
    check(report, 'END: validated', bool(verdict) and verdict['status'] == 4,
          f"{verdict['verdict']}, {verdict['duplicates']} duplicate(s) acked unwritten" if verdict else 'no verdict')
    if a.inject_dup:
        check(report, 'the injected duplicate was acked and not written', bool(verdict) and verdict['duplicates'] >= 1,
              f"{verdict['duplicates'] if verdict else '?'} duplicate(s)")
    p.close()
    time.sleep(3.0)
    q = Port(a.node, wait_s=30.0)
    after = hello_row(q.name, q.hello())
    q.close()
    report['after'] = after
    check(report, 'HELLO shows the image build id', want is not None and after['build_id'] == want,
          f"image {want}, node {after['build_id']} ({after['version']})")
    return finish(report, a.report)


# ---- walk ------------------------------------------------------------------------------

def cmd_lr_set(a):
    report = {'command': 'lr-set', 'checks': []}
    q, mode = set_lr(a.port, a.mode)
    check(report, f"{q.name}: LR {a.mode}", mode == a.mode, f"HELLO lr {mode}")
    q.close()
    return finish(report, a.report)


def cmd_walk(a):
    p = Port(a.master)
    if ((p.hello() or {}).get('lr_mode')) != a.lr:
        p, mode = set_lr(a.master, a.lr, p)
        if mode != a.lr:
            raise CouldNotRun(f"could not set LR {a.lr} (HELLO from after the restart says {mode})")
    p.send(nx.set_role('master'))
    path = a.csv or time.strftime(f'walk-%Y%m%d-%H%M-{a.lr}.csv')
    notes = []
    threading.Thread(target=lambda: [notes.append(line.strip()) for line in sys.stdin], daemon=True).start()
    print(f"walking at LR {a.lr} -> {path}; type a distance (or anything) + Enter to tag the next row; Ctrl-C ends")
    t0, nxt, last_rs = time.time(), 0.0, 0.0
    with open(path, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(WALK_HEADER)
        try:
            while True:
                st = time.time() - t0
                if st >= nxt:
                    p.send(nx.media_sync(WALK_LAYER, SHOW_INDEX, st * 1000.0, True))
                    nxt += 1.0 / MS_RATE_HZ
                if st - last_rs >= 1.0:
                    last_rs = st
                    note = notes.pop(0) if notes else ''
                    rows = walk_rows(time.time(), st, p.running_state(0.3), note)
                    w.writerows(rows)
                    f.flush()
                    r = rows[0]
                    print(f"\r  {r[1]:>7}s  locked {r[3]}  gaps {r[4]}  rssi {r[5]}  {note}   ", end='', flush=True)
                time.sleep(0.01)
        except KeyboardInterrupt:
            pass
    p.send(nx.media_sync(WALK_LAYER, 0, 0, False))
    print(f"\n{path}")
    p.close()
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='nowde-bench', description=__doc__.split('\n')[0])
    ap.add_argument('--report', default=None, help='JSON report path (default nowde-bench-<cmd>-<time>.json)')
    sub = ap.add_subparsers(dest='cmd', required=True)

    def show_args(s):
        s.add_argument('--play', type=float, default=60.0)
        s.add_argument('--idle', type=float, default=60.0)
        s.add_argument('--short', action='store_true', help='6 s play + 6 s idle, 32 CCs: a smoke run')
    sub.add_parser('nodes')
    s = sub.add_parser('accept')
    s.add_argument('--master', required=True)
    s.add_argument('--slave', action='append', required=True)
    s.add_argument('--second-master')
    show_args(s)
    s = sub.add_parser('drive')
    s.add_argument('--master', required=True)
    show_args(s)
    s = sub.add_parser('listen')
    s.add_argument('--slave', action='append', required=True)
    s.add_argument('--wait', type=float, default=60.0, help='extra seconds to wait for the drive to start')
    show_args(s)
    s = sub.add_parser('lr')
    s.add_argument('--a', required=True, help='the slave')
    s.add_argument('--b', required=True, help='the master')
    s.add_argument('--c', help='a second slave on this host (check 4)')
    s.add_argument('--mode', choices=('250k', '500k'), default='250k', help='the LR rate of checks 1, 2, 4, 6')
    s.add_argument('--settle', type=float, default=15.0)
    s.add_argument('--load', type=float, default=30.0)
    s = sub.add_parser('relay')
    s.add_argument('--chain', nargs=3, required=True)
    s.add_argument('--settle', type=float, default=8.0)
    show_args(s)
    s = sub.add_parser('ota')
    s.add_argument('--node', required=True)
    s.add_argument('--image', required=True)
    s.add_argument('--inject-dup', action='store_true')
    s = sub.add_parser('lr-set')
    s.add_argument('--port', required=True)
    s.add_argument('mode', choices=tuple(nx.LR_MODES))
    s = sub.add_parser('walk')
    s.add_argument('--master', required=True)
    s.add_argument('--lr', choices=tuple(nx.LR_MODES), required=True)
    s.add_argument('--csv')
    a = ap.parse_args(argv)
    a.report = a.report or time.strftime(f'nowde-bench-{a.cmd}-%Y%m%d-%H%M%S.json')
    try:
        return {'nodes': cmd_nodes, 'accept': cmd_accept, 'drive': cmd_drive, 'listen': cmd_listen,
                'lr': cmd_lr, 'relay': cmd_relay, 'ota': cmd_ota, 'lr-set': cmd_lr_set,
                'walk': cmd_walk}[a.cmd](a)
    except CouldNotRun as e:
        print(f"could not run: {e}", file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
