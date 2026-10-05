"""Dependency-free checks of nowde_sysex against the byte layouts in docs/PROTOCOL.md."""
import hashlib
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

# v2.1 — the trust layer: HELLO build identity (c.8) and the RUNNING_STATE `alone` bit (c.7).
# Both are trailers, so again the claim is two-sided: the new field parses, the old frame doesn't grow one.
bid = 'ff80a5017f00c3e9'                    # high bits set on purpose: it travels 7-bit encoded
hb = nx.hello('2.1.0', 1000, 1, 1, 2, 2, False, 0, bid)
check(len(hb) + 2 == 36, f"HELLO v2.1 length ({len(hb) + 2})")
p = nx.parse(hb)[1]
check(p['build_id'] == bid and p['sync_gaps'] == 0 and p['lr'] is False, f"HELLO build_id {p}")
check('build_id' not in nx.parse(nx.hello('2.2.0', 1000, 1, 0, 3, 2, True, 640))[1], "HELLO v2.2 unchanged")

ea = nx.running_state_chunks([], 1000, False, alone=True)[0]
check(len(ea) + 2 == 15, f"RUNNING_STATE empty chunk + alone is 15 bytes ({len(ea) + 2})")   # MillluBridge floor is 14
check(nx.parse(ea)[1]['alone'] is True and nx.parse(ea)[1]['total'] == 0, f"RUNNING_STATE alone {nx.parse(ea)[1]}")
check(nx.parse(nx.running_state_chunks([], 1000, False, alone=False)[0])[1]['alone'] is False, "RUNNING_STATE not alone")
check('alone' not in nx.parse(nx.running_state_chunks([], 1, False)[0])[1], "RUNNING_STATE pre-2.1 empty chunk")
# A chunk that carries a record never carries the byte: the record must stay the frame's tail.
ra = nx.running_state_chunks([{'mac': [1, 2, 3, 4, 5, 6], 'layer': 'main'}], 1000, True, alone=False)[0]
check(len(ra) == 2 + 5 + 4 + 1 + 45 and 'alone' not in nx.parse(ra)[1], "RUNNING_STATE record chunk has no alone")

# The id a .bin will report, read without flashing it (app descriptor head at 0xB0).
img = bytearray(0x200)
img[0] = 0xE9
img[0x20:0x24] = nx.APP_DESC_MAGIC.to_bytes(4, 'little')
img[0xB0:0xB8] = bytes.fromhex(bid)
check(nx.bin_build_id(bytes(img)) == bid, "bin_build_id app image")
check(nx.bin_build_id(bytes(nx.APP_IMAGE_OFFSET) + bytes(img)) == bid, "bin_build_id merged image")
check(nx.bin_build_id(bytes(0x200)) is None, "bin_build_id no descriptor")
# ...and on what the gate just built: the descriptor field the firmware reads into HELLO must be
# the ELF's SHA-256. A toolchain that stopped stamping it would put zeros on every unit's wire.
build = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '.pio', 'build')
seen = 0
for env in sorted(os.listdir(build)) if os.path.isdir(build) else []:
    fb, fe = (os.path.join(build, env, 'firmware.' + x) for x in ('bin', 'elf'))
    if os.path.isfile(fb) and os.path.isfile(fe):
        with open(fb, 'rb') as b, open(fe, 'rb') as e:
            got, want = nx.bin_build_id(b.read()), hashlib.sha256(e.read()).hexdigest()[:2 * nx.BUILD_ID_LEN]
        check(got == want, f"{env}: build id {got} != ELF sha256 {want}")
        seen += 1
print(f"build id = ELF sha256 on {seen} built env(s)" if seen else "build id vs ELF: no .pio build here, skipped")
print("nowde_sysex selftest OK")
