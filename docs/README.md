# Nowde - ESP32-S3 USB MIDI Device

This is a PlatformIO project for ESP32-S3 that creates a USB MIDI device.

## Hardware

- **Board**: ESP32-S3 DevKit C-1
- **USB Ports**: 
  - **USB Port (UART)**: Used for flashing and serial debugging
  - **USB Port (Native)**: Used for USB MIDI communication

## Features

- Native USB MIDI support using ESP32-S3's built-in USB
- Recognized as a standard USB MIDI device on any computer
- No external MIDI hardware required

## Setup

### Prerequisites

1. Install [PlatformIO](https://platformio.org/install)
2. Install PlatformIO extension in VS Code (recommended)

### Building and Uploading

1. Connect the ESP32-S3 to your computer via the **UART USB port** (for flashing)
2. Build the project:
   ```bash
   pio run
   ```
3. Upload to the board:
   ```bash
   pio run --target upload
   ```

### Using USB MIDI

1. After uploading, disconnect the UART USB cable
2. Connect the **Native USB port** to your computer
3. The device should appear as a USB MIDI device in your DAW or MIDI software

## Configuration

### Build Flags

The project uses these important build flags in `platformio.ini`:

- `ARDUINO_USB_MODE=1`: Enable native USB mode
- `ARDUINO_USB_CDC_ON_BOOT=0`: Disable CDC on boot to allow MIDI
- `BOARD_HAS_USB_NATIVE=1`: Indicate native USB support

#### Optional: Force Receiver Subscription (Test)

For testing, you can force the device to start directly in receiver mode and subscribe to a specific "layer" without sending a SysEx command.

Use the following define either at the top of `src/main.cpp` or via `platformio.ini` build flags:

- `FORCE_RECEIVER_LAYER="LayerName"`

Example using `platformio.ini`:

```ini
[env:esp32-s3-devkitc-1]
build_flags =
   -DARDUINO_USB_MODE=1
   -DARDUINO_USB_CDC_ON_BOOT=0
   -DFORCE_RECEIVER_LAYER=\"TestLayer\"
```

When this define is present, the firmware:

- Enables receiver mode on boot
- Subscribes to the specified layer
- Will still accept a later SysEx Subscribe Layer message that can change the subscription

### MIDI Library

The project uses the `USBMIDI` library for ESP32-S3, which provides:
- `MIDI.noteOn(note, velocity, channel)`
- `MIDI.noteOff(note, velocity, channel)`
- `MIDI.controlChange(controller, value, channel)`
- And other standard MIDI messages

## Example Code

The default `main.cpp` sends a test MIDI note every 2 seconds. You can modify this to:

- Read sensors and send MIDI CC messages
- Respond to MIDI input from a computer
- Create a custom MIDI controller

## Troubleshooting

### Device not recognized

1. Make sure you're using the **Native USB port**, not the UART port
2. Check that the USB cable supports data transfer (not just power)
3. Try a different USB port on your computer

### Upload fails

1. Make sure you're using the **UART USB port** for flashing
2. Hold the BOOT button while connecting if the board doesn't enter bootloader mode
3. Check that the correct port is selected in PlatformIO

## License

This project is open source and available under the MIT License.
