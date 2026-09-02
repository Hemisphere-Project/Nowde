"""Dependency-free checks of nowde_sysex against the byte layouts in docs/PROTOCOL.md."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowde_sysex as nx  # noqa: E402


def check(cond, what):
    if not cond:
        raise SystemExit(f"FAIL: {what}")


for raw in ([], [0x80], list(range(256)), [0xFF] * 36):
    check(nx.decode7(nx.encode7(raw)) == raw, f"7-bit roundtrip {len(raw)}")
    check(all(b < 0x80 for b in nx.encode7(raw)), "7-bit safe")

ms = [0xF0] + nx.media_sync('hplayer2', 7, 0xDEADBEEF, True) + [0xF7]
check(len(ms) == 27 and ms[19] == 7 and ms[25] == 1, "MEDIA_SYNC layout")
check(nx.parse(ms[1:-1])[1] == {'layer': 'hplayer2', 'index': 7, 'position_ms': 0xDEADBEEF, 'playing': True}, "MEDIA_SYNC parse")

crl = [0xF0] + nx.change_receiver_layer([1, 2, 3, 4, 5, 6], 'stage') + [0xF7]
check(len(crl) == 30, "CHANGE_RECEIVER_LAYER length")
check(nx.parse(crl[1:-1])[1] == {'mac': '01:02:03:04:05:06', 'layer': 'stage'}, "CHANGE_RECEIVER_LAYER parse")

h = nx.hello('2.0', 100000, 1, 1, 2)
check(len(h) + 2 == 22, "HELLO v2 length")
name, info = nx.parse(h)
check(name == 'HELLO' and info == {'version': '2.0', 'uptime_ms': 100000, 'boot': 'POWERON', 'role': 'master', 'board': 'atoms3'}, f"HELLO parse {info}")
check(len(nx.hello('1.2', 5, 1)) + 2 == 20, "HELLO v1 length")

cs = nx.config_state(False, 400, 0, 3, 'abc')
check(nx.parse(cs)[1]['layer'] == 'abc' and nx.parse(cs)[1]['role'] == 'slave', "CONFIG_STATE parse")

chunks = nx.running_state_chunks([{'mac': [1, 2, 3, 4, 5, 6], 'layer': 'main', 'version': '2.0', 'last_seen_ms': 1000, 'index': 9}], 5000, True)
check(len(chunks) == 1, "one chunk per slave")
_, rs = nx.parse(chunks[0])
check(rs['synced'] and rs['receivers'][0]['layer'] == 'main' and rs['receivers'][0]['index'] == 9 and rs['receivers'][0]['last_seen_ms'] == 1000, f"RUNNING_STATE parse {rs}")
check(len(nx.running_state_chunks([], 1, False)) == 1 and nx.parse(nx.running_state_chunks([], 1, False)[0])[1]['total'] == 0, "empty table chunk")

check(nx.parse(nx.ota_begin(123456))[0] == 'OTA_BEGIN', "OTA_BEGIN")
check(nx.parse(nx.set_role('auto'))[1] == {'role': 'auto'}, "SET_ROLE")
print("nowde_sysex selftest OK")
