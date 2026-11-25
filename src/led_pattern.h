#pragma once

// Nowde 3 LED + button vocabulary (nowde#t-050 ③, proposal §7) as pure functions: no Arduino, no
// state of their own, so tools/led_pattern_test.cpp runs them on the host (tools/selftest.py).
//
//  ledLook(in)            what the node is doing -> one colour and one pattern, by the §7 table
//  ledFrame(look, t, b)   the pattern at time t, scaled by the brightness b (0..127)
//  identifyOver(in)       an identify has shown its strobe and its hop count
//  buttonStep(s, down, t) one 10 ms poll of the button -> identify / pin toggle / clear NVS
//
// The §7 table, highest priority first:
//   identify (host IDENTIFY or a short press)  white     strobe 3 s, then the hop count in blinks
//   radio watchdog / error                     red       flashes = cause code
//   OTA                                        purple    pulse
//   boot                                       white     one flash (the caller holds `booting`
//                                                        for BOOT_FLASH_MS)
//   -- show-dark: everything below is OFF --
//   pin conflict                               blue/red  alternate
//   sender, host driving                       blue      solid
//   standby (host driving, outranked)          blue      slow blink
//   no layer assigned (`-`)                    dim cyan  solid
//   link lost, freewheeling (<= 180 s)         amber     fast blink
//   alone (no master heard, or not locked yet) dim white slow breathe
//   slave, clock coarse (disagreement)         green     double blink
//   slave, locked                              green     solid
//
// Show-dark keeps errors (the §7 rule) and an identify: a person asked to see this node, for 3 s.

#include <stdint.h>

namespace led {

struct Rgb {
  uint8_t r, g, b;
  bool operator==(const Rgb& o) const { return r == o.r && g == o.g && b == o.b; }
};

enum class Pattern : uint8_t {
  Off,
  Solid,
  SlowBlink,    // 1 s period, half on
  FastBlink,    // 250 ms period, half on
  DoubleBlink,  // two 100 ms flashes, then dark to 1.2 s
  Breathe,      // 3 s triangle, never below a tenth
  Alternate,    // `color` then `alt`, 500 ms each
  Pulse,        // 1 s triangle
  Strobe,       // 100 ms period, half on
  Count,        // `count` flashes of 200 ms on / 200 ms off, then 1 s dark; repeats
  CountOnce,    // the same `count` flashes from `t0`, once, then off
};

enum class Role : uint8_t { Slave, Sender, Standby };

struct Look {
  Rgb color;
  Rgb alt;          // Alternate only
  Pattern pattern;
  uint8_t count;    // Count / CountOnce
  uint32_t t0;      // CountOnce: when its first flash starts
};

struct Inputs {
  uint32_t now;            // ms
  bool booting;
  bool ota;
  uint8_t errorCode;       // 0 = none; else the cause, shown as that many red flashes
  bool identify;           // an identify is running, since identifyStart
  uint32_t identifyStart;
  uint8_t hops;            // hops to the sender, shown after the identify strobe (0 = none)
  bool dark;               // show-dark flag (NVS)
  bool pinConflict;        // two pinned masters heard (the election, ④)
  Role role;
  bool layerAssigned;      // false: the node sits on the unassigned layer (`-`)
  bool linkLost;           // was following a master, lost it, freewheeling
  bool masterHeard;        // a master's beacon within the timeout
  bool locked;             // the mesh clock is synced to it
  bool coarse;             // ... but disagrees past the fine threshold
};

static const uint32_t BOOT_FLASH_MS = 250;
static const uint32_t IDENTIFY_STROBE_MS = 3000;
static const uint32_t COUNT_STEP_MS = 400;     // one flash + its gap
static const uint32_t COUNT_REST_MS = 1000;
static const uint8_t DEFAULT_BRIGHTNESS = 16;  // of 127: low by default (§7)

static const Rgb WHITE = {255, 255, 255};
static const Rgb DIM_WHITE = {60, 60, 60};
static const Rgb RED = {255, 0, 0};
static const Rgb PURPLE = {160, 0, 255};
static const Rgb BLUE = {0, 0, 255};
static const Rgb DIM_CYAN = {0, 60, 60};
static const Rgb AMBER = {255, 110, 0};
static const Rgb GREEN = {0, 255, 0};
static const Rgb BLACK = {0, 0, 0};

inline Look look(Rgb c, Pattern p, uint8_t count = 0, uint32_t t0 = 0) {
  Look l;
  l.color = c;
  l.alt = BLACK;
  l.pattern = p;
  l.count = count;
  l.t0 = t0;
  return l;
}

inline Look ledLook(const Inputs& in) {
  if (in.identify) {
    uint32_t since = in.now - in.identifyStart;
    if (since < IDENTIFY_STROBE_MS) return look(WHITE, Pattern::Strobe);
    return look(WHITE, Pattern::CountOnce, in.hops, in.identifyStart + IDENTIFY_STROBE_MS);
  }
  if (in.errorCode) return look(RED, Pattern::Count, in.errorCode);
  if (in.ota) return look(PURPLE, Pattern::Pulse);
  if (in.booting) return look(WHITE, Pattern::Solid);
  if (in.dark) return look(BLACK, Pattern::Off);
  if (in.pinConflict) {
    Look l = look(BLUE, Pattern::Alternate);
    l.alt = RED;
    return l;
  }
  if (in.role == Role::Sender) return look(BLUE, Pattern::Solid);
  if (in.role == Role::Standby) return look(BLUE, Pattern::SlowBlink);
  if (!in.layerAssigned) return look(DIM_CYAN, Pattern::Solid);
  if (in.linkLost) return look(AMBER, Pattern::FastBlink);
  if (!in.masterHeard || !in.locked) return look(DIM_WHITE, Pattern::Breathe);
  if (in.coarse) return look(GREEN, Pattern::DoubleBlink);
  return look(GREEN, Pattern::Solid);
}

// An identify is over once its strobe and its hop count have shown (plus one gap).
inline bool identifyOver(const Inputs& in) {
  if (!in.identify) return true;
  return in.now - in.identifyStart >= IDENTIFY_STROBE_MS + uint32_t(in.hops) * COUNT_STEP_MS;
}

inline uint8_t scale(uint8_t v, uint32_t num, uint32_t den) {
  return uint8_t((uint32_t(v) * num) / den);
}

inline Rgb dimmed(Rgb c, uint32_t num, uint32_t den) {
  Rgb o = {scale(c.r, num, den), scale(c.g, num, den), scale(c.b, num, den)};
  return o;
}

// triangle 0..1000..0 over `period`
inline uint32_t triangle(uint32_t t, uint32_t period) {
  uint32_t p = t % period;
  uint32_t half = period / 2;
  return p < half ? (p * 1000) / half : ((period - p) * 1000) / half;
}

// The pattern at time t (ms), at brightness 0..127 (127 = the colour as written).
inline Rgb ledFrame(const Look& l, uint32_t t, uint8_t brightness) {
  if (brightness > 127) brightness = 127;
  Rgb c = l.color;
  bool on = true;
  switch (l.pattern) {
    case Pattern::Off: on = false; break;
    case Pattern::Solid: break;
    case Pattern::SlowBlink: on = (t % 1000) < 500; break;
    case Pattern::FastBlink: on = (t % 250) < 125; break;
    case Pattern::DoubleBlink: {
      uint32_t p = t % 1200;
      on = p < 100 || (p >= 200 && p < 300);
      break;
    }
    case Pattern::Breathe: {
      uint32_t k = 100 + (triangle(t, 3000) * 900) / 1000;
      c = dimmed(c, k, 1000);
      break;
    }
    case Pattern::Alternate:
      if ((t % 1000) >= 500) c = l.alt;
      break;
    case Pattern::Pulse:
      c = dimmed(c, triangle(t, 1000), 1000);
      break;
    case Pattern::Strobe: on = (t % 100) < 50; break;
    case Pattern::Count: {
      uint32_t period = uint32_t(l.count) * COUNT_STEP_MS + COUNT_REST_MS;
      uint32_t p = t % period;
      on = p < uint32_t(l.count) * COUNT_STEP_MS && (p % COUNT_STEP_MS) < COUNT_STEP_MS / 2;
      break;
    }
    case Pattern::CountOnce: {
      if (t < l.t0) { on = false; break; }
      uint32_t p = t - l.t0;
      on = p < uint32_t(l.count) * COUNT_STEP_MS && (p % COUNT_STEP_MS) < COUNT_STEP_MS / 2;
      break;
    }
  }
  if (!on) return BLACK;
  return dimmed(c, brightness, 127);
}

// ---- button: BOOT (GPIO0) on the DevKits, BtnA on the Atoms (proposal §7, D5) ----
//   short press (released before 3 s)  identify
//   3 s hold (released before 10 s)    pin or unpin this node as master
//   10 s hold                          clear NVS and reboot -- fires while still held
// A press shorter than BUTTON_DEBOUNCE_MS is a bounce, not a press.

enum class ButtonEvent : uint8_t { None, Identify, PinToggle, ClearNvs };

struct ButtonState {
  bool down;
  bool cleared;     // the 10 s hold fired in this press
  uint32_t since;
};

static const uint32_t BUTTON_DEBOUNCE_MS = 30;
static const uint32_t BUTTON_PIN_MS = 3000;
static const uint32_t BUTTON_CLEAR_MS = 10000;

inline ButtonEvent buttonStep(ButtonState& s, bool pressed, uint32_t now) {
  if (pressed) {
    if (!s.down) {
      s.down = true;
      s.cleared = false;
      s.since = now;
      return ButtonEvent::None;
    }
    if (!s.cleared && now - s.since >= BUTTON_CLEAR_MS) {
      s.cleared = true;
      return ButtonEvent::ClearNvs;
    }
    return ButtonEvent::None;
  }
  if (!s.down) return ButtonEvent::None;
  s.down = false;
  uint32_t held = now - s.since;
  if (s.cleared || held < BUTTON_DEBOUNCE_MS) return ButtonEvent::None;
  if (held >= BUTTON_CLEAR_MS) return ButtonEvent::ClearNvs;   // a poll gap hid the 10 s mark
  if (held >= BUTTON_PIN_MS) return ButtonEvent::PinToggle;
  return ButtonEvent::Identify;
}

}  // namespace led
