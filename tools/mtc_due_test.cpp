// Host test of src/mtc_due.h -- when the slave's next MTC frame is due (#t-056).
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. No framework: one
// check() that exits non-zero, like selftest.py's own.
#include <cstdio>
#include <cstdlib>

#include "../src/mtc_due.h"

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static const uint32_t P = 1000000UL / 30;          // one MTC frame at 30 fps, as main.cpp
static const uint32_t MIN_US = 60UL * 1000000UL;

// How many frames a loop ticking every 10 ms sends in `spanUs`, starting at `startUs` with `due`.
static int framesIn(uint32_t startUs, uint32_t due, uint32_t spanUs) {
  int n = 0;
  for (uint32_t t = 0; t < spanUs; t += 10000) {
    if (mtcFrameDue(startUs + t, due, P)) n++;
  }
  return n;
}

int main() {
  int checks = 0;

  // Playing steadily: 30 frames a second, never a burst.
  check(framesIn(5 * MIN_US, 5 * MIN_US, 1000000) == 30, "steady play sends 30 frames a second");
  checks++;

  // A frame not yet due stays not due, and fires once its time comes.
  uint32_t due = 1000000 + P;
  check(!mtcFrameDue(1000000 + P - 1, due, P), "a frame one microsecond early is not due");
  check(due == 1000000 + P, "and its due time does not move");
  check(mtcFrameDue(1000000 + P, due, P), "the same frame fires on time");
  check(due == 1000000 + 2 * P, "and the next one is one period later");
  checks += 4;

  // A stall longer than a frame re-anchors on now: one frame, no catch-up burst.
  due = 1000000;
  check(mtcFrameDue(1000000 + 5 * P, due, P), "a late frame fires");
  check(due == 1000000 + 6 * P, "after a stall the next frame is one period from now");
  checks += 2;

  // The #t-056 case: the last frame went out, then the node sat stopped for 40 min. The 2.4.1
  // compare reads that due time as 31.6 min in the future; it is due now.
  uint32_t last = 7 * MIN_US;
  uint32_t stopped = 40 * MIN_US;
  check(framesIn(last + stopped, last + P, 1000000) == 30,
        "after a 40 min stop the first second of play carries 30 frames");
  checks++;

  // Every stop length up to two micros() wraps (143 min), by the minute: play resumes at once.
  for (uint32_t m = 1; m <= 143; m++) {
    uint32_t now = last + m * MIN_US;
    uint32_t d = last + P;
    check(mtcFrameDue(now, d, P), "a play after any stop length sends its first frame at once");
    checks++;
  }

  // First play after boot (due still 0) at an uptime of 50 min: due at once.
  due = 0;
  check(mtcFrameDue(50 * MIN_US, due, P), "a first play at 50 min uptime sends a frame at once");
  checks++;

  // Across the micros() wrap while playing: still 30 a second.
  check(framesIn(0xFFFFFFFFUL - 500000UL, 0xFFFFFFFFUL - 500000UL, 1000000) == 30,
        "playing across the micros() wrap sends 30 frames a second");
  checks++;

  std::printf("mtc_due host test OK (%d checks)\n", checks);
  return 0;
}
