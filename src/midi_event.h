#pragma once

#include <Arduino.h>

// v2.3 MIDI_EVENT (0x04), the MASTER leg (#t-037 slice 2). Layout: docs/PROTOCOL.md "The v2.3
// event frame". Both entry points run on MIDI_Task only (midi.cpp), so the batch needs no lock.

// One host channel-voice message (USB-MIDI CIN 0x8-0xE). Ignored unless this node is a master.
// The ONE call midiProcess() makes for host channel voice -- keep it one line wide.
void midiEventCapture(uint8_t status, uint8_t data1, uint8_t data2);

// Polled every MIDI_Task tick: closes a 5 ms batch that has run its window, and sends the
// pure carry-over frame that repeats the last frame's new records when nothing new followed.
void midiEventTick();
