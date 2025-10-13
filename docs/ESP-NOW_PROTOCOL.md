# ESP-NOW Communication Protocol

## Overview
This document describes the ESP-NOW communication protocol implemented for Nowde devices.

## System Architecture

### Device Modes
- **Sender Mode**: Activated by Bridge when USB connection is established
  - Broadcasts presence beacons
  - Maintains table of known receivers
  - Reports receiver table to Bridge via USB MIDI
  
- **Receiver Mode**: Activated by SysEx command with layer subscription
  - Listens for sender beacons
  - Maintains table of known senders
  - Periodically sends subscription info to senders

Both modes can be active simultaneously on the same device.

## SysEx Protocol (USB MIDI)

### Message Format
All SysEx messages use manufacturer ID `0x7D` (Educational/Development use)

```
F0 7D <command> [data...] F7
```

### Commands

#### 1. Bridge Connected (0x01)
Activates sender mode on Nowde
```
F0 7D 01 F7
```

#### 2. Subscribe to Layer (0x02)
Activates receiver mode with layer subscription (max 16 chars)
```
F0 7D 02 [layer_name...] F7
```

#### 3. Receiver Table Report (0x03)
Sent from Nowde to Bridge with receiver status
```
F0 7D 03 [count] [mac1(6) layer1(16)] [mac2(6) layer2(16)] ... F7
```

## ESP-NOW Protocol

### Message Types

#### 1. Sender Beacon (0x01)
Minimal broadcast to announce sender presence
```c
struct SenderBeacon {
  uint8_t type = 0x01;
} __attribute__((packed));
```
Size: 1 byte

#### 2. Receiver Info (0x02)
Sent from receiver to known senders with layer subscription
```c
struct ReceiverInfo {
  uint8_t type = 0x02;
  char layer[16];
} __attribute__((packed));
```
Size: 17 bytes (1 + 16)

### Communication Flow

1. **Bridge connects to Nowde via USB**
   - Bridge detects "Nowde" device
   - Opens MIDI ports
   - Sends "Bridge Connected" SysEx
   - Nowde activates sender mode

2. **Sender Mode Operations**
   - Broadcasts beacon every 1 second
   - Maintains receiver table with 5s timeout
   - Reports receiver table to Bridge every 500ms (on change)

3. **Receiver discovers Sender**
   - Receives sender beacon
   - Adds to sender table
   - Sends receiver info with subscribed layer

4. **Sender receives Receiver Info**
   - Updates receiver table
   - Reports update to Bridge

5. **Table Management**
   - Entries older than 5s are removed
   - Random jitter (0-200ms) prevents collisions

## Constants (Configurable)

```cpp
#define NOWDE_VERSION "1.0"
#define MAX_LAYER_LENGTH 16
#define RECEIVER_TIMEOUT_MS 5000
#define SENDER_TIMEOUT_MS 5000
#define RECEIVER_BEACON_INTERVAL_MS 1000
#define SENDER_BEACON_INTERVAL_MS 1000
#define BRIDGE_REPORT_INTERVAL_MS 500
```

## Device Identity
- **UUID**: Derived from MAC address
- **Name**: Not hardcoded (USB name is "Nowde 1.0")
- **Version**: Compile-time constant "1.0"
- **No EEPROM storage**: Device state is volatile

## Broadcast Address
Uses `FF:FF:FF:FF:FF:FF` for all ESP-NOW broadcasts

## Message Size Limits
- ESP-NOW: 250 bytes max
- Sender Beacon: 1 byte ✓
- Receiver Info: 17 bytes ✓
- All messages well within limit

## Bridge Implementation

### Python MIDI SysEx Methods

```python
# In output_manager.py
def send_bridge_connected(self):
    """Activate sender mode"""
    message = [0xF0, 0x7D, 0x01, 0xF7]
    self.midi_out.send_message(message)

def send_subscribe_layer(self, layer_name):
    """Activate receiver mode with layer"""
    layer_bytes = layer_name[:16].encode('ascii')
    message = [0xF0, 0x7D, 0x02] + list(layer_bytes) + [0xF7]
    self.midi_out.send_message(message)
```

### Auto-connection Flow
1. Bridge scans MIDI ports every second
2. Finds first port starting with "Nowde"
3. Opens bidirectional MIDI connection
4. Sends "Bridge Connected" SysEx
5. Nowde activates sender mode
6. Bridge GUI updates with remote Nowde table

## Testing Procedure

1. **Flash Nowde firmware** to ESP32-S3
2. **Connect via USB** to computer running Bridge
3. **Bridge should auto-detect** and show "Nowde 1.0"
4. **Check Serial Monitor**: Should show "Sender mode activated"
5. **Second Nowde**: Can be configured as receiver via SysEx
6. **Monitor Bridge GUI**: Should populate Remote Nowdes table

## Troubleshooting

### No sender mode activation
- Check USB MIDI connection
- Verify SysEx message format
- Check Serial output for "Sender mode activated"

### ESP-NOW not working
- Ensure WiFi.mode(WIFI_STA) is called
- Check esp_now_init() return value
- Verify broadcast peer added successfully

### Timeout issues
- Adjust timeout constants if needed
- Check beacon intervals match expectations
- Verify table cleanup is running
