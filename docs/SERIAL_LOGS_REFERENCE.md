# Nowde Serial Monitor Log Reference

## Overview
The Nowde firmware outputs detailed logs via Serial Monitor (115200 baud) to help you monitor ESP-NOW communication and debug issues.

## Serial Port Configuration

The logging output can be easily switched between different serial ports. At the top of `main.cpp`:

```cpp
// ============= LOGGING CONFIGURATION =============
// Choose which Serial port to use for debug logs
// Options: Serial (USB), Serial1 (UART1), Serial2 (UART2)
#define DEBUG_SERIAL Serial
// Uncomment one of these to use a different port:
// #define DEBUG_SERIAL Serial1
// #define DEBUG_SERIAL Serial2
```

### Available Options:

1. **Serial (USB CDC)** - Default
   - Uses USB port (same as MIDI)
   - No extra wiring needed
   - 115200 baud
   - Good for basic debugging

2. **Serial1 (UART1)**
   - External UART pins required
   - Can monitor while MIDI is active
   - Typical pins: TX=43, RX=44 (check your board)
   - Requires USB-to-Serial adapter

3. **Serial2 (UART2)**
   - Alternative external UART
   - Typical pins: TX=17, RX=18 (check your board)
   - Requires USB-to-Serial adapter

### How to Switch:

1. **To use Serial (USB)** - Default, no changes needed
2. **To use Serial1:**
   ```cpp
   #define DEBUG_SERIAL Serial1
   // Optional: Initialize with specific pins in setup()
   // Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
   ```
3. **To use Serial2:**
   ```cpp
   #define DEBUG_SERIAL Serial2
   // Optional: Initialize with specific pins in setup()
   // Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
   ```

### ESP32-S3 Pin Recommendations:

**Serial1:**
- TX: GPIO 43
- RX: GPIO 44

**Serial2:**
- TX: GPIO 17  
- RX: GPIO 18

*Note: Check your specific ESP32-S3 board pinout as pins may vary.*

## Startup Sequence

```
╔════════════════════════════════════╗
║         NOWDE ESP-NOW v1.0         ║
║    Hemisphere Project © 2025       ║
╚════════════════════════════════════╝

[INIT] USB initialized
[INIT] USB MIDI initialized
[INIT] WiFi STA mode configured
[INIT] ESP-NOW initialized
[INIT] ESP-NOW callbacks registered
[INIT] Broadcast peer added

═══════════════════════════════════
Device MAC: XX:XX:XX:XX:XX:XX
Version: 1.0
═══════════════════════════════════

Waiting for USB MIDI commands...
  - Bridge Connected: Activates sender mode
  - Subscribe Layer: Activates receiver mode
```

## Mode Activation

### Sender Mode Activated
```
=== SENDER MODE ACTIVATED ===
Received: Bridge Connected SysEx
Status: Broadcasting ESP-NOW beacons
=============================
```

**When:** Bridge detects Nowde and sends SysEx command
**Next:** Device starts broadcasting beacons every 1 second

### Receiver Mode Activated
```
=== RECEIVER MODE ACTIVATED ===
Received: Subscribe to Layer SysEx
Layer: Layer1
Status: Listening for sender beacons
===============================
```

**When:** Bridge sends Subscribe to Layer SysEx
**Next:** Device listens for sender beacons and registers them

## ESP-NOW Communication

### Sender Beacon Transmission (Sender Mode)
```
[ESP-NOW TX] Sender Beacon #10 (every 10th logged)
[ESP-NOW TX] Sender Beacon #20 (every 10th logged)
[ESP-NOW TX] Sender Beacon #30 (every 10th logged)
```

**Frequency:** Every 1 second (logged every 10th to reduce spam)
**Purpose:** Announce presence to receivers

### Receiver Info Transmission (Receiver Mode)
```
[ESP-NOW TX] Receiver Info #10 to 1 sender(s) (every 10th logged)
  Layer: Layer1
[ESP-NOW TX] Receiver Info #20 to 1 sender(s) (every 10th logged)
  Layer: Layer1
```

**Frequency:** Every 1 second + random 0-200ms (logged every 10th)
**Purpose:** Inform senders of layer subscription

### Sender Beacon Received (Receiver Mode)
```
[ESP-NOW RX] Sender Beacon
  From: AA:BB:CC:DD:EE:FF
  Action: Registered new sender
  Total Senders: 1
```

**When:** Receiver detects a new sender
**Result:** Sender added to local table

### Receiver Info Received (Sender Mode)
```
[ESP-NOW RX] Receiver Info
  From: 11:22:33:44:55:66
  Layer: Layer1
  Action: Registered new receiver
  Total Receivers: 1
```

**When:** Sender receives info from a receiver
**Result:** Receiver added to local table

### Layer Change (Sender Mode)
```
[ESP-NOW RX] Receiver Info Update
  From: 11:22:33:44:55:66
  Layer Changed: Layer2
```

**When:** Receiver changes layer subscription
**Result:** Table updated with new layer

## Table Management

### Bridge Report (Sender Mode)
```
[BRIDGE REPORT] Receiver table update
  Receivers: 2
    - 11:22:33:44:55:66 (Layer1)
    - AA:BB:CC:DD:EE:FF (Layer2)
```

**Frequency:** Every 500ms (only logged when count changes)
**Purpose:** Send receiver list to Bridge via USB MIDI

### Timeout - Receiver Removed (Sender Mode)
```
[TIMEOUT] Receiver removed
  MAC: 11:22:33:44:55:66
  Layer: Layer1
  Remaining: 1
```

**When:** No updates from receiver for 5 seconds
**Result:** Receiver removed from table

### Timeout - Sender Removed (Receiver Mode)
```
[TIMEOUT] Sender removed
  MAC: AA:BB:CC:DD:EE:FF
  Remaining: 0
```

**When:** No beacons from sender for 5 seconds
**Result:** Sender removed from table

## Log Categories

### [INIT] - Initialization
- USB/MIDI setup
- WiFi configuration
- ESP-NOW initialization
- Peer registration

### [ESP-NOW TX] - Transmission
- Sender beacons (every 10th logged)
- Receiver info (every 10th logged)

### [ESP-NOW RX] - Reception
- New device discovery
- Layer updates
- Beacon reception

### [BRIDGE REPORT] - USB Communication
- Receiver table updates to Bridge
- Only logged when table changes

### [TIMEOUT] - Connection Loss
- Device removed due to timeout
- Shows remaining devices

### [ERROR] - Errors
- Initialization failures
- ESP-NOW errors

## Example Sessions

### Scenario 1: Single Sender (Bridge Mode)

```
[INIT] USB initialized
[INIT] USB MIDI initialized
...

=== SENDER MODE ACTIVATED ===
Received: Bridge Connected SysEx
Status: Broadcasting ESP-NOW beacons
=============================

[ESP-NOW TX] Sender Beacon #10 (every 10th logged)
[ESP-NOW TX] Sender Beacon #20 (every 10th logged)

[ESP-NOW RX] Receiver Info
  From: 11:22:33:44:55:66
  Layer: Layer1
  Action: Registered new receiver
  Total Receivers: 1

[BRIDGE REPORT] Receiver table update
  Receivers: 1
    - 11:22:33:44:55:66 (Layer1)
```

### Scenario 2: Receiver Finding Sender

```
[INIT] USB initialized
[INIT] USB MIDI initialized
...

=== RECEIVER MODE ACTIVATED ===
Received: Subscribe to Layer SysEx
Layer: Layer1
Status: Listening for sender beacons
===============================

[ESP-NOW RX] Sender Beacon
  From: AA:BB:CC:DD:EE:FF
  Action: Registered new sender
  Total Senders: 1

[ESP-NOW TX] Receiver Info #10 to 1 sender(s) (every 10th logged)
  Layer: Layer1
```

### Scenario 3: Connection Loss

```
[ESP-NOW RX] Receiver Info
  From: 11:22:33:44:55:66
  Layer: Layer1
  Action: Registered new receiver
  Total Receivers: 1

[ESP-NOW TX] Sender Beacon #50 (every 10th logged)
[ESP-NOW TX] Sender Beacon #60 (every 10th logged)

[TIMEOUT] Receiver removed
  MAC: 11:22:33:44:55:66
  Layer: Layer1
  Remaining: 0

[BRIDGE REPORT] Receiver table update
  Receivers: 0
```

## Debugging Tips

### No Sender Beacons?
- Check for "SENDER MODE ACTIVATED" message
- Verify Bridge sent SysEx (check Bridge logs)
- Look for ESP-NOW initialization errors

### No Receiver Detection?
- Look for "Receiver Info" messages
- Check if receiver is in range
- Verify receiver activated ("RECEIVER MODE ACTIVATED")

### Frequent Timeouts?
- Check distance between devices (<20m recommended)
- Look for interference
- Verify power supply stability
- Consider increasing timeout values

### Table Not Updating?
- Check for "BRIDGE REPORT" messages
- Verify USB MIDI connection
- Look for SysEx send errors

## Log Verbosity

### Current Settings
- **Beacons:** Every 10th logged (reduces spam)
- **Receiver Info:** Every 10th logged (reduces spam)
- **Bridge Reports:** Only on table changes
- **Discoveries:** Always logged
- **Timeouts:** Always logged
- **Errors:** Always logged

### Adjusting Verbosity

To log EVERY beacon/info message (for deep debugging):

```cpp
// In sendSenderBeacon():
if (beaconCount % 1 == 0) {  // Log every beacon
  Serial.printf("[ESP-NOW TX] Sender Beacon #%d\n", beaconCount);
}

// In sendReceiverInfo():
if (infoCount % 1 == 0 && sentCount > 0) {  // Log every send
  Serial.printf("[ESP-NOW TX] Receiver Info #%d\n", infoCount);
}
```

## Serial Monitor Settings

**Required Settings:**
- Baud Rate: 115200
- Line Ending: Both NL & CR (or Newline)
- Port: Depends on DEBUG_SERIAL configuration
  - **Serial (USB):** Secondary USB CDC port (not the MIDI port)
  - **Serial1/Serial2:** External USB-to-Serial adapter

**PlatformIO Monitor:**
```bash
cd Nowde
platformio device monitor
```
*Note: Will auto-detect the Serial port. For Serial1/Serial2, connect USB-to-Serial adapter.*

**Arduino IDE:**
- Tools → Serial Monitor
- Set baud to 115200
- Select correct COM port

**External USB-to-Serial Adapter (for Serial1/Serial2):**
1. Connect adapter TX to ESP32 RX pin
2. Connect adapter RX to ESP32 TX pin  
3. Connect GND to GND
4. Open terminal: `screen /dev/tty.usbserial-XXXX 115200`
5. Or use PlatformIO with correct port

## Color Coding (if supported)

Some terminal programs support ANSI colors. Future versions may include:
- 🟢 Green: Success/Discovery
- 🔴 Red: Errors/Timeouts
- 🔵 Blue: Info/Status
- 🟡 Yellow: Warnings
