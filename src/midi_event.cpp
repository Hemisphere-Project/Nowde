#include "midi_event.h"

#include "esp_now_handlers.h"   // relaySend()
#include "mesh_relay.h"         // midiEventFrameComplete()
#include "nowde_config.h"
#include "nowde_state.h"
#include "usb_out.h"

// docs/PROTOCOL.md "The v2.3 event frame (0x04)" -- what a v2.3 master emits:
//   - host events are batched over MIDI_EVENT_BATCH_MS, opened by the first event of a batch;
//   - a CC repeated inside one batch keeps only its newest value, per (channel, controller);
//   - a batch that reaches MIDI_EVENT_MAX_NEW is sent at once and the next event opens another
//     batch in the same tick -- the cap bounds a frame, never the stream: nothing is dropped;
//   - every frame repeats the previous frame's NEW records (depth 1), re-based on its own fireAt;
//     when no new event follows within one batch window, that repeat goes out alone
//     (newCount 0, fireAt = mesh time at mint).
// groupMask is 0xFFFF and the layer is this node's own -- see midiEventLayer() below.
//   - about once a second (MIDI_EVENT_SNAPSHOT_MS) the master's CC table goes out as SNAPSHOT
//     frames: controllers only, <= MIDI_EVENT_MAX_NEW per frame, offset 0, no carry, evSeq 0
//     (a slave applies them at once and never dedups them). A snapshot waits for an open batch
//     or an owed carry: live events first.

namespace {

struct PendingEvent {
  uint8_t status, data1, data2;
  uint32_t capturedAt;   // mesh ms
};

struct SentEvent {
  uint8_t status, data1, data2;
  uint16_t evSeq;
  uint32_t firesAt;      // mesh ms: the frame's fireAt + this record's offset
};

PendingEvent batch[MIDI_EVENT_MAX_NEW];
uint8_t batchCount = 0;
uint32_t batchOpenedAt = 0;          // mesh ms of the batch's first event

SentEvent carry[MIDI_EVENT_MAX_NEW]; // the last frame's new records, for depth-1 carry-over
uint8_t carryCount = 0;
bool carryOwed = false;              // carry[] has not yet ridden a later frame
uint32_t carryDueAt = 0;             // mesh ms: send it alone if nothing new by then

uint16_t frameSeq = 0;               // the 0x04 FRAME counter -- not MediaSyncPacket.seq
uint16_t eventSeq = 0;               // the per-record evSeq counter -- carry-over dedups on it

// SNAPSHOT: the last value the host sent per (channel, controller), value + 1 (0 = never seen).
constexpr uint16_t CC_SLOTS = 16 * 128;
uint8_t ccPlusOne[16][128] = {};
bool snapActive = false;             // a pass is walking the table
uint16_t snapCursor = 0;             // next slot of the pass, channel << 7 | controller
uint32_t snapNextAt = 0;             // mesh ms: the next pass, or the pass's next frame

// "MIDI follows the layer" (docs/PROTOCOL.md "groupMask, and the assumption v2.3 ships"): a
// slave filters 0x04 on the layer it already carries. Host MIDI arrives with no layer, so the
// master stamps the one it is configured with (SET_LOCAL_LAYER / the bridge GUI; "*" reaches
// "*" slaves). Master-side policy, not a wire field: changing it never touches the frame.
void midiEventLayer(char* out) {
  strncpy(out, subscribedLayer, MAX_LAYER_LENGTH);
  out[MAX_LAYER_LENGTH - 1] = '\0';
}

void putRecord(uint8_t* rec, uint8_t status, uint8_t d1, uint8_t d2, uint16_t evSeq, int16_t offset) {
  rec[0] = status;
  rec[1] = d1 & 0x7F;
  rec[2] = d2 & 0x7F;
  memcpy(rec + 3, &evSeq, sizeof(evSeq));     // little-endian, as the header's packed fields
  memcpy(rec + 5, &offset, sizeof(offset));
}

// An offset outside i16 cannot be written; such a carry is long stale at every slave anyway.
bool fitsOffset(int32_t delta) {
  return delta >= INT16_MIN && delta <= INT16_MAX;
}

// Mint one frame from the open batch (possibly empty) plus the owed carry, and send it.
void sendFrame(uint32_t fireAt) {
  uint8_t frame[MIDI_EVENT_FRAME_MAX];
  MidiEventHeader header;
  header.hdrLen = MIDI_EVENT_HEADER_LEN;
  header.flags = 0;
  header.recLen = MIDI_EVENT_RECORD_LEN;
  header.groupMask = 0xFFFF;               // v2.3 masters address every channel
  header.seq = ++frameSeq;
  header.fireAt = fireAt;
  header.newCount = batchCount;
  header.carryCount = 0;
  midiEventLayer(header.layer);

  uint8_t* rec = frame + MIDI_EVENT_HEADER_LEN;
  SentEvent sent[MIDI_EVENT_MAX_NEW];
  for (uint8_t i = 0; i < batchCount; i++) {
    const PendingEvent& e = batch[i];
    // >= 0 and ascending; record 0 fires at fireAt exactly (offset 0)
    int16_t offset = static_cast<int16_t>(e.capturedAt + MIDI_EVENT_LEAD_MS - fireAt);
    uint16_t evSeq = ++eventSeq;
    putRecord(rec, e.status, e.data1, e.data2, evSeq, offset);
    rec += MIDI_EVENT_RECORD_LEN;
    sent[i] = {e.status, e.data1, e.data2, evSeq, fireAt + offset};
  }
  if (carryOwed) {
    for (uint8_t i = 0; i < carryCount; i++) {
      int32_t delta = static_cast<int32_t>(carry[i].firesAt - fireAt);   // negative behind new records
      if (!fitsOffset(delta)) continue;
      putRecord(rec, carry[i].status, carry[i].data1, carry[i].data2, carry[i].evSeq,
                static_cast<int16_t>(delta));
      rec += MIDI_EVENT_RECORD_LEN;
      header.carryCount++;
    }
  }
  memcpy(frame, &header, sizeof(header));
  int len = MIDI_EVENT_HEADER_LEN + (header.newCount + header.carryCount) * MIDI_EVENT_RECORD_LEN;

  // A direct 0x04 floods like a direct 0x03 (#t-037 slice 1): slaves in range relay it on.
  relaySend(broadcastAddress, frame, len);

  // Depth 1: what rides the NEXT frame is this frame's new records, never its carried ones.
  memcpy(carry, sent, sizeof(SentEvent) * batchCount);
  carryCount = batchCount;
  carryOwed = batchCount > 0;
  carryDueAt = meshMillisStable() + MIDI_EVENT_BATCH_MS;
  batchCount = 0;
}

// One SNAPSHOT frame from the pass's cursor; ends the pass once the table has been walked.
void snapshotTick(uint32_t now) {
  if (static_cast<int32_t>(now - snapNextAt) < 0) return;
  if (!snapActive) {
    snapActive = true;
    snapCursor = 0;
  }
  uint8_t frame[MIDI_EVENT_FRAME_MAX];
  uint8_t* rec = frame + MIDI_EVENT_HEADER_LEN;
  uint8_t n = 0;
  while (snapCursor < CC_SLOTS && n < MIDI_EVENT_MAX_NEW) {
    uint8_t channel = snapCursor >> 7;
    uint8_t controller = snapCursor & 0x7F;
    uint8_t value = ccPlusOne[channel][controller];
    snapCursor++;
    if (value == 0) continue;
    putRecord(rec, 0xB0 | channel, controller, value - 1, 0, 0);
    rec += MIDI_EVENT_RECORD_LEN;
    n++;
  }
  if (n > 0) {
    MidiEventHeader header;
    header.hdrLen = MIDI_EVENT_HEADER_LEN;
    header.flags = MIDI_EVENT_FLAG_SNAPSHOT;
    header.recLen = MIDI_EVENT_RECORD_LEN;
    header.groupMask = 0xFFFF;
    header.seq = ++frameSeq;               // its own frame: the relay dedups on it like any other
    header.fireAt = now;
    header.newCount = n;
    header.carryCount = 0;
    midiEventLayer(header.layer);
    memcpy(frame, &header, sizeof(header));
    relaySend(broadcastAddress, frame, MIDI_EVENT_HEADER_LEN + n * MIDI_EVENT_RECORD_LEN);
  }
  if (snapCursor >= CC_SLOTS) {
    snapActive = false;
    snapNextAt = now + MIDI_EVENT_SNAPSHOT_MS;
  } else {
    snapNextAt = now + MIDI_EVENT_BATCH_MS;  // more of this pass, one batch window apart
  }
}

void flushBatch() {
  if (batchCount == 0) return;
  sendFrame(batch[0].capturedAt + MIDI_EVENT_LEAD_MS);   // fireAt = capture + lead
}

}  // namespace

void midiEventCapture(uint8_t status, uint8_t data1, uint8_t data2) {
  if (!senderModeEnabled) return;
  if (status < 0x80 || status > 0xEF) return;      // channel voice only (docs/PROTOCOL.md, Record)
  uint8_t kind = status & 0xF0;
  if (kind == 0xC0 || kind == 0xD0) data2 = 0;     // one-data-byte statuses carry data2 = 0

  uint32_t now = meshMillisStable();
  if (kind == 0xB0) {
    ccPlusOne[status & 0x0F][data1 & 0x7F] = (data2 & 0x7F) + 1;   // what SNAPSHOT re-sends
  }
  if (batchCount == 0) {
    batchOpenedAt = now;
  }

  // CC coalescing per (channel, controller): the newest value replaces the older record, and
  // moves to the end so records stay in ascending offset with the time it was actually sent.
  if (kind == 0xB0) {
    for (uint8_t i = 0; i < batchCount; i++) {
      if (batch[i].status == status && batch[i].data1 == data1) {
        memmove(&batch[i], &batch[i + 1], sizeof(PendingEvent) * (batchCount - i - 1));
        batchCount--;
        break;
      }
    }
    if (batchCount == 0) batchOpenedAt = now;   // it was the batch's only record
  }

  batch[batchCount++] = {status, data1, data2, now};
  if (batchCount >= MIDI_EVENT_MAX_NEW) {
    flushBatch();    // full: send now, the next event opens a new batch in this same tick
  }
}

void midiEventTick() {
  if (!senderModeEnabled) {
    batchCount = 0;
    carryOwed = false;
    snapActive = false;
    return;
  }
  uint32_t now = meshMillisStable();
  if (batchCount > 0) {
    if (static_cast<int32_t>(now - batchOpenedAt) >= MIDI_EVENT_BATCH_MS) {
      flushBatch();
    }
    return;
  }
  if (carryOwed && static_cast<int32_t>(now - carryDueAt) >= 0) {
    sendFrame(now);   // the pure carry-over frame: newCount 0, fireAt = mesh time at mint
    return;
  }
  snapshotTick(now);
}

// ============================== the SLAVE leg (slice 3) ==============================
// docs/PROTOCOL.md "Slave-side handling of a 0x04 frame". Runs in the WiFi task (onDataRecv),
// which owns everything below; the host is reached only through usbOutMidi()'s queue.

namespace {

// evSeq dedup: bit i of `fired` = event (top - i) already played. Distinct from the relay's
// (origin, type, seq) FRAME dedup on purpose -- a carried record arrives in a NEW frame.
struct EvSeqWindow {
  bool valid = false;
  uint8_t origin[6] = {};
  uint16_t top = 0;
  uint32_t fired[MIDI_EVENT_DEDUP_WINDOW / 32] = {};
  unsigned long lastFresh = 0;   // millis()
} rxWindow;

bool windowBit(uint16_t age) {
  return rxWindow.fired[age >> 5] & (1u << (age & 31));
}

void windowSet(uint16_t age, bool on) {
  uint32_t mask = 1u << (age & 31);
  if (on) rxWindow.fired[age >> 5] |= mask;
  else rxWindow.fired[age >> 5] &= ~mask;
}

// true = first sighting, now marked fired; false = already fired, or too far behind to tell.
bool evSeqFresh(const uint8_t* origin, uint16_t evSeq) {
  unsigned long now = millis();
  if (!rxWindow.valid || memcmp(rxWindow.origin, origin, 6) != 0 ||
      now - rxWindow.lastFresh > MIDI_EVENT_DEDUP_IDLE_MS) {
    // A new master, or one quiet long enough to have rebooted: its counter starts afresh.
    rxWindow.valid = true;
    memcpy(rxWindow.origin, origin, 6);
    memset(rxWindow.fired, 0, sizeof(rxWindow.fired));
    rxWindow.top = evSeq;
    windowSet(0, true);
    rxWindow.lastFresh = now;
    return true;
  }
  int16_t ahead = static_cast<int16_t>(evSeq - rxWindow.top);
  if (ahead > 0) {
    for (int i = MIDI_EVENT_DEDUP_WINDOW - 1; i >= 0; i--) {   // slide: old bit i -> i + ahead
      windowSet(i, i >= ahead && windowBit(i - ahead));
    }
    rxWindow.top = evSeq;
    windowSet(0, true);
    rxWindow.lastFresh = now;
    return true;
  }
  uint16_t age = static_cast<uint16_t>(-ahead);
  if (age >= MIDI_EVENT_DEDUP_WINDOW || windowBit(age)) {
    return false;
  }
  windowSet(age, true);
  rxWindow.lastFresh = now;
  return true;
}

// Rule 5: re-emit on the record's own channel. Channel voice: USB-MIDI CIN = the status nibble.
void playRecord(uint8_t status, uint8_t data1, uint8_t data2) {
  uint8_t kind = status & 0xF0;
  midiEventPacket_t packet;
  packet.header = kind >> 4;
  packet.byte1 = status;
  packet.byte2 = data1 & 0x7F;
  packet.byte3 = (kind == 0xC0 || kind == 0xD0) ? 0 : (data2 & 0x7F);
  usbOutMidi(packet);
}

}  // namespace

void midiEventReceive(const uint8_t* origin, const uint8_t* frame, int len) {
  if (!midiEventFrameComplete(frame, len)) {
    return;   // validation rules 1-2; a longer frame is accepted and its tail ignored (rule 3)
  }
  MidiEventHeader header;
  memcpy(&header, frame, sizeof(header));

  // 1. MIDI follows the layer (his 20/09 ruling): the MediaSync filter, and the only one --
  //    groupMask is on the wire and ignored in v2.3 (docs/PROTOCOL.md "groupMask").
  if (!layerMatches(subscribedLayer, header.layer)) {
    return;
  }
  // 2. The origin lock, unchanged: the master this node follows, or nothing.
  if (!originAccepts(origin)) {
    return;
  }

  const bool snapshot = header.flags & MIDI_EVENT_FLAG_SNAPSHOT;   // other flag bits: ignored
  const int records = header.newCount + header.carryCount;
  const uint8_t* rec = frame + header.hdrLen;                       // never our own sizeof
  const uint32_t now = meshMillisStable();

  for (int i = 0; i < records; i++, rec += header.recLen) {
    uint8_t status = rec[0];
    if (status < 0x80 || status > 0xEF) {
      continue;   // not channel voice: skip the record, keep the frame
    }
    if (snapshot) {
      // Current state, at once: no schedule, no evSeq dedup. Controllers only -- a Note On
      // replayed from a snapshot would retrigger.
      if ((status & 0xF0) == 0xB0) {
        playRecord(status, rec[1], rec[2]);
      }
      continue;
    }
    uint16_t evSeq;
    int16_t offset;
    memcpy(&evSeq, rec + 3, sizeof(evSeq));
    memcpy(&offset, rec + 5, sizeof(offset));
    uint32_t firesAt = header.fireAt + static_cast<int32_t>(offset);
    // 4. Stale first, so a copy too old to play never touches the dedup window.
    if (static_cast<int32_t>(now - firesAt) > MIDI_EVENT_MAX_STALE_MS) {
      continue;
    }
    // 3. Fired once, whichever frame or path brought it.
    if (!evSeqFresh(origin, evSeq)) {
      continue;
    }
    playRecord(status, rec[1], rec[2]);   // on arrival; #t-038 moves this onto the 1 ms tick
  }
}
