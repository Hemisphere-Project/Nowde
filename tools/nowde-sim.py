#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["mido>=1.3", "python-rtmidi>=1.5"]
# ///
"""nowde-sim — a software Nowde node on ALSA virtual MIDI ports, for hosts with no hardware.

  nowde-sim master [--name NAME] [--slaves N]
      Looks like a v2 master node: answers HELLO (role=master, board=atoms3) to QUERY_CONFIG
      and to the first QUERY_RUNNING_STATE after a silence, keeps a fake slave table, prints
      every MEDIA_SYNC / CHANGE_RECEIVER_LAYER / SET_* the host sends. Run HPlayer2's
      biennale.py next to it and the profile takes the master leg.

  nowde-sim slave [--name NAME] [--index N] [--loop SEC] [--fps 30] [--legacy]
      Looks like a slave node: HELLO (role=slave, board=atoms3-lite) — or nothing at all with
      --legacy, like a v1.2 receiver — then plays a loop: CC#100=N, MTC full-frame, MIDI Start,
      quarter-frames at 30 fps, CC#100 repeated every second. HPlayer2 chases it as if a
      master were on the mesh. Keys: space play/stop, 1-9 change index, q quit.

The port enumerates as "<NAME>:<NAME>" (default NAME "Nowde - SIM"), which HPlayer2's ^Nowde
filter matches. Needs /dev/snd/seq (audio group), no root, no kernel module.
"""
import argparse
import os
import select
import sys
import termios
import threading
import time
import tty

import mido

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowde_sysex as nx  # noqa: E402


def open_ports(name, on_msg):
    to_host = mido.open_output(name, virtual=True, client_name=name)
    from_host = mido.open_input(name, virtual=True, client_name=name, callback=on_msg)
    return to_host, from_host


def sx(payload):
    return mido.Message('sysex', data=payload)


def run_master(a):
    state = {'last_rx': 0.0, 'role': 1}
    slaves = [{'mac': [0xA0, 0xB1, 0xC2, 0xD3, 0xE4, i], 'layer': '*', 'version': '2.0', 'last_seen_ms': 300, 'index': 0}
              for i in range(a.slaves)]
    t0 = time.time()

    def uptime():
        return int((time.time() - t0) * 1000)

    def on_msg(msg):
        now = time.time()
        resumed = now - state['last_rx'] > 5.0
        state['last_rx'] = now
        if msg.type != 'sysex' or not msg.data or msg.data[0] != nx.MANUFACTURER:
            print(f"  <- {msg}")
            return
        name, info = nx.parse(list(msg.data))
        cmd = msg.data[1]
        if cmd == nx.CMD_QUERY_CONFIG:
            print("  <- QUERY_CONFIG  -> HELLO + CONFIG_STATE")
            to_host.send(sx(nx.hello('2.0', uptime(), 1, state['role'], 2)))
            to_host.send(sx(nx.config_state(False, 400, state['role'], 2, 'hplayer2')))
        elif cmd == nx.CMD_QUERY_RUNNING_STATE:
            if resumed:
                print("  <- QUERY_RUNNING_STATE (host resumed) -> HELLO")
                to_host.send(sx(nx.hello('2.0', uptime(), 1, state['role'], 2)))
            for c in nx.running_state_chunks(slaves, uptime(), True):
                to_host.send(sx(c))
        elif cmd == nx.CMD_MEDIA_SYNC:
            for s in slaves:
                s['index'] = info['index'] if info['playing'] else 0
            print(f"\r  <- MEDIA_SYNC layer={info['layer']} index={info['index']} pos={info['position_ms']} {'PLAY' if info['playing'] else 'stop'}   ", end='', flush=True)
        elif cmd == nx.CMD_CHANGE_RECEIVER_LAYER:
            print(f"\n  <- CHANGE_RECEIVER_LAYER {info}")
            for s in slaves:
                if ':'.join('%02X' % b for b in s['mac']) == info['mac']:
                    s['layer'] = info['layer']
        elif cmd == nx.CMD_SET_ROLE:
            print(f"\n  <- SET_ROLE {info}")
            state['role'] = {'slave': 0, 'master': 1}.get(info['role'], 1)
            to_host.send(sx(nx.hello('2.0', uptime(), 1, state['role'], 2)))
        else:
            print(f"\n  <- {name} {info}")

    to_host, from_host = open_ports(a.name, on_msg)
    print(f"master node '{a.name}' up with {a.slaves} fake slave(s) — Ctrl-C to quit")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    from_host.close()
    to_host.close()


def run_slave(a):
    state = {'last_rx': 0.0, 'playing': False, 'index': a.index, 't0': 0.0, 'lastcc': 0.0}
    boot = time.time()

    def uptime():
        return int((time.time() - boot) * 1000)

    def on_msg(msg):
        now = time.time()
        resumed = now - state['last_rx'] > 5.0
        state['last_rx'] = now
        if a.legacy or msg.type != 'sysex' or not msg.data or msg.data[0] != nx.MANUFACTURER:
            return
        cmd = msg.data[1]
        if cmd == nx.CMD_QUERY_CONFIG or (cmd == nx.CMD_QUERY_RUNNING_STATE and resumed):
            to_host.send(sx(nx.hello('2.0', uptime(), 1, 0, 3)))
            if cmd == nx.CMD_QUERY_CONFIG:
                to_host.send(sx(nx.config_state(False, 400, 0, 3, '*')))
        elif cmd == nx.CMD_SET_LOCAL_LAYER:
            to_host.send(sx(nx.config_state(False, 400, 0, 3, nx.parse(list(msg.data))[1]['layer'])))

    to_host, from_host = open_ports(a.name, on_msg)

    def send_qf(pos_ms):
        total = int(pos_ms * a.fps / 1000)
        fr, sec, mn, hr = total % a.fps, (total // a.fps) % 60, (total // (a.fps * 60)) % 60, (total // (a.fps * 3600)) % 24
        rate = {24: 0, 25: 1, 30: 3}.get(a.fps, 3)
        pieces = [fr & 0xF, (fr >> 4) & 1, sec & 0xF, (sec >> 4) & 3, mn & 0xF, (mn >> 4) & 3, hr & 0xF, ((hr >> 4) & 1) | (rate << 1)]
        for i, v in enumerate(pieces):
            to_host.send(mido.Message('quarter_frame', frame_type=i, frame_value=v))
        return hr, mn, sec, fr

    def full_frame(pos_ms):
        total = int(pos_ms * a.fps / 1000)
        fr, sec, mn, hr = total % a.fps, (total // a.fps) % 60, (total // (a.fps * 60)) % 60, (total // (a.fps * 3600)) % 24
        rate = {24: 0, 25: 1, 30: 3}.get(a.fps, 3)
        to_host.send(mido.Message('sysex', data=[0x7F, 0x7F, 0x01, 0x01, (rate << 5) | hr, mn, sec, fr]))

    def start():
        state['playing'] = True
        state['t0'] = time.time()
        to_host.send(mido.Message('control_change', channel=0, control=100, value=state['index']))
        full_frame(0)
        to_host.send(mido.Message('start'))
        state['lastcc'] = time.time()
        print(f"\n  PLAY index {state['index']}")

    def stop():
        state['playing'] = False
        to_host.send(mido.Message('control_change', channel=0, control=100, value=0))
        to_host.send(mido.Message('stop'))
        print("\n  STOP")

    def keys():
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setcbreak(fd)
            while True:
                if select.select([sys.stdin], [], [], 0.1)[0]:
                    ch = sys.stdin.read(1)
                    if ch == 'q':
                        os._exit(0)
                    elif ch == ' ':
                        stop() if state['playing'] else start()
                    elif ch.isdigit() and ch != '0':
                        state['index'] = int(ch)
                        if state['playing']:
                            start()
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)

    print(f"slave node '{a.name}' ({'legacy v1.2' if a.legacy else 'v2'}) — space play/stop, 1-9 index, q quit")
    if sys.stdin.isatty():
        threading.Thread(target=keys, daemon=True).start()
    if a.autoplay:
        start()
    period = 1.0 / a.fps
    try:
        while True:
            if state['playing']:
                pos = ((time.time() - state['t0']) * 1000) % (a.loop * 1000)
                hr, mn, sec, fr = send_qf(pos)
                print(f"\r  MTC {hr:02d}:{mn:02d}:{sec:02d}:{fr:02d}  idx {state['index']}   ", end='', flush=True)
                if time.time() - state['lastcc'] >= 1.0:
                    to_host.send(mido.Message('control_change', channel=0, control=100, value=state['index']))
                    state['lastcc'] = time.time()
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    from_host.close()
    to_host.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest='role', required=True)
    m = sp.add_parser('master'); m.add_argument('--name', default='Nowde - SIM'); m.add_argument('--slaves', type=int, default=2); m.set_defaults(f=run_master)
    s = sp.add_parser('slave'); s.add_argument('--name', default='Nowde - SIM'); s.add_argument('--index', type=int, default=1)
    s.add_argument('--loop', type=float, default=60.0, help='loop length s'); s.add_argument('--fps', type=int, default=30)
    s.add_argument('--legacy', action='store_true', help='v1.2 receiver: no HELLO ever'); s.add_argument('--autoplay', action='store_true'); s.set_defaults(f=run_slave)
    a = p.parse_args()
    a.f(a)


if __name__ == '__main__':
    main()
