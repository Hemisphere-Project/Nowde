#pragma once

#include <Arduino.h>

// ============= SINGLE USB WRITER, SINGLE ACTIVE ENDPOINT =============
// TinyUSB is driven from ONE task only: the MIDI task (core 0) pumps the queue below into the
// MIDI IN endpoint. Every other context (the ESP-NOW receive callback in the WiFi task, the
// ESP-NOW task on core 1, setup()) only enqueues and never blocks.

void usbOutInit();                                 // before anything logs or sends
// The USB device itself (TinyUSB lives in usb_out.cpp alone, Nowde 3 #t-050 ②): the descriptors
// with the node's name, the MIDI interface, then the stack. setup() keeps its order per board.
void usbOutDescriptors(const char* productName);   // "Nowde - XXXXXX", before usbOutStart()
void usbOutMidiBegin();                            // the MIDI interface
void usbOutStart();                                // USB.begin(): the host enumerates us
// MIDI task only: one USB-MIDI packet from the host (CIN + 3 bytes); false = none waiting.
bool usbInRead(uint8_t* cin, uint8_t* b1, uint8_t* b2, uint8_t* b3);
// Queue one complete F0..F7 frame as USB-MIDI packets (CIN 4, then 5/6/7 on the last), contiguous
// in the queue. The ONE SysEx packetizer: every frame to the host goes through here (host_link's
// hostLinkSend, the USB-MIDI backend). Queue full mid-frame: stops there, the host parser drops the unterminated frame.
bool usbOutSysEx(const uint8_t* frame, size_t len);
// Queue one short MIDI message (channel voice, MTC quarter frame, real-time) -- plain MIDI bytes,
// status first; the packet is the backend's business (usb_midi_packets.h). false = queue full or
// not a short message.
bool usbOutMessage(uint8_t status, uint8_t d1 = 0, uint8_t d2 = 0);
void usbOutLock();                                 // hold while enqueueing a multi-packet message
void usbOutUnlock();                               //   so it stays contiguous in the queue
void usbOutPump();                                 // MIDI task only: drain to TinyUSB
