"""Dependency-free checks of nowde_sysex / nowde_mesh against the byte layouts in
docs/PROTOCOL.md."""
import hashlib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowde_mesh as nm  # noqa: E402
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

# 2.0.4 — the volume tail. Offsets are asserted against src/sysex.cpp, which reads data[26]
# and data[27] and only at length >= 29; the 27-byte form above must stay byte-identical,
# because that is the whole mixed-fleet compatibility claim (a 2.0.3 host keeps driving a
# 2.0.4 node, and a 2.0.4 host carrying no volume leaves a node's level alone).
msv = [0xF0] + nx.media_sync('hplayer2', 7, 0xDEADBEEF, True, volume=55) + [0xF7]
check(len(msv) == 29 and msv[26] == 55 and msv[27] == nx.MEDIASYNC_FLAG_VOLUME, f"MEDIA_SYNC 2.0.4 tail {msv[26:28]}")
check(msv[:26] == ms[:26], "MEDIA_SYNC 2.0.4 tail is appended, head untouched")
check(all(b < 0x80 for b in msv[1:-1]), "MEDIA_SYNC 2.0.4 tail is 7-bit safe")
check(nx.parse(msv[1:-1])[1] == {'layer': 'hplayer2', 'index': 7, 'position_ms': 0xDEADBEEF,
                                 'playing': True, 'volume': 55, 'flags': 1}, f"MEDIA_SYNC 2.0.4 parse {nx.parse(msv[1:-1])[1]}")
# "No volume carried" must be distinguishable from "volume 0", or a silent master mutes the fleet.
check('volume' not in nx.parse(ms[1:-1])[1], "MEDIA_SYNC 27-byte frame carries no volume")
check(nx.parse(nx.media_sync('l', 1, 0, True, volume=0))[1]['volume'] == 0, "MEDIA_SYNC volume 0 is carried")
# The firmware clamps to 100 on the node side; the builder must not emit what it would clamp.
check(nx.media_sync('l', 1, 0, True, volume=255)[-2] == 100, "MEDIA_SYNC volume clamps to 100")
check(nx.media_sync('l', 1, 0, True, flags=nx.MEDIASYNC_FLAG_MUTE)[-1] == 2, "MEDIA_SYNC explicit flags")

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
# 2.4: nowde-cli ota sends <= 100 B chunks on both paths. A node before 2.4 reads the SysEx length
# as 8 bits, so every frame the CLI sends must stay under 256 B on the wire (F0 + payload + F7).
check(len(nx.ota_data_acked(126, bytes(100))) + 2 < 256 and len(nx.ota_data(bytes(100))) + 2 < 256,
      "nowde-cli's 100 B OTA chunks fit an 8-bit SysEx length")
check(len(nx.ota_data(bytes(224))) + 2 > 255, "the old 224 B chunk did not (why the CLI changed)")

# 2.4 (#t-046) — the acked OTA path and OTA_END's verdict. Byte layouts per PROTOCOL.md 0x0C / 0x23
# and src/sysex.cpp (the chunk ack is 6 bytes on the wire, the END verdict 8).
ch = bytes(range(100))
oda = nx.ota_data_acked(5, ch)
check(oda[:4] == [0x7D, 0x0C, 5, 100] and nx.parse(oda)[1] == {'seq': 5, 'len': 100, 'data': list(ch)}, "OTA_DATA_ACKED layout")
check(all(b < 0x80 for b in oda), "OTA_DATA_ACKED 7-bit safe")
ca = nx.ota_ack(5, 0)
check(len(ca) + 2 == 6 and nx.parse(ca)[1] == {'end': False, 'seq': 5, 'status': 0, 'verdict': 'ok'}, "OTA_ACK chunk")
check(nx.parse(nx.ota_ack(127, 1))[1]['end'] is False, "a 6-byte ack on seq 127 is still a chunk ack")
ev = nx.ota_ack(nx.OTA_ACK_SEQ_END, 4, duplicates=200)
check(len(ev) + 2 == 8 and ev[4:] == [200 >> 7, 200 & 0x7F], f"OTA_END verdict layout {ev}")
check(nx.parse(ev)[1] == {'end': True, 'status': 4, 'verdict': 'validated', 'duplicates': 200}, "OTA_END verdict parse")
for st, v in ((3, 'no-session'), (5, 'size-mismatch'), (6, 'invalid')):
    check(nx.parse(nx.ota_ack(0x7F, st, duplicates=0))[1]['verdict'] == v, f"OTA_END verdict {v}")
check(nx.parse(nx.ota_ack(0x7F, 4, duplicates=99999))[1]['duplicates'] == 0x3FFF, "OTA_END dups saturate at 14 bits")
check(nx.parse(nx.set_role('auto'))[1] == {'role': 'auto'}, "SET_ROLE")

# v2.2 — the trailers the broadcast half adds, and the two runtime switches.
# Every one of them is APPENDED, so the test that matters is that an older frame still parses
# into the same fields: that is the whole compat claim, and it is cheap to assert.
hg = nx.hello('2.2.0', 1000, 1, 0, 3, 2, True, (False, 120, 0), 640)
p = nx.parse(hg)[1]
check(p['sync_quality'] == 2 and p['lr'] is True and p['sync_gaps'] == 640, f"HELLO v2.2 trailers {p}")
check('sync_gaps' not in nx.parse(nx.hello('2.0.1', 1, 1, 0, 3, 1))[1], "HELLO 2.0.1 unchanged")

# 2.0.5 — the garden fleet's HELLO link tail holds payload 20..23 (28 bytes on the wire) and v2.2's
# sync_gaps sits behind it at 24..25 (30 bytes): main moved the field it had never shipped rather
# than the fleet reflashing six sealed boxes (nowde#t-047).
h5 = nx.hello('2.0.5', 1000, 1, 0, 3, 2, False, (True, 0x4000, 200))
check(len(h5) + 2 == 28, f"HELLO 2.0.5 length {len(h5) + 2}")
check(h5[2 + 20:] == [1, 0x7F, 0x7F, 127], f"HELLO 2.0.5 tail bytes {h5[22:]}")
p = nx.parse(h5)[1]
check(p['link_lost'] is True and p['ms_since_sync'] == 0x3FFF and p['watchdog_restarts'] == 127
      and 'sync_gaps' not in p, f"HELLO 2.0.5 link trailer {p}")
# Bytes exactly as 2.0.5 firmware emits them (sendHello, garden-2.0.5@0196e2d): linkLost 0,
# ms since sync 1234 = 0x09 0x52, 3 watchdog restarts. A v2.2-era parser read 20..21 as syncGaps.
p = nx.parse(nx.hello('2.0.5', 1000, 1, 0, 3, 2, False)[:] + [0, 0x09, 0x52, 3])[1]
check(p['link_lost'] is False and p['ms_since_sync'] == 1234 and p['watchdog_restarts'] == 3
      and 'sync_gaps' not in p, f"HELLO 2.0.5 field frame {p}")
check(len(hg) + 2 == 30 and hg[2 + 24:] == [640 >> 7, 640 & 0x7F], f"HELLO v2.2 sync_gaps at 24..25 {hg[26:]}")
check(nx.parse(hg)[1]['ms_since_sync'] == 120, f"HELLO v2.2 keeps the 2.0.5 tail {nx.parse(hg)[1]}")

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
hb = nx.hello('2.1.0', 1000, 1, 1, 2, 2, False, (False, 0, 0), 0, bid)
check(len(hb) + 2 == 40, f"HELLO v2.1 length ({len(hb) + 2})")
p = nx.parse(hb)[1]
check(p['build_id'] == bid and p['sync_gaps'] == 0 and p['lr'] is False, f"HELLO build_id {p}")
check('build_id' not in nx.parse(nx.hello('2.2.0', 1000, 1, 0, 3, 2, True, (False, 0, 0), 640))[1], "HELLO v2.2 unchanged")
# 2.4: gen + node id behind the build id, raw ASCII (already 7-bit). Two-sided again.
h24 = nx.hello('2.4-rc1', 1000, 1, 1, 2, 2, False, (False, 0, 0), 0, bid, gen=2, node_id='cd83fc')
check(len(h24) + 2 == 47 and all(b < 0x80 for b in h24), f"HELLO 2.4 length ({len(h24) + 2})")
p = nx.parse(h24)[1]
check(p['gen'] == 2 and p['node_id'] == 'CD83FC' and p['build_id'] == bid and p['version'] == '2.4-rc1',
      f"HELLO 2.4 gen + node_id {p}")
check('gen' not in nx.parse(hb)[1], "HELLO v2.1 has no gen")
# 2.4 LR: HELLO lr is the mode (old readers' nonzero = on still holds), and rssi follows node id.
h4r = nx.hello('2.4-rc1', 1000, 1, 0, 2, 2, 2, (False, 0, 0), 0, bid, gen=2, node_id='26B8F8', rssi=-67)
p = nx.parse(h4r)[1]
check(len(h4r) + 2 == 48 and p['lr'] is True and p['lr_mode'] == '500k' and p['rssi'] == -67,
      f"HELLO 2.4 lr 500k + rssi {p}")
check(nx.parse(nx.hello('2.4-rc1', 1000, 1, 1, 2, 2, 0, (False, 0, 0), 0, bid, gen=2, node_id='CD83FC', rssi=0))[1]['rssi'] is None,
      "HELLO rssi 0 = unknown")
check(nx.set_lr('250k') == [0x7D, 0x0B, 1] and nx.set_lr('500k')[2] == 2 and nx.set_lr(0)[2] == 0, "SET_LR modes")
sn = nx.set_neighbours(['AA:BB:CC:DD:EE:01', bytes([0x80, 1, 2, 3, 4, 5])])
check(sn[:2] == [0x7D, 0x70] and nx.decode7(sn[2:])[:12] == [0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 1, 0x80, 1, 2, 3, 4, 5]
      and all(b < 0x80 for b in sn) and nx.set_neighbours() == [0x7D, 0x70], "SET_NEIGHBOURS packs MACs 7-bit")
rr = nx.running_state_chunks([{'mac': [1, 2, 3, 4, 5, 6], 'layer': 'main', 'version': '2.4-rc1',
                               'last_seen_ms': 10, 'index': 3, 'sync_gaps': 7, 'rssi': -71}], 5000, True)[0]
q = nx.parse(rr)[1]['receivers'][0]
check(q['rssi'] == -71 and q['sync_gaps'] == 7 and q['version'] == '2.4-rc1', f"RUNNING_STATE 2.4 record rssi {q}")

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

# ============================================================================================
# v2.3 — `0x04 MIDI_EVENT`, the frame docs/PROTOCOL.md freezes before any firmware exists.
# There is nothing to interoperate with yet, so these rows are not a compat check: they are the
# record of a layout decision, in the one place that fails when somebody edits it by accident.
# ============================================================================================

ev = [nm.midi_record(0x90, 60, 100, ev_seq=1, offset=0),
      nm.midi_record(0xB1, 7, 64, ev_seq=2, offset=2),
      nm.midi_record(0xC2, 5, ev_seq=3, offset=4),          # PC: one data byte, data2 = 0
      nm.midi_record(0xE3, 0, 64, ev_seq=4, offset=4)]      # pitch bend: the 0xEn upper bound
# Every multi-byte value here is deliberately byte-ASYMMETRIC — a mask of 0xFFFF or a seq of
# 0x0101 would round-trip through a big-endian encoder without complaint.
f = nm.midi_event_frame('hplayer2', ev, fire_at=0xDEADBEEF, seq=0x1234, group_mask=0x8001)

# Field OFFSETS, not just a round trip: the struct is packed and little-endian, and a field
# that moved would round-trip through this module perfectly while breaking every node.
check(len(f) == 30 + 4 * 7, f"MIDI_EVENT length {len(f)}")
check(f[0] == 0x04 and f[1] == 30 and f[2] == 0 and f[3] == 7, "MIDI_EVENT header preamble")
check(f[4:6] == [0x01, 0x80], f"groupMask at 4, little-endian ({f[4:6]})")
check(f[6:8] == [0x34, 0x12], f"seq at 6, little-endian ({f[6:8]})")
check(f[8:12] == [0xEF, 0xBE, 0xAD, 0xDE], f"fireAt at 8, little-endian u32 ({f[8:12]})")
check(f[12] == 4 and f[13] == 0, "newCount at 12, carryCount at 13")
check(bytes(f[14:30]) == b'hplayer2' + b'\x00' * 8, "layer[16] at 14, NUL-padded")
check(f[30:37] == [0x90, 60, 100, 1, 0, 0, 0], "record 0 at hdrLen, evSeq then offset")

p = nm.parse_midi_event(f)
check(p['layer'] == 'hplayer2' and p['seq'] == 0x1234 and p['fire_at'] == 0xDEADBEEF
      and p['group_mask'] == 0x8001 and not p['snapshot'], f"MIDI_EVENT header parse {p}")
check([r['status'] for r in p['new']] == [0x90, 0xB1, 0xC2, 0xE3] and p['carried'] == [],
      "MIDI_EVENT record parse")
# Both ends of the channel-voice range, and the data2 = 0 convention for a 0xCn.
check(p['new'][2]['data2'] == 0 and all(r['voice'] for r in p['new']),
      f"0x80..0xEF all read as voice {[r['voice'] for r in p['new']]}")

# Carried records are the same events with a NEGATIVE offset, and nothing but the two counts
# says where they start. Pin the boundary and the sign: i16 two's complement in a packed struct
# is exactly the sort of field that silently reads as 65 531.
carry = [nm.midi_record(0x90, 60, 100, ev_seq=1, offset=-5)]
fc = nm.midi_event_frame('*', [nm.midi_record(0x80, 60, 0, ev_seq=4, offset=0)], carry,
                         fire_at=1000, seq=2)
pc = nm.parse_midi_event(fc)
check(len(pc['new']) == 1 and len(pc['carried']) == 1, "new/carry boundary is the counts")
check(pc['carried'][0]['offset'] == -5 and pc['carried'][0]['ev_seq'] == 1,
      f"carried offset is signed {pc['carried'][0]}")
check(pc['new'][0]['voice'], "0x80 is the bottom of the channel-voice range")
check(pc['layer'] == '*', "wildcard layer")

# Data bytes are 7-bit on this wire as on every other MIDI one; a caller handing over 0xFF is
# clamped at the encoder, not passed through to bite a host.
clamp = nm.parse_midi_event(nm.midi_event_frame('x', [nm.midi_record(0xB0, 0xFF, 0xFF)]))
check(clamp['new'][0]['data1'] == 0x7F and clamp['new'][0]['data2'] == 0x7F,
      f"data bytes masked to 7 bits {clamp['new'][0]}")

# The budget, closed both ways. A full frame must survive the `0x05` relay envelope #t-024 has
# not built yet, so 250 B is not the ceiling — 240 is.
full = nm.midi_event_frame('sixteen-chars-ok',
                           [nm.midi_record(0x90, 60, 100, ev_seq=i)
                            for i in range(nm.MIDI_EVENT_MAX_NEW)],
                           [nm.midi_record(0x80, 60, 0, ev_seq=i, offset=-5)
                            for i in range(nm.MIDI_EVENT_MAX_NEW)])
check(len(full) == nm.MIDI_EVENT_FRAME_MAX == 226, f"full frame is 226 B ({len(full)})")
check(len(full) + nm.MESH_RELAY_LEN <= nm.ESPNOW_MTU,
      f"full frame survives the relay envelope ({len(full) + nm.MESH_RELAY_LEN} B)")
check(nm.parse_midi_event(full + [0] * nm.MESH_RELAY_LEN) is not None
      and len(nm.parse_midi_event(full)['carried']) == nm.MIDI_EVENT_MAX_NEW,
      "full frame parses, 14 + 14")
# The two claims sub-step (1) of #t-036 had to reconcile: the design note's "<= 40 events per
# frame" predates both the per-event size and the flooding envelope, and does not fit. Asserted
# as a NON-fit on purpose — the day #t-024 widens the envelope, this is what says so.
check(30 + 40 * 7 + nm.MESH_RELAY_LEN > nm.ESPNOW_MTU, "the design note's 40 events do not fit")
check(30 + 2 * 15 * 7 + nm.MESH_RELAY_LEN == nm.ESPNOW_MTU,
      "15 new would land on the MTU exactly, with no margin — hence 14")

# The one rule that keeps the frame extensible under the charter: a receiver finds record 0 at
# the frame's own hdrLen and strides by its own recLen. Synthesise the frames a later
# generation would send — two extra header bytes, one extra record byte — and check this parser
# still reads what it knows. Without these rows the rule is prose, and prose does not fail.
grown_hdr = f[:1] + [32] + f[2:30] + [0xAA, 0xBB] + f[30:]
pg = nm.parse_midi_event(grown_hdr)
check(pg is not None and [r['ev_seq'] for r in pg['new']] == [1, 2, 3, 4],
      f"records found at a grown hdrLen {pg}")
grown_rec = f[:3] + [8] + f[4:30]
for i in range(4):
    grown_rec += f[30 + i * 7:37 + i * 7] + [0xCC]
pr = nm.parse_midi_event(grown_rec)
check(pr is not None and [r['ev_seq'] for r in pr['new']] == [1, 2, 3, 4],
      f"records stride by a grown recLen {pr}")

# Reject only too-short, exactly like the SysEx parsers (charter §(b)).
# f[:10] is the case only the header floor catches — shorter than the count bytes at 12/13, so
# without that first guard the parser indexes off the end instead of returning None.
check(nm.parse_midi_event(f[:10]) is None, "frame shorter than the header rejected")
check(nm.parse_midi_event(f[:29]) is None, "short header rejected")
check(nm.parse_midi_event(f[:-1]) is None, "truncated record rejected")
check(nm.parse_midi_event(f + [0x99] * 20) is not None, "trailing bytes accepted and ignored")
check(nm.parse_midi_event(f[:1] + [29] + f[2:]) is None, "hdrLen below the v2.3 floor rejected")
check(nm.parse_midi_event(f[:3] + [6] + f[4:]) is None, "recLen below the v2.3 floor rejected")

# SNAPSHOT, and the reserved flag bits an old node must ignore rather than drop on.
snap = nm.midi_event_frame('main', [nm.midi_record(0xB0, 7, 100, ev_seq=9)], snapshot=True)
check(nm.parse_midi_event(snap)['snapshot'], "SNAPSHOT flag")
check(nm.parse_midi_event(snap[:2] + [0x81] + snap[3:])['snapshot'],
      "reserved flag bits ignored, not fatal")
# A non-channel-voice status is skipped by the receiver, never fatal to the frame.
bad = f[:30] + nm.midi_record(0xF8, 0, 0, ev_seq=7) + f[37:]
pb = nm.parse_midi_event(bad)
check(pb is not None and [r['voice'] for r in pb['new']] == [False, True, True, True],
      f"clock byte flagged non-voice, frame still read {pb}")

# The firmware states the same frame sizes (src/nowde_config.h); its relay sizes the 0x05 forward
# buffer from them (#t-037 slice 1), so a drift between doc, codec and C is a truncated frame.
import re  # noqa: E402
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src', 'nowde_config.h')) as fh:
    cfg = fh.read()
for name in ('HEADER_LEN', 'RECORD_LEN', 'MAX_NEW', 'CARRY_DEPTH', 'MAX_STALE_MS'):
    m = re.search(rf'^#define MIDI_EVENT_{name}\s+(\d+)\b', cfg, re.M)
    check(m and int(m.group(1)) == getattr(nm, 'MIDI_EVENT_' + name),
          f"nowde_config.h MIDI_EVENT_{name} matches the codec")
check(nm.MIDI_EVENT_FRAME_MAX == 226, "a full 0x04 frame is 226 B")
check(nm.MESH_RELAY_LEN + nm.MIDI_EVENT_FRAME_MAX <= 250, "a full 0x04 frame fits the 0x05 envelope")
# The relay keys dedup on the frame's own seq: offset 6, LE, independent of MediaSync's seq.
rs = nm.midi_event_frame('main', [nm.midi_record(0x90, 60, 100, ev_seq=1)], seq=0xBEEF)
check(rs[6:8] == [0xEF, 0xBE] and nm.parse_midi_event(rs)['seq'] == 0xBEEF, "0x04 seq at offset 6")
full = nm.midi_event_frame('main', [nm.midi_record(0x90, 60, 100, ev_seq=i) for i in range(14)],
                           [nm.midi_record(0x90, 61, 100, ev_seq=i) for i in range(14)])
check(len(full) == nm.MIDI_EVENT_FRAME_MAX and nm.parse_midi_event(full) is not None,
      f"14 new + 14 carried is exactly the max frame ({len(full)} B)")

print("nowde_mesh selftest OK")

# ============================================================================================
# 2.4 (#t-049) — tools/nowde-bench.py, the desk's acceptance. Its hardware half needs ports; its
# judging half is pure and is what these rows pin: a parser that reads a timecode, an event or a
# HELLO wrong turns a good desk run into a FAIL (or a bad one into a PASS). mido is imported only
# when a port opens, so this runs where verify runs, with no MIDI stack installed.
# ============================================================================================
import importlib.util  # noqa: E402
_spec = importlib.util.spec_from_file_location(
    'nowde_bench', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nowde-bench.py'))
nb = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(nb)
check('mido' not in sys.modules, "nowde-bench imports mido only when a port opens")

# MTC: 01:02:03:15 at 30 fps, as eight quarter-frame nibbles (rate 3 in qf7 bits 1-2) and as a
# full frame (rate in hh bits 5-6).
qf = [15 & 0xF, 15 >> 4, 3 & 0xF, 3 >> 4, 2 & 0xF, 2 >> 4, 1 & 0xF, (1 >> 4) | (3 << 1)]
pos, fps = nb.mtc_qf_ms(qf)
check(fps == 30.0 and pos == 3723000 + 500, f"quarter frames 01:02:03:15 @30 -> {pos} ms @ {fps}")
check(nb.mtc_qf_ms(qf[:7] + [1 << 1])[1] == 25.0, "quarter frame rate bits 1 = 25 fps")
check(nb.mtc_full_ms([0x7F, 0x7F, 1, 1, 1 | (3 << 5), 2, 3, 15]) == 3723500, "MTC full frame")
check(nb.mtc_full_ms([0x7F, 0x7F, 1, 2, 0, 0, 0, 0]) is None, "a non-full-frame SysEx is not a timecode")

st = nb.stats([1, -2, 3, -4, 10])
check(st == {'n': 5, 'mean': 1.6, 'p95': 10.0, 'max': 10.0}, f"stats: signed mean, |x| p95 / max {st}")
check(nb.stats([])['n'] == 0, "stats of nothing")

script = nb.show_script(60, 60)
check(nb.show_position(script, 10) == (True, 10000.0), "show: 10 s in plays at 10 000 ms")
check(nb.show_position(script, 31) == (True, 121000.0), "show: the seek at 30 s jumps to 120 000 ms")
check(nb.show_position(script, 61) == (False, 0.0) and nb.show_position(script, 200) == (False, 0.0),
      "show: stopped after 60 s and after the end")
# Offsets skip the 2 s after a start or a seek; an exact slave reads 0.
t0 = 1000.0
mtc = [(t0 + t, nb.show_position(script, t)[1] + 5) for t in (0.5, 10, 30.5, 45, 70)]
check(nb.mtc_offsets(script, t0, mtc) == [5.0, 5.0], f"MTC offsets: settle windows and the idle skipped")

plan = nb.event_plan()
keys = [nb.event_key(*e) for e in plan]
check(len(plan) == 128 + 26 and len(set(keys)) == len(keys), "0x04 plan: every event unique")
check(all(not (e[0] == 'control_change' and e[2] == 100) for e in plan), "0x04 plan never uses CC#100")
check(nb.event_key('note_on', 0, 60, 0) == nb.event_key('note_off', 0, 60, 0), "note-on velocity 0 = note-off")
caps_a = [(1.000 + i * 0.01, k) for i, k in enumerate(keys)]
caps_b = [(1.002 + i * 0.01, k) for i, k in enumerate(keys) if i != 3] + [(9.0, keys[7])]
per, spread = nb.ledger(plan, {'a': caps_a, 'b': caps_b})
check(per['a'] == {'sent': 154, 'received': 154, 'lost': 0, 'duplicated': 0}, f"ledger a {per['a']}")
check(per['b'] == {'sent': 154, 'received': 153, 'lost': 1, 'duplicated': 1}, f"ledger b {per['b']}")
check(spread['n'] == 152 and spread['max'] == 2.0, f"fire spread: events both got once ({spread})")

ta = [(10.0 + i / 30.0, i * 1000 / 30.0 + 4) for i in range(30)]
tc = [(10.0 + i / 30.0 + 0.003, i * 1000 / 30.0 + 3) for i in range(30)]
po = nb.pair_offsets(ta, tc)
check(len(po) == 30 and all(abs(x - 4.0) < 1e-6 for x in po), f"two-slave MTC pairing {po[:3]}")

h = nx.parse(nx.hello('2.4-rc1', role=0, board=2, sync_quality=2, lr=2, link=(False, 5, 0),
                      sync_gaps=3, build_id='0123456789abcdef', gen=2, node_id='cd83fc', rssi=-61))[1]
row = nb.hello_row('Nowde - CD83FC', h)
check(row == {'port': 'Nowde - CD83FC', 'version': '2.4-rc1', 'build_id': '0123456789abcdef', 'gen': 2,
              'role': 'slave', 'board': 'atoms3', 'lr': '500k', 'node_id': 'CD83FC', 'sync': 2,
              'gaps': 3, 'rssi': -61}, f"nodes row from a 2.4 HELLO {row}")
check(nb.hello_row('x', None)['gen'] is None, "a silent node is a row of blanks")

rs = nx.parse(nx.running_state_chunks([{'mac': [1, 2, 3, 0xCD, 0x83, 0xFC], 'sync_gaps': 9, 'rssi': -70}])[0])[1]
wr = nb.walk_rows(0, 12.04, rs, '50 m')
check(len(wr) == 1 and wr[0][1:] == ['12.0', '01:02:03:CD:83:FC', 1, 9, -70, 0, '50 m'] and len(wr[0]) == len(nb.WALK_HEADER),
      f"walk row {wr}")
check(nb.walk_rows(0, 1, None)[0][2:7] == ['', '', '', '', ''], "walk: nobody heard is one empty row")
check(nb.heard_in(rs, 'CD83FC') and not nb.heard_in(rs, '26B8F8'), "a node is found in a table by its id")

lines, ok = nb.summary({'command': 'x', 'checks': [{'name': 'a', 'pass': True}, {'name': 'b', 'pass': None}]})
check(ok and lines[1].strip().startswith('INFO'), "summary: INFO never fails a run")
lines, ok = nb.summary({'command': 'x', 'checks': [{'name': 'a', 'pass': True}, {'name': 'b', 'pass': False}]})
check(not ok and lines[-1].startswith('FAIL'), "summary: one FAIL fails the run")
for c in ('nodes', 'accept', 'drive', 'listen', 'lr', 'relay', 'ota', 'lr-set', 'walk'):
    check(f'nowde-bench {c} ' in nb.__doc__, f"nowde-bench documents `{c}`")

# 25/11 rig run (#t-049 ⑥): the three defects the first hardware run found, pinned where pure.
# lr-set: a HELLO counts only if the node booted after SET_LR (its own pre-restart HELLO already
# carries the new mode).
h_old = nx.parse(nx.hello('2.4.0', uptime_ms=600000, role=0, board=2, sync_quality=0, lr=1))[1]
h_new = nx.parse(nx.hello('2.4.0', uptime_ms=2500, role=0, board=2, sync_quality=0, lr=1))[1]
check(not nb.booted_since(h_old, 100.0, 104.0), "lr-set: a HELLO from before the restart is not a read-back")
check(nb.booted_since(h_new, 100.0, 104.0), "lr-set: a HELLO 2.5 s old, 4 s after SET_LR, is")
check(not nb.booted_since(None, 100.0, 104.0), "lr-set: no HELLO proves nothing")


class FakeNode:
    """The node's side of SET_ROLE / SET_LOCAL_LAYER: each answers CONFIG_STATE with its layer."""

    def __init__(self, pattern, layer):
        self.pattern, self.name, self.layer, self.sent, self.out = pattern, 'Nowde - ' + pattern, layer, [], []

    def mark(self):
        return len(self.out)

    def send(self, payload):
        self.sent.append(payload)
        if payload[1] == nx.CMD_SET_LOCAL_LAYER:
            self.layer = bytes(payload[2:]).decode('ascii')
        if payload[1] in (nx.CMD_SET_ROLE, nx.CMD_SET_LOCAL_LAYER):
            self.out.append(nx.config_state(role=1, board=2, layer=self.layer))

    def wait_for(self, cmd, timeout=2.0, mark=None):
        return next((nx.parse(m)[1] for m in self.out[mark or 0:] if m[1] == cmd), None)


# listen / drive / accept: every node of a show goes on the show's layer, and back after.
fn, fw = FakeNode('CD83FC', '-'), FakeNode('26B8F8', 'garden')
rep = {'checks': []}
nb.take_all(rep, [(fn, 'slave'), (fw, 'master')])
check(fn.layer == nb.SHOW_LAYER and fw.layer == nb.SHOW_LAYER and all(c['pass'] for c in rep['checks']),
      f"take: both nodes on {nb.SHOW_LAYER} ({rep['checks']})")
check("was '-'" in rep['checks'][0]['detail'], "take: the layer before is reported")
nb.take(fn, 'slave')   # a second take (lr re-opens ports) must not forget the first layer
nb._restore([fn, fw])
check(fn.layer == '-' and fw.layer == 'garden' and not nb._layers_before, "restore: each node back on its own layer")
check(nb.WALK_LAYER == '*', "walk streams on the wildcard: the far node is out of the tool's reach")



class FakeSender(FakeNode):
    """A sender under a host that polls: QUERY_RUNNING_STATE gets the table, QUERY_CONFIG HELLO."""

    def send(self, payload):
        self.sent.append(payload)
        if payload[1] == nx.CMD_QUERY_RUNNING_STATE:
            self.out += nx.running_state_chunks([])
        elif payload[1] == nx.CMD_QUERY_CONFIG:
            self.out += [nx.hello('2.4.0', role=1, board=2, sync_quality=2), nx.config_state(role=1, board=2, layer='-')]


fs = FakeSender('26B8F8', '-')
fs.info = {}
hs = nb.Port.hello(fs)
check(hs and hs['version'] == '2.4.0' and [m[1] for m in fs.sent] == [nx.CMD_QUERY_RUNNING_STATE, nx.CMD_QUERY_CONFIG],
      "hello: a sender's table first, then QUERY_CONFIG (no side effect on a sender) for HELLO")
fq = FakeNode('CD83FC', '-')
fq.info = {}
check(nb.Port.hello(fq) is None and [m[1] for m in fq.sent] == [nx.CMD_QUERY_RUNNING_STATE],
      "hello: a silent node never gets QUERY_CONFIG (it would turn a legacy node into a sender)")

# exit codes: 2 = could not run (no MIDI stack, no port), never 1 (= a node FAILed).
_mido_real = nb._mido


def _no_mido():
    raise nb.CouldNotRun("no MIDI stack")


nb._mido = _no_mido
_rep = os.path.join(os.environ.get('TMPDIR', '/tmp'), 'nowde-bench-selftest.json')
check(nb.main(['--report', _rep, 'nodes']) == 2, "no MIDI stack = exit 2, not 1")
nb._mido = _mido_real

print("nowde-bench selftest OK")

# ============================================================================================
# #t-038 — the slave's schedule (docs/PROTOCOL.md "The schedule"). Its logic is a header with no
# Arduino in it, src/midi_schedule.h, so the host runs the very code the firmware builds:
# tools/midi_schedule_test.cpp, compiled here when a C++ compiler is on PATH. Without one the
# rows below still pin the depth, and the run says plainly that the schedule went unchecked.
# ============================================================================================
import shutil  # noqa: E402
import subprocess  # noqa: E402
import tempfile  # noqa: E402


def cfg_int(name):
    m = re.search(rf'^\s*#define {name}\s+(\d+)\b', cfg, re.M)
    check(m, f"nowde_config.h defines {name}")
    return int(m.group(1))


def cfg_int_hex(name):
    m = re.search(rf'^\s*#define {name}\s+(0x[0-9A-Fa-f]+|\d+)\b', cfg, re.M)
    check(m, f"nowde_config.h defines {name}")
    return int(m.group(1), 0)


# 2.4: what the HELLO trailer, the version field and ESP-NOW's peer table hold, read from the C.
check(cfg_int('NOWDE_HELLO_GEN') == 3 and cfg_int('NOWDE_NODE_ID_LEN') == nx.NODE_ID_LEN,
      "nowde_config.h HELLO gen (3 = Nowde 3) + node id match the codec")
mv = re.search(r'^#define NOWDE_VERSION "([^"]*)"', cfg, re.M)
check(mv and len(mv.group(1)) <= 7, f"NOWDE_VERSION fits the 8-byte NUL-terminated wire field ({mv and mv.group(1)})")
check(cfg_int('MAX_SENDERS') + cfg_int('MAX_RECEIVERS') + 1 <= 20,
      "senders + receivers + the broadcast peer fit ESP-NOW's 20 unencrypted peers")

# The depth is derived from the lead, the batch window and the frame cap — and #t-038's figure,
# 266, is what that derivation gives for v2.3's frozen constants. A change to any of them moves
# the depth with it; a change to the formula is what this row is for.
m = re.search(r'^#define MIDI_EVENT_SCHED_DEPTH\s+(.+)$', cfg, re.M)
check(m and m.group(1).replace(' ', '')
      == '((MIDI_EVENT_LEAD_MS/MIDI_EVENT_BATCH_MS+1)*MIDI_EVENT_MAX_NEW)',
      "MIDI_EVENT_SCHED_DEPTH is (lead / batch + 1) x max new")
lead = cfg_int('MAX_HOPS') * cfg_int('MESH_RELAY_DELAY_MAX_MS')
check(lead == 90, f"the lead the schedule is sized on is 90 ms ({lead})")
depth = (lead // cfg_int('MIDI_EVENT_BATCH_MS') + 1) * nm.MIDI_EVENT_MAX_NEW
check(depth == 266, f"schedule depth is 266 records ({depth})")

# Nowde 3 (#t-050 ③): SET_LED / IDENTIFY, the codec against the firmware's ids and its own parse.
check(cfg_int_hex('SYSEX_CMD_SET_LED') == nx.CMD_SET_LED and cfg_int_hex('SYSEX_CMD_IDENTIFY') == nx.CMD_IDENTIFY,
      "nowde_config.h SET_LED / IDENTIFY ids match the codec")
check(cfg_int_hex('NOWDE_LED_PIN_NONE') == nx.LED_PIN_NONE, "nowde_config.h NOWDE_LED_PIN_NONE matches the codec")
check(nx.set_led(48, 16, False) == [0x7D, 0x13, 48, 16, 0] and nx.identify() == [0x7D, 0x14],
      "SET_LED / IDENTIFY frames")
check(nx.parse(nx.set_led(None, 200, True))
      == ('SET_LED', {'pin': None, 'brightness': 127, 'dark': True}), "SET_LED parses back, clamped")
# (④) CLAIM and the standby role value, against the firmware's.
check(cfg_int_hex('SYSEX_CMD_CLAIM') == nx.CMD_CLAIM and nx.claim() == [0x7D, 0x0F],
      "nowde_config.h CLAIM id matches the codec")
check(cfg_int('NOWDE_ROLE_STANDBY') == 3 and nx.ROLE_NAMES[3] == 'standby',
      "HELLO role 3 = standby in the firmware and the codec")

here = os.path.dirname(os.path.abspath(__file__))
cxx = shutil.which('c++') or shutil.which('g++') or shutil.which('clang++')
# Nowde 3 (#t-050 ①): the one SysEx -> USB-MIDI packetizer, src/usb_midi_packets.h, against the
# hand-rolled builders it replaced -- tools/usb_midi_packets_test.cpp, the same way; and the receive
# parse, src/midi_rx.h, against the inline one it replaced -- tools/midi_rx_test.cpp; and (②) the
# serial link's wire, node bytes back through the stream parser -- tools/serial_link_test.cpp; and
# (③) the LED + button vocabulary, src/led_pattern.h -- tools/led_pattern_test.cpp; and (④) the
# election, src/election.{h,cpp} -- tools/election_test.cpp, built with the .cpp it tests.
# #t-053: src/air_sysex.h, the rule for raw SysEx over the air, is the same kind of header and
# runs the same way: tools/air_sysex_test.cpp.
host_sources = {'election_test': ['election.cpp']}
for host_test in ('midi_schedule_test', 'usb_midi_packets_test', 'midi_rx_test', 'serial_link_test',
                  'led_pattern_test', 'election_test', 'air_sysex_test'):
    if not cxx:
        print(f"{host_test} SKIPPED: no host C++ compiler on PATH")
        continue
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, host_test)
        build = subprocess.run([cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-o', exe,
                                os.path.join(here, host_test + '.cpp')]
                               + [os.path.join(here, '..', 'src', f) for f in host_sources.get(host_test, [])],
                               capture_output=True, text=True)
        check(build.returncode == 0, f"{host_test} builds:\n{build.stderr}")
        run = subprocess.run([exe], capture_output=True, text=True)
        check(run.returncode == 0, f"{host_test}:\n{run.stdout}{run.stderr}")
        print(run.stdout.strip())

# The firmware calls that header, and nothing else decides: the recv callback asks it before any
# over-the-air SysEx runs.
handlers = open(os.path.join(here, '..', 'src', 'esp_now_handlers.cpp')).read()
check('airSysExVerdict(layerCommand, node, from)' in handlers and 'if (!airSysExRuns(verdict))' in handlers,
      "esp_now_handlers.cpp runs raw SysEx from the air only on src/air_sysex.h's verdict")
