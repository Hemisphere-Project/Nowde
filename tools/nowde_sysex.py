"""Nowde SysEx (0x7D) wire helpers — byte-exact with the firmware (src/sysex.cpp) and the
MillluBridge Bridge. Pure Python, no dependencies. Shared by nowde-cli and nowde-sim.

Frames here are the bytes BETWEEN F0 and F7 (what mido calls `data`)."""

MANUFACTURER = 0x7D

CMD_QUERY_CONFIG = 0x01
CMD_PUSH_FULL_CONFIG = 0x02
CMD_QUERY_RUNNING_STATE = 0x03
CMD_OTA_BEGIN = 0x05
CMD_OTA_DATA = 0x06
CMD_OTA_END = 0x07
CMD_SET_ROLE = 0x08
CMD_SET_LOCAL_LAYER = 0x09
CMD_MEDIA_SYNC = 0x10
CMD_CHANGE_RECEIVER_LAYER = 0x11
CMD_HELLO = 0x20
CMD_CONFIG_STATE = 0x21
CMD_RUNNING_STATE = 0x22
CMD_ERROR_REPORT = 0x30

CMD_NAMES = {v: k[4:] for k, v in globals().items() if k.startswith('CMD_')}
ROLE_NAMES = {0: 'slave', 1: 'master', 2: 'legacy', 0x7F: 'auto'}
BOARD_NAMES = {0: 'unknown', 1: 'devkit', 2: 'atoms3', 3: 'atoms3-lite'}
ERROR_NAMES = {0x01: 'CONFIG_INVALID', 0x02: 'SYSEX_PARSE_ERROR', 0x03: 'ESPNOW_SEND_FAILED',
               0x04: 'MESH_CLOCK_LOST_SYNC', 0x05: 'RECEIVER_TIMEOUT', 0xFF: 'UNKNOWN'}
RESET_REASONS = {1: 'POWERON', 3: 'SW', 4: 'PANIC', 5: 'INT_WDT', 6: 'TASK_WDT', 7: 'WDT',
                 8: 'DEEPSLEEP', 9: 'BROWNOUT', 10: 'SDIO', 12: 'USB', 15: 'JTAG'}


def encode7(raw):
    out = []
    for i in range(0, len(raw), 7):
        chunk = raw[i:i + 7]
        msb = 0
        for j, b in enumerate(chunk):
            if b & 0x80:
                msb |= 1 << j
        out.append(msb)
        out.extend(b & 0x7F for b in chunk)
    return out


def decode7(enc):
    out = []
    i = 0
    while i < len(enc):
        msb = enc[i]
        i += 1
        for j, b in enumerate(enc[i:i + 7]):
            out.append(b | 0x80 if msb & (1 << j) else b)
        i += 7
    return out


def u32be(n):
    n &= 0xFFFFFFFF
    return [(n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF]


def from_u32be(b):
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]


def layer16(layer):
    return list((str(layer)[:16] + '\x00' * 16)[:16].encode('ascii', errors='replace'))


# ---- host -> node -----------------------------------------------------------------

def query_config():
    return [MANUFACTURER, CMD_QUERY_CONFIG]


def query_running_state():
    return [MANUFACTURER, CMD_QUERY_RUNNING_STATE]


def push_full_config(rf_sim, delay_ms):
    delay_ms = max(0, min(16383, int(delay_ms)))
    return [MANUFACTURER, CMD_PUSH_FULL_CONFIG, 1 if rf_sim else 0, (delay_ms >> 7) & 0x7F, delay_ms & 0x7F]


def set_role(role):
    code = {'slave': 0, 'master': 1, 'auto': 0x7F}[role]
    return [MANUFACTURER, CMD_SET_ROLE, code]


def set_local_layer(layer):
    return [MANUFACTURER, CMD_SET_LOCAL_LAYER] + list(str(layer)[:15].encode('ascii', errors='replace'))


def media_sync(layer, index, position_ms, playing):
    index = max(0, min(127, int(index)))
    return ([MANUFACTURER, CMD_MEDIA_SYNC] + layer16(layer) + [index]
            + encode7(u32be(max(0, int(position_ms)))) + [1 if playing else 0])


def change_receiver_layer(mac, layer):
    return [MANUFACTURER, CMD_CHANGE_RECEIVER_LAYER] + encode7(list(mac)) + encode7(layer16(layer))


def ota_begin(size):
    return [MANUFACTURER, CMD_OTA_BEGIN] + encode7(u32be(size))


def ota_data(chunk):
    return [MANUFACTURER, CMD_OTA_DATA] + encode7(list(chunk))


def ota_end():
    return [MANUFACTURER, CMD_OTA_END]


# ---- node -> host -----------------------------------------------------------------

def hello(version='2.0', uptime_ms=1000, reason=1, role=None, board=None):
    """Build a HELLO payload (for simulators)."""
    v = list(str(version).encode('ascii')[:8].ljust(8, b'\x00'))
    out = [MANUFACTURER, CMD_HELLO] + encode7(v) + encode7(u32be(uptime_ms)) + [reason & 0x7F]
    if role is not None:
        out += [int(role) & 0x7F, int(board or 0) & 0x7F]
    return out


def config_state(rf_sim=False, delay_ms=400, role=None, board=None, layer=None):
    out = [MANUFACTURER, CMD_CONFIG_STATE, 1 if rf_sim else 0, (delay_ms >> 7) & 0x7F, delay_ms & 0x7F]
    if role is not None:
        lb = list(str(layer or '').encode('ascii')[:15])
        out += [int(role) & 0x7F, int(board or 0) & 0x7F, len(lb)] + lb
    return out


def running_state_chunks(receivers, uptime_ms=1000, synced=True):
    """receivers: list of dicts {mac:[6], layer, version, last_seen_ms, index}. One chunk each."""
    chunks = []
    n = len(receivers)
    count = max(1, n)
    for ci in range(count):
        d = [MANUFACTURER, CMD_RUNNING_STATE] + encode7(u32be(uptime_ms)) + [1 if synced else 0, n, ci, count]
        if ci < n:
            r = receivers[ci]
            raw = (list(r['mac']) + layer16(r.get('layer', '-'))
                   + list(str(r.get('version', '2.0')).encode('ascii')[:8].ljust(8, b'\x00'))
                   + u32be(int(r.get('last_seen_ms', 0))) + [1, int(r.get('index', 0)) & 0x7F])
            d += [1] + encode7(raw)
        else:
            d += [0]
        chunks.append(d)
    return chunks


def error_report(code, ctx=()):
    return [MANUFACTURER, CMD_ERROR_REPORT, code & 0x7F, len(ctx)] + [b & 0x7F for b in ctx]


# ---- parsers (payload after the command byte) ---------------------------------------

def parse(data):
    """data = full payload (7D cmd ...). Returns (cmd_name, dict)."""
    if len(data) < 2 or data[0] != MANUFACTURER:
        return None, {}
    cmd, d = data[1], list(data[2:])
    name = CMD_NAMES.get(cmd, '0x%02X' % cmd)
    if cmd == CMD_HELLO and len(d) >= 16:
        info = {'version': bytes(decode7(d[0:10])[:8]).decode('ascii', 'ignore').rstrip('\x00'),
                'uptime_ms': from_u32be(decode7(d[10:15])),
                'boot': RESET_REASONS.get(d[15], 'UNKNOWN_%d' % d[15])}
        if len(d) >= 18:
            info['role'] = ROLE_NAMES.get(d[16], '?')
            info['board'] = BOARD_NAMES.get(d[17], '?')
        return name, info
    if cmd == CMD_CONFIG_STATE and len(d) >= 3:
        info = {'rf_sim': bool(d[0]), 'rf_sim_delay_ms': (d[1] << 7) | d[2]}
        if len(d) >= 6:
            info['role'] = ROLE_NAMES.get(d[3], '?')
            info['board'] = BOARD_NAMES.get(d[4], '?')
            info['layer'] = bytes(d[6:6 + d[5]]).decode('ascii', 'ignore')
        return name, info
    if cmd == CMD_RUNNING_STATE and len(d) >= 10:
        info = {'uptime_ms': from_u32be(decode7(d[0:5])), 'synced': bool(d[5]), 'total': d[6],
                'chunk': d[7], 'chunks': d[8], 'receivers': []}
        idx = 10
        for _ in range(d[9]):
            r = decode7(d[idx:idx + 42])
            idx += 42
            if len(r) < 36:
                break
            info['receivers'].append({
                'mac': ':'.join('%02X' % b for b in r[0:6]), 'mac_bytes': r[0:6],
                'layer': bytes(r[6:22]).decode('ascii', 'ignore').rstrip('\x00'),
                'version': bytes(r[22:30]).decode('ascii', 'ignore').rstrip('\x00'),
                'last_seen_ms': from_u32be(r[30:34]), 'index': r[35]})
        return name, info
    if cmd == CMD_ERROR_REPORT and len(d) >= 2:
        return name, {'error': ERROR_NAMES.get(d[0], hex(d[0])), 'ctx': ' '.join('%02X' % b for b in d[2:2 + d[1]])}
    if cmd == CMD_MEDIA_SYNC and len(d) >= 23:
        return name, {'layer': bytes(d[0:16]).decode('ascii', 'ignore').rstrip('\x00'), 'index': d[16],
                      'position_ms': from_u32be(decode7(d[17:22])), 'playing': d[22] == 1}
    if cmd == CMD_CHANGE_RECEIVER_LAYER and len(d) >= 26:
        return name, {'mac': ':'.join('%02X' % b for b in decode7(d[0:7])),
                      'layer': bytes(decode7(d[7:26])).decode('ascii', 'ignore').rstrip('\x00')}
    if cmd == CMD_SET_ROLE and len(d) >= 1:
        return name, {'role': ROLE_NAMES.get(d[0], '?')}
    if cmd == CMD_SET_LOCAL_LAYER:
        return name, {'layer': bytes(d).decode('ascii', 'ignore')}
    return name, {'raw': ' '.join('%02X' % b for b in d)}
