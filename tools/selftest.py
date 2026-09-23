"""Dependency-free checks of nowde_sysex / nowde_mesh against the byte layouts in
docs/PROTOCOL.md."""
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

print("nowde_mesh selftest OK")
