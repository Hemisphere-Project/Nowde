// Host test of the Nowde 3 election (nowde#t-050 ④, proposal §5) -- src/election.{h,cpp}.
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. It pins:
//  - the rank order (pin, term, sub, lower MAC), two pins by MAC alone;
//  - the beacon trailer's bytes both ways; a 1-byte beacon is legacy;
//  - on a simulated mesh whose every frame carries its sender's MAC and reaches its sender too (the
//    real peer table: Anna's self-match hid behind a desk fake): the last host to claim leads, the
//    one it displaces yields within one beacon period with one role change (= one HELLO), no
//    re-claim on MEDIA_SYNC, an explicit CLAIM wins back, the 3 s standby takeover with company and
//    none without (own frames are not company), two pins -> lower MAC + both flag the conflict, a
//    legacy beacon is followed and never competed with, never-sender, an auto sender whose host
//    stops goes quiet, and the millis() wrap.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/election.h"

using namespace election;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static Rank rank(uint8_t pin, uint16_t term, uint8_t sub, uint8_t macLast) {
  Rank r = Rank();
  r.pin = pin;
  r.term = term;
  r.sub = sub;
  r.mac[0] = 0x24;
  r.mac[5] = macLast;
  return r;
}

struct Node {
  Election e;
  uint8_t mac[6];
  bool hostOn = false;  // its host streams MEDIA_SYNC
  bool radioOn = true;  // cut off: hears nothing, heard by nobody
  bool legacy = false;  // a 1.2 / 2.x master: 1-byte beacon, no election
  uint32_t phase = 0;   // its beacon's offset in the second, in 100 ms steps
  uint32_t changes = 0;
};

struct Mesh {
  std::vector<Node> nodes;
  uint32_t now;
  uint32_t steps = 0;

  explicit Mesh(uint32_t t0) : now(t0) {}

  size_t add(uint8_t macLast, Mode mode) {
    Node n;
    for (int i = 0; i < 6; i++) n.mac[i] = 0;
    n.mac[0] = 0x24;
    n.mac[5] = macLast;
    nodes.push_back(n);
    nodes.back().e.begin(nodes.back().mac, mode, now);
    return nodes.size() - 1;
  }

  // One 100 ms step: hosts sync, then on each whole second every node beacons (a sender its rank,
  // a legacy master one byte) or sends a receiver frame -- delivered to EVERY node, itself included.
  void step() {
    now += 100;
    for (Node& n : nodes)
      if (n.hostOn && !n.legacy) n.e.hostMediaSync(now);
    steps++;
    for (Node& src : nodes) {
      if ((steps + src.phase) % 10 != 0 || !src.radioOn) continue;
      bool sends = src.legacy || src.e.sending();
      Rank r = src.e.rank();
      uint8_t frame[1 + BEACON_TRAILER_LEN] = {0x01};
      size_t len = src.legacy ? 1 : 1 + encodeTrailer(r, 0, frame + 1);
      for (Node& dst : nodes) {
        if (!dst.radioOn || dst.legacy) continue;
        if (sends) {
          Rank got;
          bool v3 = decodeBeacon(frame, len, src.mac, &got, nullptr);
          dst.e.beacon(now, src.mac, v3 ? &got : nullptr);
        } else {
          dst.e.heard(now, src.mac);
        }
      }
    }
    for (Node& n : nodes) {
      if (n.legacy) continue;
      n.e.tick(now);
      if (n.e.takeRoleChange()) n.changes++;
    }
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) step();
  }
  Role role(size_t i) const { return nodes[i].e.role(); }
  size_t senders() const {
    size_t s = 0;
    for (const Node& n : nodes) s += n.legacy || n.e.sending();
    return s;
  }
};

static void rankOrder() {
  check(better(rank(1, 0, 0, 9), rank(0, 50, 9, 1)), "a pin beats any term");
  check(better(rank(0, 3, 0, 9), rank(0, 2, 9, 1)), "a higher term beats sub and MAC");
  check(better(rank(0, 3, 2, 9), rank(0, 3, 1, 1)), "same term: a higher sub wins");
  check(better(rank(0, 3, 1, 1), rank(0, 3, 1, 9)), "a tie: the lower MAC wins");
  check(!better(rank(0, 3, 1, 9), rank(0, 3, 1, 9)), "a rank never beats itself");
  check(better(rank(1, 0, 0, 1), rank(1, 40, 3, 9)), "two pins: the lower MAC, whatever the term");
}

static void trailer() {
  uint8_t frame[1 + BEACON_TRAILER_LEN] = {0x01};
  Rank r = rank(1, 0x1234, 7, 5);
  check(encodeTrailer(r, 0xBEEF, frame + 1) == 7, "the trailer is 7 bytes");
  const uint8_t want[] = {0x01, 3, 1, 0x34, 0x12, 7, 0xEF, 0xBE};
  for (int i = 0; i < 8; i++) check(frame[i] == want[i], "trailer bytes: gen pin term(LE) sub mesh(LE)");
  Rank got;
  uint16_t mesh = 0;
  check(decodeBeacon(frame, sizeof frame, r.mac, &got, &mesh), "a v3 beacon decodes");
  check(got.pin == 1 && got.term == 0x1234 && got.sub == 7 && mesh == 0xBEEF && sameMac(got.mac, r.mac),
        "and decodes back");
  check(!decodeBeacon(frame, 1, r.mac, &got, &mesh), "a 1-byte beacon is legacy");
  frame[1] = 2;
  check(!decodeBeacon(frame, sizeof frame, r.mac, &got, &mesh), "a trailer below gen 3 is legacy");
  uint8_t longer[12] = {0x01, 4, 0, 1, 0, 0, 0, 0, 9, 9, 9, 9};
  check(decodeBeacon(longer, sizeof longer, r.mac, &got, &mesh) && got.term == 1,
        "a later gen's longer trailer reads by length");
}

static void lastClaimLeads(uint32_t t0) {
  Mesh m(t0);
  size_t a = m.add(0x0A, Mode::Auto), b = m.add(0x0B, Mode::Auto), c = m.add(0x0C, Mode::Auto);
  m.run(3000);
  check(m.senders() == 0, "no host, no sender");
  m.nodes[a].hostOn = true;
  m.run(2000);
  check(m.role(a) == Role::Sender && m.nodes[a].e.rank().term == 1, "A's host starts: A sends, term 1");
  check(m.role(b) == Role::Slave && m.role(c) == Role::Slave, "B and C follow");
  check(m.nodes[a].changes == 1, "one role change on A: one HELLO");
  m.nodes[b].hostOn = true;
  m.step();
  check(m.role(b) == Role::Sender && m.nodes[b].e.rank().term == 2, "B's host starts later: term 2");
  m.run(1000);
  check(m.role(a) == Role::Standby, "A yields within one beacon period");
  check(m.nodes[a].changes == 2, "A's yield is one more HELLO");
  m.run(20000);
  check(m.role(a) == Role::Standby && m.senders() == 1,
        "A's host keeps streaming: no re-claim on MEDIA_SYNC");
  check(m.nodes[a].changes == 2 && m.nodes[b].changes == 1, "and no flapping");
  m.nodes[a].e.hostClaim(m.now);
  check(m.role(a) == Role::Sender && m.nodes[a].e.rank().term == 3, "an explicit CLAIM: term 3");
  m.run(1000);
  check(m.role(b) == Role::Standby && m.senders() == 1, "B yields to it");

  // A's host stops: the auto sender goes quiet, B (standby, host driving, C heard) takes over.
  m.nodes[a].hostOn = false;
  m.run(HOST_GONE_MS);
  check(m.role(a) == Role::Slave, "A's host gone 5 s: A stops sending");
  m.run(TAKEOVER_MS + 1000);
  check(m.role(b) == Role::Sender, "B takes over within 3 s of silence");
  check(m.nodes[b].e.rank().term == 3 && m.nodes[b].e.rank().sub == 1, "as (pin, T, sub + 1)");
  m.nodes[a].hostOn = true;  // A's host back after > 5 s: a claim, which outranks the takeover
  m.run(2000);
  check(m.role(a) == Role::Sender && m.role(b) == Role::Standby && m.nodes[a].e.rank().term == 4,
        "a real claim outranks any takeover");
}

static void takeoverNeedsCompany(uint32_t t0) {
  // Two nodes, the sender cut off: the standby hears nobody but itself -> no claim.
  Mesh m(t0);
  size_t a = m.add(0x0A, Mode::Auto), b = m.add(0x0B, Mode::Auto);
  m.nodes[a].hostOn = true;
  m.run(2000);
  m.nodes[b].hostOn = true;
  m.run(2000);
  check(m.role(b) == Role::Sender && m.role(a) == Role::Standby, "B claims last, A standby");
  m.nodes[b].radioOn = false;
  m.run(10000);
  check(m.role(a) == Role::Standby, "sender gone, nobody else heard: no takeover (own frames are not company)");
  // Its own echo is all A hears; a third node appearing is company.
  size_t c = m.add(0x0C, Mode::Auto);
  m.run(TAKEOVER_MS);
  check(m.role(a) == Role::Sender && m.role(c) == Role::Slave, "company after the silence: A takes over");

  // The two-laptop case: B's host stops, B is still on the radio as a slave -> company.
  Mesh n(t0);
  size_t x = n.add(0x0A, Mode::Auto), y = n.add(0x0B, Mode::Auto);
  n.nodes[x].hostOn = true;
  n.run(2000);
  n.nodes[y].hostOn = true;
  n.run(2000);
  n.nodes[y].hostOn = false;
  n.run(HOST_GONE_MS + 900);
  check(n.role(y) == Role::Slave, "B's host stopped: B is a slave");
  check(n.role(x) == Role::Standby, "A has not taken over yet (< 3 s of silence)");
  n.run(TAKEOVER_MS + 1000);
  check(n.role(x) == Role::Sender, "two laptops: A takes over, B's slave frames are company");

  // No host on the standby: no takeover.
  Mesh p(t0);
  size_t s = p.add(0x0A, Mode::Auto), t = p.add(0x0B, Mode::Auto);
  size_t u = p.add(0x0C, Mode::Auto);
  p.nodes[s].hostOn = true;
  p.run(2000);
  p.nodes[t].hostOn = true;
  p.run(2000);
  p.nodes[s].hostOn = false;
  p.run(HOST_GONE_MS);
  check(p.role(s) == Role::Slave, "a standby whose host stops is a slave");
  p.nodes[t].radioOn = false;
  p.run(10000);
  check(p.role(s) == Role::Slave && p.role(u) == Role::Slave,
        "the sender cut off, no other host driving: no host-less sender");
}

static void pins(uint32_t t0) {
  Mesh m(t0);
  size_t a = m.add(0x0A, Mode::Pin), b = m.add(0x0B, Mode::Pin), c = m.add(0x0C, Mode::Auto);
  check(m.role(a) == Role::Sender && m.role(b) == Role::Sender, "a pin sends from boot, host or not");
  m.nodes[b].hostOn = true;
  m.run(2000);
  check(m.role(a) == Role::Sender && m.role(b) == Role::Standby, "two pins: the lower MAC wins, host or term");
  check(m.nodes[b].e.pinConflict(m.now), "the yielding pin flags the conflict");
  check(!m.nodes[a].e.pinConflict(m.now),
        "the winner beaconed first and never heard the other pin: it cannot know (said in the commit)");
  check(!m.nodes[c].e.pinConflict(m.now), "an auto node never flags it");
  m.nodes[c].hostOn = true;
  m.run(2000);
  check(m.role(c) == Role::Standby && m.senders() == 1, "a host claim never beats a pin");
  m.nodes[a].radioOn = false;
  m.run(TAKEOVER_MS + 1000);
  check(m.role(b) == Role::Sender && m.role(c) == Role::Standby, "the winning pin gone: the other pin sends");
  m.run(PIN_CONFLICT_HOLD_MS);
  check(!m.nodes[b].e.pinConflict(m.now), "the conflict clears once the rival pin is gone");

  // The other pin beacons first: the winner hears it before it yields, and flags it too.
  Mesh q(t0);
  size_t qa = q.add(0x0A, Mode::Pin), qb = q.add(0x0B, Mode::Pin);
  q.nodes[qb].phase = 5;
  q.run(2000);
  check(q.role(qa) == Role::Sender && q.role(qb) == Role::Slave, "the lower MAC still wins (B has no host: slave)");
  check(q.nodes[qa].e.pinConflict(q.now) && q.nodes[qb].e.pinConflict(q.now), "both flag the conflict");

  // SET_ROLE live: auto -> pin -> never.
  Mesh n(t0);
  size_t x = n.add(0x0A, Mode::Auto), y = n.add(0x0B, Mode::Auto);
  n.nodes[x].hostOn = true;
  n.run(2000);
  n.nodes[y].e.setMode(Mode::Pin, n.now);
  n.run(1000);
  check(n.role(y) == Role::Sender && n.role(x) == Role::Standby, "pinning a node makes it the sender");
  n.nodes[y].e.setMode(Mode::Never, n.now);
  check(n.role(y) == Role::Slave, "never-sender: slave at once");
  n.nodes[y].hostOn = true;
  n.nodes[y].e.hostClaim(n.now);
  n.run(TAKEOVER_MS + 2000);
  check(n.role(y) == Role::Slave, "never-sender ignores its host's claims");
  check(n.role(x) == Role::Sender, "and the standby takes over from it");
}

static void legacy(uint32_t t0) {
  Mesh m(t0);
  size_t l = m.add(0x01, Mode::Auto);
  m.nodes[l].legacy = true;  // AnnaTV's v1.2 / the garden's 2.0.5: 1-byte beacon
  size_t a = m.add(0x0A, Mode::Auto), b = m.add(0x0B, Mode::Pin);
  m.nodes[a].hostOn = true;
  m.run(3000);
  check(m.role(a) == Role::Standby, "a host claim never competes with a legacy master");
  check(m.role(b) == Role::Standby || m.role(b) == Role::Slave, "nor does a pin");
  check(m.nodes[b].e.pinConflict(m.now), "and the pin flags the conflict");
  check(m.senders() == 1, "the legacy master alone sends");
  m.nodes[a].e.hostClaim(m.now);
  m.run(1000);
  check(m.role(a) == Role::Standby, "nor does an explicit CLAIM");
}

int main() {
  rankOrder();
  trailer();
  lastClaimLeads(0);
  takeoverNeedsCompany(0);
  pins(0);
  legacy(0);
  // millis() wraps 49.7 days in: every scenario again across the wrap.
  uint32_t wrap = 0xFFFFFFFFu - 4999u - 999u + 1u;  // a whole second, 6 s before the wrap
  wrap -= wrap % 1000;
  lastClaimLeads(wrap);
  takeoverNeedsCompany(wrap);
  pins(wrap);
  legacy(wrap);
  std::printf("election_test: ok\n");
  return 0;
}
