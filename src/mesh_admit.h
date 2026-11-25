#pragma once

// Nowde 3 admission (#t-050 ⑤, proposal §6.6, rail §Wire contract): one mesh per show.
// Pure logic, no Arduino: tools/mesh_admit_test.cpp runs it on the host from selftest.
//
// A node's mesh id (SET_MESH 0x12, NVS, 14 bits) rides the only v3 trailer there is, the
// beacon's. Two nodes hear each other unless BOTH carry a non-zero id and the ids differ:
// - 0 on the node = open: it hears every mesh, as every 2.x node does;
// - 0 on the frame = an open sender: every node hears it;
// - the rule is symmetric, so admission never makes one sender hear a rival that cannot hear
//   it back (the election compares ranks both ways).
// Only a beacon names its mesh, so a node remembers the MACs whose beacon named another one and
// drops EVERY frame from them, before any dispatch (MCK, MEDIA_SYNC, 0x04, relays, company):
// a slave locks on MEDIA_SYNC by layer, not by beacon, so dropping the beacon alone would let a
// foreign master on the same layer drive it. Legacy frames (1-byte beacons, 2.x / v1.2 nodes)
// carry no id and pass as before.

#include <stdint.h>
#include <string.h>

namespace mesh_admit {

static const uint16_t OPEN = 0;
static const uint16_t MAX_ID = 0x3FFF;  // 14 bits: two 7-bit SysEx bytes

inline bool admits(uint16_t mine, uint16_t theirs) {
  return mine == OPEN || theirs == OPEN || mine == theirs;
}

// SET_MESH's payload: low 7 bits first, then the high 7.
inline uint16_t fromSysEx(uint8_t lo, uint8_t hi) {
  return (uint16_t)((lo & 0x7F) | ((hi & 0x7F) << 7));
}

// The nodes of another mesh heard lately. An entry lives `timeoutMs` after that node's last
// foreign beacon (the sender table's 5 s); a beacon of an admitted mesh from it forgets it at
// once. Full table: the stalest entry is replaced. Touched by the receive callback only.
template <int N>
struct ForeignTable {
  uint8_t mac[N][6];
  uint32_t at[N];
  bool used[N];

  ForeignTable() {
    for (int i = 0; i < N; i++) used[i] = false;
  }

  int find(const uint8_t* m) const {
    for (int i = 0; i < N; i++) {
      if (used[i] && memcmp(mac[i], m, 6) == 0) return i;
    }
    return -1;
  }

  void note(const uint8_t* m, uint32_t now) {
    int i = find(m);
    if (i < 0) {
      i = 0;
      for (int j = 0; j < N; j++) {
        if (!used[j]) { i = j; break; }
        if ((uint32_t)(now - at[j]) > (uint32_t)(now - at[i])) i = j;
      }
      memcpy(mac[i], m, 6);
      used[i] = true;
    }
    at[i] = now;
  }

  void forget(const uint8_t* m) {
    int i = find(m);
    if (i >= 0) used[i] = false;
  }

  bool has(const uint8_t* m, uint32_t now, uint32_t timeoutMs) {
    int i = find(m);
    if (i < 0) return false;
    if ((uint32_t)(now - at[i]) > timeoutMs) {
      used[i] = false;
      return false;
    }
    return true;
  }
};

}  // namespace mesh_admit
