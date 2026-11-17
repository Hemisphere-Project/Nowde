#pragma once

#include <stdint.h>

// v2.3 MIDI_EVENT (0x04), the slave's SCHEDULE (#t-038) -- docs/PROTOCOL.md "Slave-side handling
// of a 0x04 frame" and "The schedule". Pure logic: no Arduino, no FreeRTOS, no clock read. The
// firmware's MIDI_Task owns the one EventSchedule and is its only caller (src/midi_event.cpp);
// tools/midi_schedule_test.cpp runs this same header on the host, from tools/selftest.py.
//
// Every time here is mesh ms (meshMillis()), which wraps at 2^32 ms: times are only ever compared
// as (int32_t) differences, never as unsigned values.

struct ScheduledEvent {
  uint32_t firesAt;   // mesh ms: the frame's fireAt + the record's offset
  uint8_t status, data1, data2;
};

// Is `a` strictly earlier than `b`, across the wrap?
inline bool meshBefore(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}

// Does this node's mesh clock agree with the master's closely enough to schedule a frame on it?
// The frame does not carry its mint time, but it states it to within one batch window: a frame
// with new records says fireAt = capture + lead, a pure carry-over or a SNAPSHOT says fireAt =
// mint. `leadMs` is THIS node's MIDI_EVENT_LEAD_MS -- a master built on another lead moves the
// estimate by the difference, inside a tolerance of `thresholdMs` (CLOCK_DESYNC_THRESHOLD_MS, the
// same 200 ms at which a MediaSync is accepted without compensation). Transit is in the estimate,
// exactly as it is in MediaSync's delta.
inline bool frameClockTrusted(uint32_t now, uint32_t fireAt, bool statesLead, uint32_t leadMs,
                              uint32_t thresholdMs) {
  uint32_t mintedAt = fireAt - (statesLead ? leadMs : 0);
  int32_t skew = static_cast<int32_t>(now - mintedAt);
  int32_t limit = static_cast<int32_t>(thresholdMs);
  return skew <= limit && skew >= -limit;
}

// When one (non-SNAPSHOT) record fires, or false = drop it as stale (rule 4).
// Clock trusted: at fireAt + offset; past by more than `maxStaleMs` -> dropped; past by less ->
// due at once. Clock NOT trusted: at `now`, and never stale -- "late" measured on a clock that
// disagrees says nothing about the record, and dropping on it is the -69 s bug in another place
// (knowledge: never gate delivery on clock agreement). Precision is lost, the event is not.
inline bool recordFiresAt(uint32_t now, uint32_t fireAt, int16_t offset, bool clockTrusted,
                          uint32_t maxStaleMs, uint32_t* firesAt) {
  if (!clockTrusted) {
    *firesAt = now;
    return true;
  }
  uint32_t at = fireAt + static_cast<int32_t>(offset);
  if (static_cast<int32_t>(now - at) > static_cast<int32_t>(maxStaleMs)) {
    return false;
  }
  *firesAt = at;
  return true;
}

// The records a slave holds until their fire time, earliest first. A ring, so firing the head
// costs nothing; insertion is STABLE -- a record lands after every record due the same
// millisecond -- so a chord, or a Note Off then Note On retrigger on one key, leaves in the order
// the master captured it. Records almost always arrive in time order: an insert walks back zero
// or one slot.
template <uint16_t Depth>
class EventSchedule {
 public:
  // Hold `e` until it is due. Full: the EARLIEST record -- the head, or `e` itself when it is due
  // sooner -- goes to `fire` now instead, early rather than never, and what is held still leaves
  // in time order. Returns false when that happened (something fired early).
  template <typename Fire>
  bool add(const ScheduledEvent& e, Fire&& fire) {
    bool onTime = true;
    if (count_ == Depth) {
      onTime = false;
      if (meshBefore(e.firesAt, at(0).firesAt)) {
        fire(e);
        return false;
      }
      fire(at(0));
      pop();
    }
    uint16_t pos = count_;
    while (pos > 0 && meshBefore(e.firesAt, at(pos - 1).firesAt)) {
      at(pos) = at(pos - 1);
      pos--;
    }
    at(pos) = e;
    count_++;
    return onTime;
  }

  // Every record due at `now` goes to `fire`, earliest first. Returns how many.
  template <typename Fire>
  uint16_t fireDue(uint32_t now, Fire&& fire) {
    uint16_t n = 0;
    while (count_ > 0 && !meshBefore(now, at(0).firesAt)) {
      fire(at(0));
      pop();
      n++;
    }
    return n;
  }

  // Is a record with this status byte and first data byte still waiting? A SNAPSHOT record for a
  // controller the stream is about to set yields to the stream (docs/PROTOCOL.md "The schedule").
  bool holds(uint8_t status, uint8_t data1) const {
    for (uint16_t i = 0; i < count_; i++) {
      const ScheduledEvent& e = at(i);
      if (e.status == status && e.data1 == data1) {
        return true;
      }
    }
    return false;
  }

  uint16_t size() const { return count_; }
  static constexpr uint16_t capacity() { return Depth; }

 private:
  static uint16_t slot(uint32_t i) { return static_cast<uint16_t>(i % Depth); }
  ScheduledEvent& at(uint16_t i) { return ring_[slot(static_cast<uint32_t>(head_) + i)]; }
  const ScheduledEvent& at(uint16_t i) const { return ring_[slot(static_cast<uint32_t>(head_) + i)]; }
  void pop() {
    head_ = slot(static_cast<uint32_t>(head_) + 1);
    count_--;
  }

  ScheduledEvent ring_[Depth] = {};
  uint16_t head_ = 0;
  uint16_t count_ = 0;
};
