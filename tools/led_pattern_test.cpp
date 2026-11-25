// Host test of the Nowde 3 LED + button vocabulary (nowde#t-050 ③, proposal §7) -- src/led_pattern.h.
// tools/selftest.py compiles and runs it when a host C++ compiler is on PATH. It pins:
//  - every row of the §7 table: the state it names gives its colour and its pattern;
//  - the priority between rows (identify > error > OTA > boot > show-dark > pin conflict > role >
//    layer > link lost > alone > coarse > locked), each row checked with every lower one also set;
//  - show-dark: off for every status row, errors and identify still shown;
//  - each pattern's timing, and the brightness scaling (low by default, 0 = off, capped at 127);
//  - identify: 3 s strobe, then exactly `hops` blinks once, then identifyOver();
//  - the button: bounce ignored, short = identify, 3 s = pin toggle on release, 10 s = clear NVS
//    while still held and nothing on that release.
#include <cstdio>
#include <cstdlib>

#include "../src/led_pattern.h"

using namespace led;

static void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    std::exit(1);
  }
}

static Inputs locked() {
  Inputs in = Inputs();
  in.now = 100000;
  in.role = Role::Slave;
  in.layerAssigned = true;
  in.masterHeard = true;
  in.locked = true;
  return in;
}

static bool is(const Look& l, Rgb c, Pattern p) { return l.color == c && l.pattern == p; }

// the frame is lit at time t
static bool lit(const Look& l, uint32_t t) { return !(ledFrame(l, t, 127) == BLACK); }

static int countFlashes(const Look& l, uint32_t from, uint32_t to) {
  int n = 0;
  bool was = false;
  for (uint32_t t = from; t < to; t += 10) {
    bool on = lit(l, t);
    if (on && !was) n++;
    was = on;
  }
  return n;
}

static void table() {
  Inputs in = locked();
  check(is(ledLook(in), GREEN, Pattern::Solid), "slave, locked: green solid");
  in.coarse = true;
  check(is(ledLook(in), GREEN, Pattern::DoubleBlink), "slave, coarse: green double blink");
  in.locked = false;
  check(is(ledLook(in), DIM_WHITE, Pattern::Breathe), "slave, not locked yet: dim white breathe");
  in = locked();
  in.masterHeard = false;
  check(is(ledLook(in), DIM_WHITE, Pattern::Breathe), "alone: dim white slow breathe");
  in.linkLost = true;
  check(is(ledLook(in), AMBER, Pattern::FastBlink), "link lost: amber fast blink");
  in.layerAssigned = false;
  check(is(ledLook(in), DIM_CYAN, Pattern::Solid), "no layer: dim cyan solid");
  in.role = Role::Standby;
  check(is(ledLook(in), BLUE, Pattern::SlowBlink), "standby: blue slow blink");
  in.role = Role::Sender;
  check(is(ledLook(in), BLUE, Pattern::Solid), "sender: blue solid");
  in.pinConflict = true;
  Look pc = ledLook(in);
  check(is(pc, BLUE, Pattern::Alternate) && pc.alt == RED, "pin conflict: blue/red alternate");
  in.dark = true;
  check(ledLook(in).pattern == Pattern::Off, "show-dark: off");
  in.booting = true;
  check(is(ledLook(in), WHITE, Pattern::Solid), "boot: white (held for one flash)");
  in.ota = true;
  check(is(ledLook(in), PURPLE, Pattern::Pulse), "OTA: purple pulse");
  in.errorCode = 3;
  Look er = ledLook(in);
  check(is(er, RED, Pattern::Count) && er.count == 3, "error: red, flashes = cause code");
  in.identify = true;
  in.identifyStart = in.now - 10;
  check(is(ledLook(in), WHITE, Pattern::Strobe), "identify: white strobe, above everything");
}

static void dark() {
  Role roles[] = {Role::Slave, Role::Sender, Role::Standby};
  for (int r = 0; r < 3; r++) {
    for (int bits = 0; bits < 64; bits++) {
      Inputs in = locked();
      in.dark = true;
      in.role = roles[r];
      in.pinConflict = bits & 1;
      in.layerAssigned = bits & 2;
      in.linkLost = bits & 4;
      in.masterHeard = bits & 8;
      in.locked = bits & 16;
      in.coarse = bits & 32;
      check(ledLook(in).pattern == Pattern::Off, "show-dark: every status row is off");
      in.errorCode = 2;
      check(is(ledLook(in), RED, Pattern::Count), "show-dark: an error still shows");
      in.errorCode = 0;
      in.identify = true;
      in.identifyStart = in.now;
      check(is(ledLook(in), WHITE, Pattern::Strobe), "show-dark: an identify still shows");
    }
  }
}

static void patterns() {
  Look s = look(GREEN, Pattern::Solid);
  check(ledFrame(s, 0, 127) == GREEN, "brightness 127 = the colour as written");
  check(ledFrame(s, 0, 200) == GREEN, "brightness capped at 127");
  check(ledFrame(s, 0, 0) == BLACK, "brightness 0 = off");
  Rgb low = ledFrame(s, 0, DEFAULT_BRIGHTNESS);
  check(low.g > 0 && low.g <= 40 && low.r == 0 && low.b == 0, "default brightness is low");
  check(ledFrame(look(GREEN, Pattern::Off), 0, 127) == BLACK, "off is off");

  Look sb = look(BLUE, Pattern::SlowBlink);
  check(lit(sb, 0) && lit(sb, 499) && !lit(sb, 500) && !lit(sb, 999) && lit(sb, 1000), "slow blink: 1 s");
  Look fb = look(AMBER, Pattern::FastBlink);
  check(lit(fb, 0) && !lit(fb, 125) && lit(fb, 250), "fast blink: 250 ms");
  check(countFlashes(look(GREEN, Pattern::DoubleBlink), 0, 1200) == 2, "double blink: 2 per 1.2 s");
  check(countFlashes(look(WHITE, Pattern::Strobe), 0, 1000) == 10, "strobe: 10 Hz");

  Look br = look(DIM_WHITE, Pattern::Breathe);
  Rgb lo = ledFrame(br, 0, 127), hi = ledFrame(br, 1500, 127);
  check(lo.r > 0 && lo.r < hi.r && hi == DIM_WHITE, "breathe: never dark, peaks at the colour");
  Look pu = look(PURPLE, Pattern::Pulse);
  check(ledFrame(pu, 0, 127) == BLACK && ledFrame(pu, 500, 127) == PURPLE, "pulse: 0 -> colour -> 0 in 1 s");

  Look al = look(BLUE, Pattern::Alternate);
  al.alt = RED;
  check(ledFrame(al, 0, 127) == BLUE && ledFrame(al, 600, 127) == RED, "alternate: blue then red");

  for (uint8_t n = 1; n <= 7; n++) {
    Look c = look(RED, Pattern::Count, n);
    uint32_t period = n * COUNT_STEP_MS + COUNT_REST_MS;
    check(countFlashes(c, 0, period) == n, "error code: n flashes per period");
    check(countFlashes(c, 0, 3 * period) == 3 * n, "error code: repeats");
  }
}

static void identify() {
  for (uint8_t hops = 0; hops <= 4; hops++) {
    Inputs in = locked();
    in.identify = true;
    in.identifyStart = 5000;
    in.hops = hops;
    int strobes = 0, blinks = 0;
    bool was = false;
    for (uint32_t t = 5000;; t += 10) {
      in.now = t;
      if (identifyOver(in)) break;
      Look l = ledLook(in);
      bool on = lit(l, t);
      if (on && !was) (l.pattern == Pattern::Strobe ? strobes : blinks)++;
      was = on;
      check(t - 5000 < 20000, "identify ends");
    }
    check(strobes == 30, "identify: 3 s of 10 Hz strobe");
    check(blinks == hops, "identify: then the hop count in blinks, once");
  }
  Inputs in = locked();
  check(identifyOver(in), "no identify = over");
}

static void button() {
  ButtonState s = ButtonState();
  uint32_t t = 1000;
  // a 20 ms bounce
  check(buttonStep(s, true, t) == ButtonEvent::None, "press");
  check(buttonStep(s, false, t + 20) == ButtonEvent::None, "bounce ignored");
  // short press
  buttonStep(s, true, t = 2000);
  for (uint32_t u = t; u < t + 200; u += 10) check(buttonStep(s, true, u) == ButtonEvent::None, "held");
  check(buttonStep(s, false, t + 200) == ButtonEvent::Identify, "short press = identify");
  // 2.99 s is still short, 3 s is the pin
  buttonStep(s, true, t = 5000);
  check(buttonStep(s, false, t + 2990) == ButtonEvent::Identify, "2.99 s = identify");
  buttonStep(s, true, t = 9000);
  for (uint32_t u = t; u <= t + 4000; u += 10) check(buttonStep(s, true, u) == ButtonEvent::None, "nothing while held < 10 s");
  check(buttonStep(s, false, t + 4000) == ButtonEvent::PinToggle, "3 s hold = pin toggle on release");
  // 10 s fires once while held, and the release does nothing more
  buttonStep(s, true, t = 20000);
  int clears = 0;
  for (uint32_t u = t; u <= t + 12000; u += 10) {
    ButtonEvent e = buttonStep(s, true, u);
    if (e == ButtonEvent::ClearNvs) {
      clears++;
      check(u - t >= BUTTON_CLEAR_MS && u - t < BUTTON_CLEAR_MS + 10, "clear at 10 s");
    } else {
      check(e == ButtonEvent::None, "nothing else while held");
    }
  }
  check(clears == 1, "10 s hold = clear NVS, once");
  check(buttonStep(s, false, t + 12000) == ButtonEvent::None, "release after a clear: nothing");
  // a poll gap across the 10 s mark still clears, on release
  buttonStep(s, true, t = 40000);
  check(buttonStep(s, false, t + 11000) == ButtonEvent::ClearNvs, "unpolled 10 s hold = clear");
  // millis() wrap inside a press
  buttonStep(s, true, t = 0xFFFFFF00u);
  check(buttonStep(s, false, t + 3500) == ButtonEvent::PinToggle, "pin across the millis() wrap");
}

int main() {
  table();
  dark();
  patterns();
  identify();
  button();
  std::printf("led_pattern_test: ok\n");
  return 0;
}
