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

# 2.0.1: sync_quality trailers (HELLO + RUNNING_STATE record)
hq = nx.hello('2.0.1', 1000, 1, 0, 3, 1)   # slave, board lite, coarse
check(nx.parse(hq)[1].get('sync_quality') == 1, f"HELLO sync_quality trailer {nx.parse(hq)[1]}")
rq = nx.running_state_chunks([{'mac': [1, 2, 3, 4, 5, 6], 'layer': 'main', 'version': '2.0.1',
                               'last_seen_ms': 500, 'index': 2, 'sync_quality': 2}], 1000, True)
check(nx.parse(rq[0])[1]['receivers'][0]['sync_quality'] == 2, f"RUNNING_STATE sync_quality {nx.parse(rq[0])[1]}")

check(nx.parse(nx.ota_begin(123456))[0] == 'OTA_BEGIN', "OTA_BEGIN")
check(nx.parse(nx.set_role('auto'))[1] == {'role': 'auto'}, "SET_ROLE")

# v2.2 — the trailers the broadcast half adds, and the two runtime switches.
# Every one of them is APPENDED, so the test that matters is that an older frame still parses
# into the same fields: that is the whole compat claim, and it is cheap to assert.
hg = nx.hello('2.2.0', 1000, 1, 0, 3, 2, True, 640)
p = nx.parse(hg)[1]
check(p['sync_quality'] == 2 and p['lr'] is True and p['sync_gaps'] == 640, f"HELLO v2.2 trailers {p}")
check('sync_gaps' not in nx.parse(nx.hello('2.0.1', 1, 1, 0, 3, 1))[1], "HELLO 2.0.1 unchanged")

cs22 = nx.config_state(False, 400, 0, 3, 'abc', stop_on_link_lost=False,
                       origin=[0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33], origin_pinned=True)
p = nx.parse(cs22)[1]
check(p['layer'] == 'abc' and p['stop_on_link_lost'] is False and p['origin_pinned']
      and p['origin'] == 'AA:BB:CC:11:22:33', f"CONFIG_STATE v2.2 tail {p}")
check(nx.parse(nx.config_state(False, 400, 0, 3, 'abc', stop_on_link_lost=True,
                               origin=None))[1]['origin'] is None, "CONFIG_STATE no origin yet")
check('origin' not in nx.parse(nx.config_state(False, 400, 0, 3, 'abc'))[1], "CONFIG_STATE v2 unchanged")

rg = nx.running_state_chunks([{'mac': [1, 2, 3, 4, 5, 6], 'layer': 'main', 'version': '2.2.0',
                               'last_seen_ms': 500, 'index': 2, 'sync_quality': 2,
                               'sync_gaps': 4097}], 1000, True)
r0 = nx.parse(rg[0])[1]['receivers'][0]
check(r0['sync_gaps'] == 4097 and r0['sync_quality'] == 2 and r0['index'] == 2
      and r0['last_seen_ms'] == 500, f"RUNNING_STATE sync_gaps {r0}")
# The record STRIDE (39 raw -> 45 encoded) is invisible while RECEIVERS_PER_CHUNK is 1: with
# one record there is nothing after it to land wrong. Pin it both ways, or the day the
# firmware packs two per chunk the parser silently reads the second one off by two bytes.
check(len(rg[0]) == 2 + 5 + 4 + 1 + 45, f"RUNNING_STATE record is 45 encoded bytes ({len(rg[0])})")
two = rg[0] + nx.encode7(list(range(39)))   # a second record glued into the same chunk
two[11] = 2                                 # slavesInChunk (after 7D 22 uptime(5) synced total chunk chunks)
check([r['index'] for r in nx.parse(two)[1]['receivers']] == [2, 35], "RUNNING_STATE 2-record stride")

check(nx.parse(nx.set_loss_policy(False))[1] == {'policy': 'freewheel'}, "SET_LOSS_POLICY")
check(nx.parse(nx.set_origin([1, 2, 3, 4, 5, 0xF6]))[1] == {'origin': '01:02:03:04:05:F6'}, "SET_ORIGIN pin")
check(nx.parse(nx.set_origin())[1] == {'origin': None}, "SET_ORIGIN release")
print("nowde_sysex selftest OK")
