#include <Arduino.h>
#include <USB.h>
#include <USBMIDI.h>

// Create USB MIDI instance
USBMIDI MIDI;

void setup() {
  // Initialize USB
  USB.begin();
  
  // Initialize USB MIDI
  MIDI.begin();
  
  // Wait for USB to be ready
  delay(1000);
  
  // Optional: Send a test note on startup
  // MIDI.noteOn(60, 127, 1);  // Middle C, max velocity, channel 1
  // delay(500);
  // MIDI.noteOff(60, 0, 1);
}

void loop() {
  // Example: Send MIDI note every 2 seconds
  static unsigned long lastNoteTime = 0;
  unsigned long currentTime = millis();
  
  if (currentTime - lastNoteTime >= 2000) {
    lastNoteTime = currentTime;
    
    // Send a test MIDI note
    MIDI.noteOn(60, 100, 1);   // Note On: Middle C, velocity 100, channel 1
    delay(100);
    MIDI.noteOff(60, 0, 1);    // Note Off: Middle C
  }
  
  // Your custom MIDI logic goes here
  // For example, you could:
  // - Read sensors and convert to MIDI
  // - Receive MIDI from USB and process it
  // - Control LEDs based on MIDI input
  
  delay(10);
}
