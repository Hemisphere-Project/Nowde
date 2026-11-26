#pragma once

#include <stdint.h>

// When the slave's next MTC frame is due (#t-056). Pure logic: no Arduino, no clock read. The
// ESP-NOW task in src/main.cpp owns mediaSyncState.mtcNextDueUs and is the only caller;
// tools/mtc_due_test.cpp runs this same header on the host, from tools/selftest.py.
//
// 2.4 schedules MTC by frame on micros(), which wraps every 2^32 us (71.6 min), so "due?" is a
// signed difference. The scheduler only ever sets the due time at most one period ahead of now --
// but nothing moves it while the node is stopped. Read again after a stop of 35.8 to 71.6 min (or
// on a first play at that uptime, from its boot value 0), the stale value lands in the signed
// "not yet" half and the node sent no quarter-frame for up to 35.8 min, while CC#100 and the
// full-frames of a jump still went out: the host played the clip with no clock to follow.
//
// A due time more than one period ahead can only be stale: it is due now.

// Returns true when a frame is due; `due` is then already moved to the next frame. A stall longer
// than a frame re-anchors on now instead of bursting to catch up (2.4's rule, unchanged).
inline bool mtcFrameDue(uint32_t nowUs, uint32_t& due, uint32_t periodUs) {
  int32_t late = static_cast<int32_t>(nowUs - due);
  if (late < -static_cast<int32_t>(periodUs)) {
    due = nowUs;   // stale anchor from before a stop, or from boot
    late = 0;
  }
  if (late < 0) {
    return false;
  }
  due += periodUs;
  if (static_cast<int32_t>(nowUs - due) >= 0) {
    due = nowUs + periodUs;
  }
  return true;
}
