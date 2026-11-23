#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["mido>=1.3", "python-rtmidi>=1.5"]
# ///
"""nowde-cli — drive a Nowde node from a laptop over USB-MIDI, no host application needed.

  nowde-cli ports                         list MIDI ports (Nowde ones marked)
  nowde-cli hello                         QUERY_CONFIG handshake: HELLO + CONFIG_STATE (turns a legacy node into a sender)
  nowde-cli probe                         QUERY_RUNNING_STATE: HELLO from a v2 node after a silence, silent on a v1.2 receiver
  nowde-cli watch [-t SEC] [--no-log]     print everything the node sends (SysEx decoded, MTC as timecode, CC) + its log (v2)
  nowde-cli slaves                        the sender's slave table (RUNNING_STATE)
  nowde-cli set-role master|slave|auto    store the role on the node (v2)
  nowde-cli set-layer NAME                set the node's own layer (v2)
  nowde-cli assign MAC LAYER              re-layer a remote slave through this sender (CHANGE_RECEIVER_LAYER)
  nowde-cli play INDEX [-l LAYER] [-d SEC] [-t SEC] [--from MS] [--rate HZ] [--no-stop]
                                          stream MEDIA_SYNC playing INDEX (loop of SEC seconds) for -t seconds or until Ctrl-C
  nowde-cli stop [-l LAYER]               one MEDIA_SYNC with state=stopped
  nowde-cli rfsim on|off [DELAY_MS]       RF simulation (random send delay) on the sender
  nowde-cli ota FIRMWARE.bin [--blind]     firmware update over USB-MIDI: acked 100 B chunks, ends on the node's verdict
  nowde-cli build-id FIRMWARE.bin...      the build id each image will report in HELLO (v2.1) — compare with `hello`

Port selection: -p PATTERN (regex on the port name, default ^Nowde). Run with uv:
  uv run tools/nowde-cli.py hello
"""
import argparse
import os
import re
import sys
import time
import threading

import mido

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowde_sysex as nx  # noqa: E402


class Node:
    def __init__(self, pattern, quiet=False):
        self.rx = re.compile(pattern)
        self.quiet = quiet
        self.received = []
        self.lock = threading.Lock()
        self.qf = [0] * 8
        ins = [n for n in mido.get_input_names() if self.rx.search(n)]
        outs = [n for n in mido.get_output_names() if self.rx.search(n)]
        if not ins or not outs:
            sys.exit(f"no MIDI port matching /{pattern}/ (inputs: {mido.get_input_names()})")
        self.inp = mido.open_input(ins[0], callback=self._on)
        self.out = mido.open_output(outs[0])
        self.name = ins[0]

    def close(self):
        self.inp.close()
        self.out.close()

    def _on(self, msg):
        with self.lock:
            self.received.append((time.time(), msg))
        if self.quiet:
            return
        if msg.type == 'sysex':
            if msg.data and msg.data[0] == nx.MANUFACTURER:
                name, info = nx.parse(list(msg.data))
                if name == 'LOG':
                    print(f"\n  node| {info['text']}")
                else:
                    print(f"\n  <- {name} {info}")
            elif len(msg.data) == 8 and tuple(msg.data[0:4]) == (127, 127, 1, 1):
                h, m, s, f = msg.data[4] & 0x1F, msg.data[5], msg.data[6], msg.data[7]
                print(f"\n  <- MTC full-frame {h:02d}:{m:02d}:{s:02d}:{f:02d}")
            else:
                print(f"\n  <- sysex {' '.join('%02X' % b for b in msg.data)}")
        elif msg.type == 'quarter_frame':
            self.qf[msg.frame_type] = msg.frame_value
            if msg.frame_type == 7:
                fr = self.qf[0] | (self.qf[1] << 4)
                sec = self.qf[2] | (self.qf[3] << 4)
                mn = self.qf[4] | (self.qf[5] << 4)
                hr = self.qf[6] | ((self.qf[7] & 1) << 4)
                print(f"\r  <- MTC {hr:02d}:{mn:02d}:{sec:02d}:{fr:02d}   ", end='', flush=True)
        elif msg.type == 'control_change':
            print(f"\n  <- CC#{msg.control} = {msg.value} (ch {msg.channel + 1})")
        elif msg.type in ('start', 'stop', 'continue'):
            print(f"\n  <- MIDI {msg.type.upper()}")
        else:
            print(f"  <- {msg}")

    def send(self, payload, label=None):
        if label:
            print(f"  -> {label}")
        self.out.send(mido.Message('sysex', data=payload))

    def wait_for(self, cmd, timeout=2.0):
        end = time.time() + timeout
        seen = len(self.received)
        while time.time() < end:
            with self.lock:
                for t, m in self.received[seen:]:
                    if m.type == 'sysex' and len(m.data) >= 2 and m.data[0] == nx.MANUFACTURER and m.data[1] == cmd:
                        return nx.parse(list(m.data))[1]
                seen = len(self.received)
            time.sleep(0.02)
        return None


def cmd_ports(a):
    for kind, names in (('in ', mido.get_input_names()), ('out', mido.get_output_names())):
        for n in names:
            print(f"{kind}  {'*' if re.search(a.port, n) else ' '} {n}")


def cmd_hello(a):
    n = Node(a.port)
    n.send(nx.query_config(), 'QUERY_CONFIG')
    if not n.wait_for(nx.CMD_HELLO):
        print("  (no HELLO within 2 s)")
    time.sleep(0.3)
    n.close()


def cmd_probe(a):
    n = Node(a.port)
    n.send(nx.query_running_state(), 'QUERY_RUNNING_STATE')
    time.sleep(1.0)
    n.close()


def cmd_watch(a):
    n = Node(a.port)
    print(f"watching {n.name} for {a.time or 'ever'} s, Ctrl-C to stop")
    if not a.no_log:
        n.send(nx.set_log(True))          # v2: the node's log comes along as LOG frames
    try:
        end = time.time() + a.time if a.time else None
        while end is None or time.time() < end:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    if not a.no_log:
        n.send(nx.set_log(False))
        time.sleep(0.1)
    n.close()


def cmd_slaves(a):
    n = Node(a.port, quiet=True)
    n.send(nx.query_running_state())
    time.sleep(1.0)
    rows = []
    synced = None
    alone = None
    with n.lock:
        for t, m in n.received:
            if m.type == 'sysex' and len(m.data) >= 2 and m.data[0] == nx.MANUFACTURER and m.data[1] == nx.CMD_RUNNING_STATE:
                _, info = nx.parse(list(m.data))
                synced = info['synced']
                alone = info.get('alone', alone)
                rows.extend(info['receivers'])
    if synced is None:
        print("no RUNNING_STATE: the node is not a sender (slave role, or legacy node before `hello`)")
    else:
        qname = {0: 'NONE', 1: 'coarse', 2: 'LOCKED', 0xFF: '?'}
        locked = sum(1 for r in rows if r.get('sync_quality') == 2)
        # v2.1: `alone` = no peer heard since boot, a lone master doing its job — not a lost mesh
        print(f"mesh clock {'SYNCED' if synced else 'not synced'} · {len(rows)} slave(s) · {locked} LOCKED"
              + (" · alone (no peer heard since boot)" if alone else ""))
        for r in rows:
            q = qname.get(r.get('sync_quality', 0xFF), '?')
            # v2.2: `missed` is what replaced the MAC ACK. Broadcast MEDIA_SYNC gets no
            # per-frame feedback, so a slave that hears half the fleet's traffic looks
            # perfectly healthy on every other column here.
            print(f"  {r['mac']}  lock={q:<6} layer={r['layer']:<16} v{r['version']:<6} "
                  f"index={r['index']:<3} missed={r.get('sync_gaps', 0):<5} seen {r['last_seen_ms']} ms ago")
    n.close()


def cmd_set_role(a):
    n = Node(a.port)
    n.send(nx.set_role(a.role), f'SET_ROLE {a.role}')
    n.wait_for(nx.CMD_HELLO)
    time.sleep(0.3)
    n.close()


def cmd_set_layer(a):
    n = Node(a.port)
    n.send(nx.set_local_layer(a.layer), f'SET_LOCAL_LAYER {a.layer}')
    n.wait_for(nx.CMD_CONFIG_STATE)
    time.sleep(0.3)
    n.close()


def cmd_set_loss(a):
    """v2.2: freewheel vs stop on link loss, without a reflash."""
    n = Node(a.port)
    n.send(nx.set_loss_policy(a.policy == 'stop'), f'SET_LOSS_POLICY {a.policy}')
    n.wait_for(nx.CMD_CONFIG_STATE)
    time.sleep(0.3)
    n.close()


def cmd_set_origin(a):
    """v2.2: pin this slave to one master, or release it. Under broadcast delivery nothing
    else keeps two installations in earshot of each other apart."""
    mac = None
    if a.mac and a.mac != 'release':
        mac = [int(x, 16) for x in a.mac.replace('-', ':').split(':')]
        if len(mac) != 6:
            sys.exit("MAC must be 6 hex bytes, e.g. A0:B1:C2:D3:E4:F5 (or `release`)")
    n = Node(a.port)
    n.send(nx.set_origin(mac), f'SET_ORIGIN {a.mac or "release"}')
    n.wait_for(nx.CMD_CONFIG_STATE)
    time.sleep(0.3)
    n.close()


def cmd_assign(a):
    mac = [int(x, 16) for x in a.mac.replace('-', ':').split(':')]
    if len(mac) != 6:
        sys.exit("MAC must be 6 hex bytes, e.g. A0:B1:C2:D3:E4:F5")
    n = Node(a.port)
    n.send(nx.change_receiver_layer(mac, a.layer), f'CHANGE_RECEIVER_LAYER {a.mac} -> {a.layer}')
    time.sleep(1.0)
    n.close()


def cmd_play(a):
    n = Node(a.port, quiet=not a.verbose)
    n.send(nx.query_config(), 'QUERY_CONFIG')
    time.sleep(0.3)
    period = 1.0 / a.rate
    start = time.time()
    print(f"streaming MEDIA_SYNC layer={a.layer} index={a.index} loop={a.duration}s at {a.rate} Hz — Ctrl-C to stop")
    try:
        while not a.time or time.time() - start < a.time:
            pos = int(a.start + ((time.time() - start) * 1000) % (a.duration * 1000))
            n.send(nx.media_sync(a.layer, a.index, pos, True))
            print(f"\r  -> {pos // 60000:02d}:{(pos // 1000) % 60:02d}.{pos % 1000:03d}", end='', flush=True)
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    print()
    if a.no_stop:
        print("  (no stop frame: the slaves will go LINK LOST after 10 s)")
    else:
        n.send(nx.media_sync(a.layer, 0, 0, False), 'MEDIA_SYNC stop')
    time.sleep(0.2)
    n.close()


def cmd_stop(a):
    n = Node(a.port)
    n.send(nx.media_sync(a.layer, 0, 0, False), 'MEDIA_SYNC stop')
    time.sleep(0.2)
    n.close()


def cmd_rfsim(a):
    n = Node(a.port)
    n.send(nx.push_full_config(a.state == 'on', a.delay), f'PUSH_FULL_CONFIG rfsim={a.state} delay={a.delay}ms')
    n.wait_for(nx.CMD_CONFIG_STATE)
    time.sleep(0.2)
    n.close()


def _ota_acks(n, mark):
    """OTA_ACK frames received since index `mark` of n.received, parsed, oldest first."""
    with n.lock:
        msgs = n.received[mark:]
    out = []
    for _, m in msgs:
        if m.type == 'sysex' and len(m.data) >= 2 and m.data[0] == nx.MANUFACTURER and m.data[1] == nx.CMD_OTA_ACK:
            out.append(nx.parse(list(m.data))[1])
    return out


def cmd_ota(a):
    """2.4: no QUERY_CONFIG first (OTA is not sender-gated since 2.0.3, and QUERY_CONFIG turns a
    legacy node into a sender). Default is the acked path, OTA_DATA_ACKED: <= 100 B chunks, one
    at a time, each resent until the node acks it -- a duplicate it already wrote is acked and not
    written again (2.4). The run ends on the node's END verdict, not on a port that vanished.
    --blind is the pre-2.0.3 stream (OTA_DATA, no acks), in <= 100 B chunks too: 224 B chunks made
    a 260 B frame the node's 8-bit length wrapped. Exit 0 only on a validated image."""
    data = open(a.firmware, 'rb').read()
    n = Node(a.port, quiet=True)
    print(f"OTA {len(data)} bytes from {a.firmware} to {n.name} ({'blind' if a.blind else 'acked'})")
    n.send(nx.ota_begin(len(data)), 'OTA_BEGIN')
    time.sleep(0.5)
    chunk = 100
    sent = resends = 0
    t0 = time.time()
    for k, i in enumerate(range(0, len(data), chunk)):
        part = data[i:i + chunk]
        if a.blind:
            n.send(nx.ota_data(part))
            time.sleep(a.pace)
        else:
            seq = k % 127                              # 0x7F is OTA_END's verdict
            for attempt in range(a.retries + 1):
                mark = len(n.received)
                n.send(nx.ota_data_acked(seq, part))
                end = time.time() + a.ack_timeout
                ack = None
                while time.time() < end and ack is None:
                    ack = next((x for x in _ota_acks(n, mark) if not x.get('end') and x.get('seq') == seq), None)
                    if ack is None:
                        time.sleep(0.002)
                if ack and ack['status'] == 0:
                    break
                if ack and ack['status'] in (2, 3):
                    print(f"\n  chunk {k} at {i}: {ack['verdict']} -- the node closed the session")
                    n.close()
                    sys.exit(1)
                resends += 1
            else:
                print(f"\n  chunk {k} at {i}: no ok ack after {a.retries + 1} sends -- giving up")
                n.close()
                sys.exit(1)
        sent += len(part)
        if k % 50 == 0:
            print(f"\r  {100 * sent // len(data):3d}%  {sent}/{len(data)}", end='', flush=True)
    print(f"\r  100%  {sent}/{len(data)} in {time.time() - t0:.1f}s, {resends} resent")
    mark = len(n.received)
    n.send(nx.ota_end(), 'OTA_END')
    end = time.time() + 15.0
    verdict = None
    while time.time() < end and verdict is None:
        verdict = next((x for x in _ota_acks(n, mark) if x.get('end')), None)
        time.sleep(0.05)
    n.close()
    if verdict is None:
        print("  no END verdict (a node before 2.4 sends none: check its HELLO build id after the reboot)")
        sys.exit(2)
    print(f"  END: {verdict['verdict']}, {verdict['duplicates']} duplicate chunk(s) acked unwritten")
    sys.exit(0 if verdict['status'] == 4 else 1)


def cmd_build_id(a):
    for path in a.firmware:
        with open(path, 'rb') as fh:
            bid = nx.bin_build_id(fh.read())
        print(f"{bid or '(no app descriptor)':<16}  {path}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('-p', '--port', default='^Nowde', help='regex on the MIDI port name (default ^Nowde)')
    sp = p.add_subparsers(dest='cmd', required=True)
    sp.add_parser('ports').set_defaults(f=cmd_ports)
    sp.add_parser('hello').set_defaults(f=cmd_hello)
    sp.add_parser('probe').set_defaults(f=cmd_probe)
    w = sp.add_parser('watch'); w.add_argument('-t', '--time', type=float, default=0)
    w.add_argument('--no-log', action='store_true', help='do not ask the node for its log (v1.2 nodes)'); w.set_defaults(f=cmd_watch)
    sp.add_parser('slaves').set_defaults(f=cmd_slaves)
    r = sp.add_parser('set-role'); r.add_argument('role', choices=['master', 'slave', 'auto']); r.set_defaults(f=cmd_set_role)
    l = sp.add_parser('set-layer'); l.add_argument('layer'); l.set_defaults(f=cmd_set_layer)
    lp = sp.add_parser('set-loss'); lp.add_argument('policy', choices=['stop', 'freewheel']); lp.set_defaults(f=cmd_set_loss)
    og = sp.add_parser('set-origin'); og.add_argument('mac', nargs='?', help='master MAC, or `release`'); og.set_defaults(f=cmd_set_origin)
    g = sp.add_parser('assign'); g.add_argument('mac'); g.add_argument('layer'); g.set_defaults(f=cmd_assign)
    y = sp.add_parser('play'); y.add_argument('index', type=int); y.add_argument('-l', '--layer', default='hplayer2')
    y.add_argument('-d', '--duration', type=float, default=60.0, help='loop length in s')
    y.add_argument('--from', dest='start', type=int, default=0, help='start position ms')
    y.add_argument('--rate', type=float, default=10.0); y.add_argument('-v', '--verbose', action='store_true')
    y.add_argument('-t', '--time', type=float, default=0, help='stream for this many s then stop (default: until Ctrl-C)')
    y.add_argument('--no-stop', action='store_true', help='leave without the stop frame (link-lost test)')
    y.set_defaults(f=cmd_play)
    s = sp.add_parser('stop'); s.add_argument('-l', '--layer', default='hplayer2'); s.set_defaults(f=cmd_stop)
    f = sp.add_parser('rfsim'); f.add_argument('state', choices=['on', 'off']); f.add_argument('delay', type=int, nargs='?', default=400); f.set_defaults(f=cmd_rfsim)
    o = sp.add_parser('ota'); o.add_argument('firmware')
    o.add_argument('--blind', action='store_true', help='pre-2.0.3 node: OTA_DATA stream, no acks')
    o.add_argument('--pace', type=float, default=0.004, help='--blind: s between chunks')
    o.add_argument('--ack-timeout', type=float, default=0.5, help='s to wait for a chunk ack')
    o.add_argument('--retries', type=int, default=5, help='resends per chunk before giving up')
    o.set_defaults(f=cmd_ota)
    b = sp.add_parser('build-id'); b.add_argument('firmware', nargs='+'); b.set_defaults(f=cmd_build_id)
    a = p.parse_args()
    a.f(a)


if __name__ == '__main__':
    main()
