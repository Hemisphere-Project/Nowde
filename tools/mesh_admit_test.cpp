// Host test of src/mesh_admit.h -- Nowde 3 admission, one mesh per show (#t-050 ⑤).
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. No framework: one
// check() that exits non-zero, like selftest.py's own.
#include <cstdio>
#include <cstdlib>

#include "../src/mesh_admit.h"

static int checks = 0;

static void check(bool cond, const char* what) {
  checks++;
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

int main() {
  using namespace mesh_admit;

  // The rule: drop only when both ids are non-zero and differ.
  check(admits(5, 5), "same mesh: heard");
  check(!admits(5, 6) && !admits(6, 5), "two configured meshes: neither hears the other");
  check(admits(OPEN, 6), "an open node hears a configured mesh");
  check(admits(5, OPEN), "a configured node hears an open sender");
  check(admits(OPEN, OPEN), "open hears open (every 2.x mesh today)");
  bool symmetric = true;
  for (uint16_t a = 0; a < 40; a++) {
    for (uint16_t b = 0; b < 40; b++) {
      symmetric = symmetric && admits(a, b) == admits(b, a);
    }
  }
  check(symmetric, "the rule is symmetric");
  check(admits(MAX_ID, MAX_ID) && !admits(MAX_ID, MAX_ID - 1), "the 14-bit top id");

  // SET_MESH's two 7-bit bytes, low first.
  check(fromSysEx(0x05, 0x00) == 5, "id 5");
  check(fromSysEx(0x7F, 0x7F) == MAX_ID, "id 0x3FFF");
  check(fromSysEx(0x00, 0x01) == 128, "id 128: the high byte");
  check(fromSysEx(0xFF, 0xFF) == MAX_ID, "bit 7 of each byte is ignored");

  // The foreign table: a MAC whose beacon named another mesh.
  const uint8_t A[6] = {1, 2, 3, 4, 5, 6};
  const uint8_t B[6] = {1, 2, 3, 4, 5, 7};
  const uint32_t T = 5000;
  ForeignTable<2> t;
  check(!t.has(A, 0, T), "empty table: nobody is foreign");
  t.note(A, 100);
  check(t.has(A, 100 + T, T), "foreign up to the timeout");
  check(!t.has(B, 200, T), "another MAC is not");
  check(!t.has(A, 101 + T, T), "forgotten past the timeout");
  check(!t.has(A, 102 + T, T), "and stays forgotten");
  t.note(A, 1000);
  t.note(A, 3000);
  check(t.has(A, 3000 + T, T), "each foreign beacon re-arms it");
  t.forget(A);
  check(!t.has(A, 3001, T), "an admitted beacon forgets it at once");

  // Full table: the stalest entry goes.
  const uint8_t C[6] = {9, 9, 9, 9, 9, 9};
  ForeignTable<2> f;
  f.note(A, 10);
  f.note(B, 20);
  f.note(C, 30);
  check(!f.has(A, 31, T) && f.has(B, 31, T) && f.has(C, 31, T), "full: the stalest is replaced");

  // Across the millis() wrap.
  ForeignTable<2> w;
  w.note(A, 0xFFFFFF00u);
  check(w.has(A, 0x00000100u, T), "foreign across the wrap");
  check(!w.has(A, 0xFFFFFF00u + T + 1, T), "times out across the wrap");

  std::printf("mesh_admit host test OK (%d checks)\n", checks);
  return 0;
}
