# Building in Visual Studio Code

This guide explains how to build the project using the Raspberry Pi Pico extension.

## Prerequisites

Install the "Raspberry Pi Pico" extension in Visual Studio Code on Windows.
These versions are known to work:

- VS Code 1.135.0
- Raspberry Pi Pico extension 0.22.0

## Steps

1.  Open a new VS Code window.
2.  Click "Import Project" in the Raspberry Pi Pico extension.
3.  For "Location", select the `src` folder.
4.  For "Select Pico SDK version", select "v2.2.0".
5.  Click "Import".
6.  Under "Switch Board", pick "pico_w" or "pico2_w".
7.  Under "Switch Build Type", pick "Release" or "Debug".
8.  Click "Compile Project".


## Options in CMakeLists.txt

| Option | Default | Effect |
|----|----|----|
| `ENABLE_USB_LOGGING` | `OFF` | Log USB device events and every forwarded HID report. |
| `ENABLE_HEARTBEAT_LOGS` | `OFF` | Log a heartbeat line every 5 seconds to show Core 0 is alive. |