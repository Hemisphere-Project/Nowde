"""ESP-NOW mesh frame helpers — byte-exact with the structs in src/nowde_config.h and the
layouts in docs/PROTOCOL.md.

The sibling of nowde_sysex.py, for the other wire: nowde_sysex speaks the 7-bit SysEx the host
exchanges with a node over USB-MIDI, this speaks the packed little-endian structs nodes
exchange over the air. Dependency-free on purpose (tools/selftest.py imports both).

Today it covers `0x04 MIDI_EVENT` only, and that frame has no firmware behind it yet (#t-037
builds it). The reason it is here first is the charter, docs/PROTOCOL.md §(b): `0x04` freezes
the moment one fielded node parses it, so the layout is fixed on paper and the arithmetic that
makes it fit 250 bytes is checked by a gate rather than remembered.
"""

# ---- wire constants ---------------------------------------------------------------

MSG_SENDER_BEACON = 0x01
MSG_RECEIVER_INFO = 0x02
MSG_MEDIA_SYNC = 0x03
MSG_MIDI_EVENT = 0x04
MSG_MESH_RELAY = 0x05

MAX_LAYER_LENGTH = 16

# `0x04 MIDI_EVENT` — see docs/PROTOCOL.md "The v2.3 event frame".
MIDI_EVENT_HEADER_LEN = 30      # type..layer[16]; a receiver reads hdrLen, never this
MIDI_EVENT_RECORD_LEN = 7       # status data1 data2 evSeq(2) offset(2); likewise recLen
MIDI_EVENT_BATCH_MS = 5
MIDI_EVENT_CARRY_DEPTH = 1      # frames repeated; only the previous frame's NEW records
MIDI_EVENT_MAX_NEW = 14
MIDI_EVENT_MAX_RECORDS = MIDI_EVENT_MAX_NEW * (1 + MIDI_EVENT_CARRY_DEPTH)
MIDI_EVENT_FRAME_MAX = MIDI_EVENT_HEADER_LEN + MIDI_EVENT_MAX_RECORDS * MIDI_EVENT_RECORD_LEN

MIDI_EVENT_FLAG_SNAPSHOT = 0x01
MIDI_EVENT_MAX_STALE_MS = 150   # slave-side policy, not a wire field (#t-039 may revise)

# What the frame has to leave room for: the controlled-flooding envelope reserved for #t-024,
# `{ type, origin[6], seq u16, hop u8 }` wrapped around a packet verbatim. A `0x04` frame that
# does not survive it cannot cross a hop, and crossing a hop is the whole point.
ESPNOW_MTU = 250
MESH_RELAY_LEN = 10


# ---- little-endian scalars (the structs are packed and native-endian) --------------

def u16le(v):
    return [v & 0xFF, (v >> 8) & 0xFF]


def u32le(v):
    return [v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF]


def i16le(v):
    return u16le(int(v) & 0xFFFF)


def from_u16le(b):
    return b[0] | (b[1] << 8)


def from_u32le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)


def from_i16le(b):
    v = from_u16le(b)
    return v - 0x10000 if v & 0x8000 else v


def layer16(name):
    """NUL-padded 16 bytes, exactly as MediaSyncPacket and ReceiverInfo carry it."""
    return list(str(name).encode('ascii')[:MAX_LAYER_LENGTH].ljust(MAX_LAYER_LENGTH, b'\x00'))


# ---- 0x04 MIDI_EVENT --------------------------------------------------------------

def midi_record(status, data1, data2=0, ev_seq=0, offset=0):
    """One 7-byte record. `status` is a channel-voice status byte (0x80..0xEF); data2 is 0 for
    the one-data-byte statuses 0xCn / 0xDn. `offset` is signed ms from the frame's fireAt, so a
    carried record is negative."""
    return ([status & 0xFF, data1 & 0x7F, data2 & 0x7F] + u16le(ev_seq & 0xFFFF)
            + i16le(offset))


def midi_event_frame(layer, new=(), carried=(), fire_at=0, seq=0, group_mask=0xFFFF,
                     snapshot=False):
    """Build a `0x04` frame. `new` and `carried` are lists of midi_record() outputs; the new
    ones go first, the carried ones after, which is the order a receiver relies on to tell them
    apart (only the counts say where the boundary is)."""
    flags = MIDI_EVENT_FLAG_SNAPSHOT if snapshot else 0
    out = ([MSG_MIDI_EVENT, MIDI_EVENT_HEADER_LEN, flags, MIDI_EVENT_RECORD_LEN]
           + u16le(group_mask) + u16le(seq) + u32le(fire_at)
           + [len(new) & 0xFF, len(carried) & 0xFF] + layer16(layer))
    for r in list(new) + list(carried):
        out += list(r)
    return out


def parse_midi_event(data):
    """Decode a `0x04` frame, or return None if a receiver would drop it.

    Written the way the firmware must be written, which is the point of having it here: record 0
    is located at the frame's own `hdrLen` and records stride by its own `recLen` — never by the
    constants above. That is what lets a later generation append header or record fields (§(b)
    forbids widening one in place) without this parser, or a fielded v2.3 node, losing the
    fields it already knows. A frame longer than its own arithmetic says is accepted with the
    tail ignored; only a short one is rejected."""
    d = list(data)
    if len(d) < MIDI_EVENT_HEADER_LEN or d[0] != MSG_MIDI_EVENT:
        return None
    hdr_len, flags, rec_len = d[1], d[2], d[3]
    if hdr_len < MIDI_EVENT_HEADER_LEN or rec_len < MIDI_EVENT_RECORD_LEN:
        return None
    new_count, carry_count = d[12], d[13]
    total = new_count + carry_count
    if len(d) < hdr_len + total * rec_len:
        return None

    def record(i):
        r = d[hdr_len + i * rec_len:hdr_len + (i + 1) * rec_len]
        return {'status': r[0], 'data1': r[1], 'data2': r[2],
                'ev_seq': from_u16le(r[3:5]), 'offset': from_i16le(r[5:7]),
                # A status outside the channel-voice range is skipped by the receiver, not a
                # reason to drop the frame — flagged here rather than filtered, so a caller can
                # see what arrived.
                'voice': 0x80 <= r[0] <= 0xEF}

    return {
        'group_mask': from_u16le(d[4:6]),
        'seq': from_u16le(d[6:8]),
        'fire_at': from_u32le(d[8:12]),
        'layer': bytes(d[14:30]).decode('ascii', 'ignore').rstrip('\x00'),
        'snapshot': bool(flags & MIDI_EVENT_FLAG_SNAPSHOT),
        'new': [record(i) for i in range(new_count)],
        'carried': [record(new_count + i) for i in range(carry_count)],
    }
