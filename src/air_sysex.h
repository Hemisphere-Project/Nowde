#pragma once

#include <stdint.h>

// 2.4 -- which raw SysEx arriving over ESP-NOW runs (docs/PROTOCOL.md, the 0xF0 row of the
// ESP-NOW message table). Pure logic: no Arduino, no ESP-NOW state read. src/esp_now_handlers.cpp
// gathers the facts and is the only caller; tools/air_sysex_test.cpp runs this same header on the
// host, from tools/selftest.py.
//
// One host command is ever sent over the air: a master's CHANGE_RECEIVER_LAYER to one slave
// (src/sysex.cpp, the short `F0 7D 11 <layer> F7`). 2.4 runs it from the master the slave is
// origin-locked to, and nothing else from anyone. That alone left a never-layered slave deaf: on
// the factory "-" it locks only on a master of its own layer, so the master offering it a layer is
// never its locked origin (#t-053; hplayer2#t-062's rig, 25/11 -- HPlayer2 offers its layer to
// every slave still on "-", the master logged the offer, the slave dropped it, sync quality 0).
//
// The one widening: a SLAVE still on "-" that follows nobody yet takes that same command from a
// master whose beacon it heard -- its first layer, the way every node got one before 2.4. A slave
// on any other layer, or locked to a master, still takes it only from that master; a master never
// takes one; every other command is dropped from everyone.

enum AirSysExVerdict : uint8_t {
  AIR_SYSEX_RUN_LOCKED = 0,        // CHANGE_RECEIVER_LAYER from the master this slave follows
  AIR_SYSEX_RUN_FIRST_LAYER,       // CHANGE_RECEIVER_LAYER from a master heard, to a node on "-"
  AIR_SYSEX_DROP_NOT_WHITELISTED,  // any other command
  AIR_SYSEX_DROP_NOT_ORIGIN,       // the layer command from a node this one may not take it from
};

struct AirSysExNode {
  bool isSender;      // this node drives the mesh (a master)
  bool originLocked;  // it already follows a master (OriginLock.valid)
  bool unassigned;    // its layer is still the factory "-" (NOWDE_LAYER_UNASSIGNED)
};

struct AirSysExSource {
  bool lockedOrigin;  // the frame's src IS the master this node follows
  bool knownSender;   // the frame's src is in this node's sender table (its beacon was heard)
};

inline AirSysExVerdict airSysExVerdict(bool layerCommand, const AirSysExNode& node,
                                       const AirSysExSource& from) {
  if (!layerCommand) {
    return AIR_SYSEX_DROP_NOT_WHITELISTED;
  }
  if (from.lockedOrigin) {
    return AIR_SYSEX_RUN_LOCKED;
  }
  if (!node.isSender && !node.originLocked && node.unassigned && from.knownSender) {
    return AIR_SYSEX_RUN_FIRST_LAYER;
  }
  return AIR_SYSEX_DROP_NOT_ORIGIN;
}

inline bool airSysExRuns(AirSysExVerdict v) {
  return v == AIR_SYSEX_RUN_LOCKED || v == AIR_SYSEX_RUN_FIRST_LAYER;
}

// The serial log's words for a verdict.
inline const char* airSysExReason(AirSysExVerdict v) {
  switch (v) {
    case AIR_SYSEX_RUN_LOCKED:
      return "from the locked origin";
    case AIR_SYSEX_RUN_FIRST_LAYER:
      return "first layer, from a master heard";
    case AIR_SYSEX_DROP_NOT_WHITELISTED:
      return "not whitelisted";
    case AIR_SYSEX_DROP_NOT_ORIGIN:
    default:
      return "not from the locked origin";
  }
}
