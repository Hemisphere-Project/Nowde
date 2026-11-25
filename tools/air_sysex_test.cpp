// Host test of src/air_sysex.h -- which raw SysEx a node runs from the air (#t-053).
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. No framework: one
// check() that exits non-zero, like selftest.py's own.
#include <cstdio>
#include <cstdlib>

#include "../src/air_sysex.h"

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static AirSysExNode slave(bool originLocked, bool unassigned) {
  AirSysExNode n;
  n.isSender = false;
  n.originLocked = originLocked;
  n.unassigned = unassigned;
  return n;
}

static AirSysExSource source(bool lockedOrigin, bool knownSender) {
  AirSysExSource s;
  s.lockedOrigin = lockedOrigin;
  s.knownSender = knownSender;
  return s;
}

int main() {
  // 2.4 as it shipped: the layer command from the locked master runs, on any layer.
  check(airSysExVerdict(true, slave(true, false), source(true, true)) == AIR_SYSEX_RUN_LOCKED,
        "locked master re-layers its layered slave");
  check(airSysExVerdict(true, slave(true, true), source(true, true)) == AIR_SYSEX_RUN_LOCKED,
        "locked master (on '*') layers its '-' slave");

  // #t-053, the rig's case: a slave on "-" follows nobody (no master of its layer), the master it
  // hears offers its layer. 2.4.0 dropped it -- "not from the locked origin" -- and the slave
  // stayed deaf for good.
  AirSysExVerdict first = airSysExVerdict(true, slave(false, true), source(false, true));
  check(first == AIR_SYSEX_RUN_FIRST_LAYER && airSysExRuns(first),
        "a never-layered, unlocked slave takes its first layer from a master it heard");

  // ... and nothing wider than that.
  check(airSysExVerdict(true, slave(false, true), source(false, false)) == AIR_SYSEX_DROP_NOT_ORIGIN,
        "not from a node whose beacon it never heard");
  check(airSysExVerdict(true, slave(false, false), source(false, true)) == AIR_SYSEX_DROP_NOT_ORIGIN,
        "a slave already on a layer is not re-layered by a master it does not follow");
  check(airSysExVerdict(true, slave(true, true), source(false, true)) == AIR_SYSEX_DROP_NOT_ORIGIN,
        "a '-' slave that follows a master takes its layer from that master only");
  AirSysExNode master = slave(false, true);
  master.isSender = true;
  check(airSysExVerdict(true, master, source(false, true)) == AIR_SYSEX_DROP_NOT_ORIGIN,
        "a master on '-' never takes a layer from another master");

  // Every other command stays dropped from everyone, the locked master and the first-layer case
  // included.
  check(airSysExVerdict(false, slave(true, false), source(true, true)) == AIR_SYSEX_DROP_NOT_WHITELISTED,
        "OTA / SET_ROLE / SET_LR from the locked master: dropped");
  check(airSysExVerdict(false, slave(false, true), source(false, true)) == AIR_SYSEX_DROP_NOT_WHITELISTED,
        "any other command to a '-' slave: dropped");
  check(!airSysExRuns(AIR_SYSEX_DROP_NOT_WHITELISTED) && !airSysExRuns(AIR_SYSEX_DROP_NOT_ORIGIN),
        "drops never run");

  std::printf("air_sysex host test OK (10 checks)\n");
  return 0;
}
