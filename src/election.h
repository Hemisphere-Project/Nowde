#pragma once

// Nowde 3 election (nowde#t-050 ④, proposal §5): who is the media sender. Pure C++ -- no Arduino,
// time passed in -- so tools/election_test.cpp runs it on the host (tools/selftest.py).
//
// The host decides, the rank arbitrates:
//  - a node becomes the sender on a HOST CLAIM (CLAIM 0x0F, QUERY_CONFIG, PUSH_FULL_CONFIG, the
//    first MEDIA_SYNC after >= 5 s without one): term = highest term heard + 1 (Lamport);
//  - rank = (pin, term, sub, MAC), compared in that order, the lower MAC winning a tie; two pins
//    compare by MAC alone and both flag the conflict;
//  - a sender hearing a better rank yields on that beacon (within one beacon period): standby
//    while its host still drives, and an unsolicited HELLO (takeRoleChange());
//  - a standby re-claims only on a new claim event, never on every MEDIA_SYNC;
//  - standby takeover: the sender silent for TAKEOVER_MS, this node's host still driving, and
//    COMPANY (another node heard after the sender went quiet: hplayer2#t-089's rule, so a node cut
//    off from everyone never claims) -> (pin, T, sub + 1, me);
//  - a beacon without a rank (1 byte: v1.2 / 2.x) is an external pinned master: followed, never
//    competed with;
//  - an auto sender whose host has gone quiet for HOST_GONE_MS stops sending (no host-less media);
//    a pinned one is the designated master and keeps beaconing.
//
// The peer table is the real one: every frame carries the MAC it came from, and a node's OWN MAC
// never counts as a rival nor as company (Anna's self-match, hplayer2 notes
// 2026-11-06-t-086-recheck-e7d48a6).

#include <stddef.h>
#include <stdint.h>

namespace election {

static const uint32_t HOST_GONE_MS = 5000;   // a MEDIA_SYNC after this gap is a host claim
static const uint32_t TAKEOVER_MS = 3000;    // sender silence before a standby takes over
static const uint32_t SENDER_STALE_MS = 3000;  // a sender not heard for this long is quiet
static const uint32_t PIN_CONFLICT_HOLD_MS = 30000;  // a pinned rival heard this recently

// SET_ROLE 0x08 values.
enum class Mode : uint8_t { Never = 0x00, Pin = 0x01, Auto = 0x7F };
// HELLO role values (§Wire contract): 0 slave, 1 master, 3 standby.
enum class Role : uint8_t { Slave = 0, Sender = 1, Standby = 3 };

struct Rank {
  uint8_t pin;
  uint16_t term;
  uint8_t sub;
  uint8_t mac[6];
};

// a outranks b.
bool better(const Rank& a, const Rank& b);
bool sameMac(const uint8_t a[6], const uint8_t b[6]);

// Beacon 0x01 trailer (§Wire contract): gen (=3), pin, term u16 LE, sub, meshId u16 LE.
static const uint8_t BEACON_GEN = 3;
static const size_t BEACON_TRAILER_LEN = 7;
// Writes the trailer after the type byte; returns the bytes written (BEACON_TRAILER_LEN).
size_t encodeTrailer(const Rank& r, uint16_t meshId, uint8_t* out);
// A received beacon (type byte included). Returns false for a legacy beacon (no v3 trailer):
// an external pinned master. `mac` is the ESP-NOW sender's.
bool decodeBeacon(const uint8_t* data, size_t len, const uint8_t mac[6], Rank* rank,
                  uint16_t* meshId);

class Election {
 public:
  void begin(const uint8_t mac[6], Mode mode, uint32_t now);
  void setMode(Mode mode, uint32_t now);

  // Host side.
  void hostClaim(uint32_t now);      // CLAIM, QUERY_CONFIG, PUSH_FULL_CONFIG, transport-in
  void hostMediaSync(uint32_t now);  // every host MEDIA_SYNC; claims after HOST_GONE_MS without

  // Radio side: a master beacon (rank == nullptr: legacy, an external pin), and any frame at all.
  void beacon(uint32_t now, const uint8_t mac[6], const Rank* rank);
  void heard(uint32_t now, const uint8_t mac[6]);

  void tick(uint32_t now);

  Role role() const { return role_; }
  Mode mode() const { return mode_; }
  const Rank& rank() const { return mine_; }
  bool sending() const { return role_ == Role::Sender; }
  bool pinConflict(uint32_t now) const;
  bool hostDriving(uint32_t now) const;
  uint16_t highestTerm() const { return highestTerm_; }
  // True once per role change: push an unsolicited HELLO.
  bool takeRoleChange();

 private:
  void setRole(Role r);
  bool senderLive(uint32_t now) const;
  void noteTerm(uint16_t t);

  Rank mine_ = Rank();
  Mode mode_ = Mode::Auto;
  Role role_ = Role::Slave;
  bool roleChanged_ = false;
  uint16_t highestTerm_ = 0;

  bool hostSeen_ = false;
  uint32_t lastHost_ = 0;

  // The best sender heard (not me): its rank, or external (legacy) with no rank.
  bool senderKnown_ = false;
  bool senderExternal_ = false;
  Rank sender_ = Rank();
  uint32_t senderLastHeard_ = 0;

  bool otherKnown_ = false;       // a frame from a node that is neither me nor the sender
  uint32_t otherLastHeard_ = 0;

  bool pinRivalSeen_ = false;
  uint32_t pinRivalLastHeard_ = 0;
};

}  // namespace election
