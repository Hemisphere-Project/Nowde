// Host test of src/midi_schedule.h -- the slave's 0x04 schedule (#t-038). tools/selftest.py
// compiles and runs it when a host C++ compiler is on PATH. No framework: one check() that
// exits non-zero, like selftest.py's own.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/midi_schedule.h"

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

struct Played {
  uint32_t at;
  uint8_t status, data1;
};

int main() {
  std::vector<Played> out;
  uint32_t clock = 0;
  auto fire = [&](const ScheduledEvent& e) { out.push_back({clock, e.status, e.data1}); };

  // Nothing leaves before its time; everything leaves at it, earliest first.
  {
    EventSchedule<8> s;
    out.clear();
    s.add({1100, 0x90, 64, 100}, fire);
    s.add({1090, 0x90, 60, 100}, fire);   // arrives later, due sooner
    s.add({1095, 0x90, 62, 100}, fire);
    clock = 1089;
    check(s.fireDue(clock, fire) == 0 && out.empty(), "nothing fires before its time");
    clock = 1095;
    check(s.fireDue(clock, fire) == 2, "two due at 1095");
    check(out[0].data1 == 60 && out[1].data1 == 62, "out-of-order arrivals leave in time order");
    clock = 1200;
    check(s.fireDue(clock, fire) == 1 && out[2].data1 == 64 && s.size() == 0, "the last one");
  }

  // Stable: one millisecond, many records -- they leave in arrival order. A Note Off then a
  // Note On on the same key is a retrigger; swapped, it is a silent key.
  {
    EventSchedule<8> s;
    out.clear();
    s.add({500, 0x80, 60, 0}, fire);
    s.add({500, 0x90, 60, 100}, fire);
    s.add({500, 0x90, 64, 100}, fire);
    clock = 500;
    s.fireDue(clock, fire);
    check(out.size() == 3 && out[0].status == 0x80 && out[1].status == 0x90 && out[1].data1 == 60
              && out[2].data1 == 64,
          "same-ms records keep arrival order (Note Off before the Note On that follows it)");
  }

  // The 2^32 ms wrap (~49.7 days of mesh time): 0xFFFFFFF0 is before 0x10.
  {
    EventSchedule<8> s;
    out.clear();
    s.add({0x00000010u, 0x90, 2, 1}, fire);
    s.add({0xFFFFFFF0u, 0x90, 1, 1}, fire);
    clock = 0xFFFFFFF5u;
    check(s.fireDue(clock, fire) == 1 && out[0].data1 == 1, "pre-wrap record due before the wrap");
    clock = 0x00000005u;
    check(s.fireDue(clock, fire) == 0, "post-wrap record not yet due just after the wrap");
    clock = 0x00000010u;
    check(s.fireDue(clock, fire) == 1 && out[1].data1 == 2, "post-wrap record due on time");
  }

  // Full: the EARLIEST record fires now, never dropped, and order is kept.
  {
    EventSchedule<3> s;
    out.clear();
    clock = 0;
    check(s.add({100, 0x90, 1, 1}, fire) && s.add({110, 0x90, 2, 1}, fire)
              && s.add({120, 0x90, 3, 1}, fire),
          "three fit in three");
    check(!s.add({130, 0x90, 4, 1}, fire), "a fourth reports an early fire");
    check(out.size() == 1 && out[0].data1 == 1 && s.size() == 3, "full: the head fired early");
    check(!s.add({90, 0x90, 0, 1}, fire), "a record due before the head, into a full schedule");
    check(out.size() == 2 && out[1].data1 == 0, "full: the incoming record was the earliest, it fired");
    clock = 1000;
    s.fireDue(clock, fire);
    check(out.size() == 5 && out[2].data1 == 2 && out[3].data1 == 3 && out[4].data1 == 4,
          "what stayed held left in time order");
  }

  // A SNAPSHOT yields only to a held record for the SAME controller on the SAME channel.
  {
    EventSchedule<8> s;
    s.add({100, 0xB1, 7, 64}, fire);
    check(s.holds(0xB1, 7), "held CC 7 on channel 2");
    check(!s.holds(0xB0, 7), "another channel is another controller");
    check(!s.holds(0xB1, 10), "another controller");
  }

  // The clock-trust estimate (frameClockTrusted), with v2.3's figures: lead 90, threshold 200.
  {
    const uint32_t lead = 90, thr = 200;
    // A fresh frame on agreeing clocks: captured at 1000, fireAt 1090, heard at 1005.
    check(frameClockTrusted(1005, 1090, true, lead, thr), "fresh frame, clocks agree");
    // The same frame after three 30 ms hops.
    check(frameClockTrusted(1095, 1090, true, lead, thr), "relayed frame, clocks agree");
    // A carry-over or SNAPSHOT states its mint, not mint + lead.
    check(frameClockTrusted(1003, 1000, false, lead, thr), "carry-over frame, clocks agree");
    // This node 300 ms AHEAD of the master, then 69 s BEHIND it (the 2026-09-07 failure).
    check(!frameClockTrusted(1305, 1090, true, lead, thr), "300 ms ahead: not trusted");
    check(!frameClockTrusted(1005u - 69000u, 1090, true, lead, thr), "69 s behind: not trusted");
    // And across the wrap: a master just past it, this node just before.
    check(frameClockTrusted(0xFFFFFFFBu, 0x00000055u, true, lead, thr), "trusted across the wrap");
  }

  // Where a record goes (recordFiresAt): rule 4 on a trusted clock, play-on-arrival otherwise.
  {
    uint32_t at = 0;
    check(recordFiresAt(1005, 1090, 3, true, 150, &at) && at == 1093, "scheduled at fireAt + offset");
    check(recordFiresAt(1100, 1090, -5, true, 150, &at) && at == 1085,
          "a carried record (negative offset) a little late: kept, due at once");
    check(!recordFiresAt(1300, 1090, 0, true, 150, &at), "late by more than 150 ms: dropped");
    check(recordFiresAt(1240, 1090, 0, true, 150, &at) && at == 1090, "late by exactly 150 ms: kept");
    check(recordFiresAt(1305, 1090, 0, false, 150, &at) && at == 1305,
          "clock not trusted: never stale, fires on arrival");
    const uint32_t behind = 1005u - 69000u;   // wraps, as mesh time does
    check(recordFiresAt(behind, 1090, 0, false, 150, &at) && at == behind,
          "clock 69 s behind: fires on arrival, not in 69 s");
  }

  // v2.3's depth: every record the frame cap can carry across one lead plus one batch window.
  // (90 / 5 + 1) x 14 = 266. tools/selftest.py checks src/nowde_config.h states this formula.
  check((90 / 5 + 1) * 14 == 266, "schedule depth arithmetic");

  std::printf("midi_schedule host test OK\n");
  return 0;
}
