# Quick Reference: Switching Serial Ports

## One-Line Change

Edit the top of `/Users/hmini25/Documents/MillluBridge/Nowde/src/main.cpp`:

### Option 1: USB Serial (Default)
```cpp
#define DEBUG_SERIAL Serial
```
- No extra wiring
- Use PlatformIO monitor or Arduino Serial Monitor
- Shares USB with MIDI

### Option 2: Hardware UART1
```cpp
#define DEBUG_SERIAL Serial1
```
- Requires external USB-to-Serial adapter
- ESP32-S3 pins: TX=43, RX=44 (typical)
- Doesn't interfere with USB MIDI

### Option 3: Hardware UART2
```cpp
#define DEBUG_SERIAL Serial2
```
- Requires external USB-to-Serial adapter
- ESP32-S3 pins: TX=17, RX=18 (typical)
- Alternative to Serial1

## Wiring for Serial1/Serial2

```
USB-to-Serial Adapter          ESP32-S3
┌─────────────────┐           ┌──────────────┐
│                 │           │              │
│  TX  ──────────────────────>  RX (GPIO)   │
│                 │           │              │
│  RX  <──────────────────────  TX (GPIO)   │
│                 │           │              │
│  GND ──────────────────────>  GND         │
│                 │           │              │
└─────────────────┘           └──────────────┘
```

**Serial1 Example (ESP32-S3):**
- Adapter TX → ESP32 GPIO 44 (RX)
- Adapter RX → ESP32 GPIO 43 (TX)
- GND → GND

**Serial2 Example (ESP32-S3):**
- Adapter TX → ESP32 GPIO 18 (RX)
- Adapter RX → ESP32 GPIO 17 (TX)
- GND → GND

## When to Use Each Option

### Use Serial (USB) when:
- ✅ Simple debugging
- ✅ No extra hardware available
- ✅ Don't need simultaneous MIDI and logging
- ✅ Development phase

### Use Serial1/Serial2 when:
- ✅ Need to monitor while USB MIDI is active
- ✅ Deploying in production with logging
- ✅ Debugging communication issues
- ✅ Long-term monitoring
- ✅ Multiple Nowdes - one USB-Serial per device

## Compilation

After changing `DEBUG_SERIAL`:

```bash
cd /Users/hmini25/Documents/MillluBridge/Nowde
platformio run --target upload
```

That's it! The code automatically uses your chosen serial port.

## Terminal Commands

**For Serial (USB):**
```bash
platformio device monitor
```

**For Serial1/Serial2 (macOS):**
```bash
# Find your adapter
ls /dev/tty.usbserial-*

# Connect
screen /dev/tty.usbserial-XXXX 115200

# Disconnect: Ctrl+A then K then Y
```

**For Serial1/Serial2 (Linux):**
```bash
# Find your adapter
ls /dev/ttyUSB*

# Connect
screen /dev/ttyUSB0 115200
```

**For Serial1/Serial2 (Windows):**
```bash
# Use PuTTY or Arduino Serial Monitor
# Select correct COM port at 115200 baud
```

## Pro Tip: Multiple Nowdes

When debugging multiple Nowdes simultaneously:

1. **Nowde #1 (Sender):** Connect via USB, use Serial (default)
2. **Nowde #2 (Receiver):** Use Serial1 with USB-Serial adapter
3. **Nowde #3 (Receiver):** Use Serial1 with another USB-Serial adapter

Each device sends logs to its own terminal window!

## Troubleshooting

### No output on Serial1/Serial2?
- Check wiring (TX/RX crossed?)
- Verify pins in your ESP32-S3 variant
- Ensure baud rate is 115200
- Try swapping TX/RX connections

### Garbled output?
- Wrong baud rate (must be 115200)
- Incorrect pins
- Bad USB-Serial adapter
- Power supply issues

### Can't find USB-Serial port?
- Check adapter is recognized: `ls /dev/tty.*` (macOS)
- Install driver (CH340, CP2102, FTDI, etc.)
- Try different USB port

## Hardware Serial Adapters

**Recommended adapters:**
- CP2102 modules (~$2-5)
- CH340G modules (~$1-3)
- FTDI FT232RL (~$5-15, higher quality)

**What to buy:**
- 3.3V compatible (ESP32 is 3.3V!)
- USB-to-TTL/UART converter
- Minimum 3 wires: TX, RX, GND

**Where to find:**
- Amazon, eBay, AliExpress
- Adafruit, SparkFun (more expensive but reliable)
- Local electronics stores
