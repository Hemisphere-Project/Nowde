#include "midi_event.h"

#include "esp_now_handlers.h"   // relaySend()
#include "nowde_config.h"
#include "nowde_state.h"

// docs/PROTOCOL.md "The v2.3 event frame (0x04)" -- what a v2.3 master emits:
//   - host events are batched over MIDI_EVENT_BATCH_MS, opened by the first event of a batch;
//   - a CC repeated inside one batch keeps only its newest value, per (channel, controller);
//   - a batch that reaches MIDI_EVENT_MAX_NEW is sent at once and the next event opens another
//     batch in the same tick -- the cap bounds a frame, never the stream: nothing is dropped;
//   - every frame repeats the previous frame's NEW records (depth 1), re-based on its own fireAt;
//     when no new event follows within one batch window, that repeat goes out alone
//     (newCount 0, fireAt = mesh time at mint).
// groupMask is 0xFFFF and the layer is this node's own -- see midiEventLayer() below.

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
  }
}
