> [!IMPORTANT]
> This is a fork of [shiomachisoft/picow_ble_usb_hid_bridge](https://github.com/shiomachisoft/picow_ble_usb_hid_bridge) that adds multi-device connections. I modified the project to get my own keyboard *and* mouse working with one dongle. It is not maintained beyond the that functionality.

# Pico W / Pico 2 W - BLE to USB HID Bridge

This firmware for the Raspberry Pi Pico W / Pico 2 W allows you to use multiple BLE HID devices, such as a keyboard and mouse, as wired USB devices, even on PCs without Bluetooth.  
It operates as a BLE Central (Host), forwarding input data from the connected BLE device to the host PC via USB, where it is recognized as a standard USB HID device.
      
**Key Benefits & Use Cases**  

* **Works even before OS boot**  
  It can be used even before the OS Bluetooth drivers are loaded (such as during UEFI/BIOS setup or OS installation).  
  
* **Share and switch between multiple PCs via a USB switch**  
  Because it is recognized as a wired USB device, it is compatible with USB switches.   
  *(Note: Compatibility with KVM switches is currently unverified.)*  

**Note:** For the opposite direction, USB to BLE, see
[pico_usb_ble_hid_bridge](https://github.com/shiomachisoft/pico_usb_ble_hid_bridge).

<img width="716" height="391" alt="image" src="https://github.com/user-attachments/assets/6d4410d5-2912-4bd5-93dc-8aef206fb2b0" />

## Source Code & Binaries

The full source code for this program and the ready-to-flash binary (.uf2 file) are available in this repository:

- **Pre-built binaries**: Available under releases
- **Build from source**: See [docs/build.md](docs/build.md) for detailed build instructions using VS Code and the Pico SDK.

> **Note:**  
> The source code is written in C using the Pico SDK.

## Usage

1.  Plug the board into a USB port. The LED blinks while nothing is connected
    over BLE.
2.  Put the keyboard or mouse into pairing mode; its manual will say how.
3.  The LED goes solid once the device is connected, and the PC sees a USB input
    device.

After the first pairing, the Pico persistently stores which devices it needs to
reconnect to, at the next power-on. It remembers up to eight previously-paired
devices, though only two can be bridged to the PC at the same time. Some
peripherals sleep deeply, and do not reconnect unprompted — press a key or two
to wake them up and reconnect.

## How it works

**Two cores.** Core 0 runs the USB device stack and Core 1 runs BTstack, so a
report received over BLE can be sent over USB with little delay between the two.
The USB endpoint is polled every 1 ms.

**Pass-through.** The HID report descriptor is read from the BLE device and
handed to the PC unchanged, so device-specific keys such as media controls keep
working. Input reports are forwarded byte for byte. The USB device re-enumerates
once the BLE link is up, which is what makes the PC read the new descriptor.

**Connection handling.** The bridge alternates between reconnecting to a bonded
device and scanning for new ones. It supports Resolvable Private Addresses (RPA)
via Identity Resolving Keys (IRK), enabling automatic reconnection even when
peripherals periodically rotate their Bluetooth address for privacy. Once the link
is encrypted it asks for a 12.5-15 ms connection interval, so a power-saving
default on the peripheral does not turn into input lag.

## Documentation

- [Building in VS Code](docs/build.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Verified Devices](docs/verified_devices.md)

## License
See LICENSE.TXT.

## Acknowledgments

* [mateibarbu19](https://github.com/mateibarbu19) - For the changes and implementation in version 20260810.
