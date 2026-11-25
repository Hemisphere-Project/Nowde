// Nowde 3 election (nowde#t-050 ④, proposal §5). See election.h; pure, host-tested by
// tools/election_test.cpp.
#include "election.h"

#include <string.h>

namespace election {

static bool after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static bool within(uint32_t now, uint32_t t, uint32_t ms) { return (uint32_t)(now - t) < ms; }

bool sameMac(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

bool better(const Rank& a, const Rank& b) {
  if (a.pin != b.pin) return a.pin > b.pin;
  if (!a.pin) {  // two pins compare by MAC alone (§5.2)
    if (a.term != b.term) return a.term > b.term;
    if (a.sub != b.sub) return a.sub > b.sub;
  }
  return memcmp(a.mac, b.mac, 6) < 0;
}

size_t encodeTrailer(const Rank& r, uint16_t meshId, uint8_t* out) {
  out[0] = BEACON_GEN;
  out[1] = r.pin ? 1 : 0;
  out[2] = (uint8_t)(r.term & 0xFF);
  out[3] = (uint8_t)(r.term >> 8);
  out[4] = r.sub;
  out[5] = (uint8_t)(meshId & 0xFF);
  out[6] = (uint8_t)(meshId >> 8);
  return BEACON_TRAILER_LEN;
}

bool decodeBeacon(const uint8_t* data, size_t len, const uint8_t mac[6], Rank* rank,
                  uint16_t* meshId) {
  // Read by length (§Wire contract): a later gen may append; anything shorter is legacy.
  if (len < 1 + BEACON_TRAILER_LEN || data[1] < BEACON_GEN) return false;
  rank->pin = data[2] ? 1 : 0;
  rank->term = (uint16_t)(data[3] | (data[4] << 8));
  rank->sub = data[5];
  memcpy(rank->mac, mac, 6);
  if (meshId) *meshId = (uint16_t)(data[6] | (data[7] << 8));
  return true;
}

void Election::begin(const uint8_t mac[6], Mode mode, uint32_t now) {
  *this = Election();
  memcpy(mine_.mac, mac, 6);
  setMode(mode, now);
  roleChanged_ = false;  // the boot HELLO already says it
}

void Election::setMode(Mode mode, uint32_t now) {
  mode_ = mode;
  mine_.pin = mode == Mode::Pin ? 1 : 0;
  tick(now);
}

bool Election::hostDriving(uint32_t now) const {
  return hostSeen_ && within(now, lastHost_, HOST_GONE_MS);
}

bool Election::senderLive(uint32_t now) const {
  return senderKnown_ && within(now, senderLastHeard_, SENDER_STALE_MS);
}

bool Election::pinConflict(uint32_t now) const {
  return mode_ == Mode::Pin && pinRivalSeen_ &&
         within(now, pinRivalLastHeard_, PIN_CONFLICT_HOLD_MS);
}

bool Election::takeRoleChange() {
  bool c = roleChanged_;
  roleChanged_ = false;
  return c;
}

void Election::setRole(Role r) {
  if (r == role_) return;
  role_ = r;
  roleChanged_ = true;
}

void Election::noteTerm(uint16_t t) {
  if (t > highestTerm_) highestTerm_ = t;
}

void Election::hostMediaSync(uint32_t now) {
  bool gap = !hostDriving(now);
  if (gap) {
    hostClaim(now);
    return;
  }
  lastHost_ = now;
  tick(now);
}

void Election::hostClaim(uint32_t now) {
  hostSeen_ = true;
  lastHost_ = now;
  if (mode_ == Mode::Never) return;
  if (senderLive(now) && senderExternal_) {  // never compete with a legacy master
    if (role_ == Role::Slave) setRole(Role::Standby);
    tick(now);
    return;
  }
  if (highestTerm_ < 0xFFFF) highestTerm_++;
  mine_.term = highestTerm_;
  mine_.sub = 0;
  setRole(Role::Sender);
  tick(now);  // a pinned rival still outranks a claim
}

void Election::beacon(uint32_t now, const uint8_t mac[6], const Rank* rank) {
  if (sameMac(mac, mine_.mac)) return;  // my own frame: never a rival
  if (rank) noteTerm(rank->term);
  if (mode_ == Mode::Pin && (!rank || rank->pin)) {
    pinRivalSeen_ = true;
    pinRivalLastHeard_ = now;
  }

  bool take;
  if (!senderLive(now) || sameMac(mac, sender_.mac)) {
    take = true;
  } else if (senderExternal_) {
    take = false;  // the first legacy master holds
  } else {
    take = !rank || better(*rank, sender_);
  }
  if (take) {
    if (senderKnown_ && !sameMac(mac, sender_.mac) && senderLive(now)) {
      otherKnown_ = true;  // the sender it displaces is still a node we hear
      otherLastHeard_ = now;
    }
    senderKnown_ = true;
    senderExternal_ = rank == nullptr;
    if (rank) sender_ = *rank;
    memcpy(sender_.mac, mac, 6);
    senderLastHeard_ = now;
  } else {
    otherKnown_ = true;  // a worse sender, about to yield: company
    otherLastHeard_ = now;
  }
  tick(now);
}

void Election::heard(uint32_t now, const uint8_t mac[6]) {
  if (sameMac(mac, mine_.mac)) return;  // my own frame: never company
  otherKnown_ = true;
  otherLastHeard_ = now;
}

void Election::tick(uint32_t now) {
  if (mode_ == Mode::Never) {
    setRole(Role::Slave);
    return;
  }
  bool driving = hostDriving(now);
  bool live = senderLive(now);
  bool outranked = live && (senderExternal_ || better(sender_, mine_));

  if (role_ == Role::Sender) {
    if (outranked) setRole(driving ? Role::Standby : Role::Slave);
    else if (mode_ == Mode::Auto && !driving) setRole(Role::Slave);
    return;
  }
  if (mode_ == Mode::Pin && !outranked) {  // the designated master
    setRole(Role::Sender);
    return;
  }
  if (role_ != Role::Standby) return;
  if (!driving) {
    setRole(Role::Slave);
    return;
  }
  // Standby takeover: the sender silent for TAKEOVER_MS, and company since it went quiet.
  if (senderKnown_ && !live && !within(now, senderLastHeard_, TAKEOVER_MS) && otherKnown_ &&
      after(otherLastHeard_, senderLastHeard_)) {
    mine_.term = senderExternal_ ? highestTerm_ : sender_.term;
    mine_.sub = (uint8_t)((senderExternal_ ? 0 : sender_.sub) + 1);
    senderKnown_ = false;
    setRole(Role::Sender);
  }
}

}  // namespace election
