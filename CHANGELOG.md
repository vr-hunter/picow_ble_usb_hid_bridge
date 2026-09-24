# Changelog

## 20260924

- Log console moved from UART0 (GPIO 0/1) to the USB CDC-ACM serial port.
  `pico_enable_stdio_usb` is now enabled, UART stdio is disabled, and the
  `UART_BAUD_RATE` build option was removed. Open the new serial port (for
  example `/dev/ttyACM*`) to read the logs.

## 20260830

- Support for Devices with Privacy Addresses (RPA)
  - Smoothly and automatically reconnects to paired Bluetooth devices (such as modern keyboards and mice) that periodically change their address for privacy.
- Simplified Pairing
  - Eliminated manual passkey/PIN entry, allowing Bluetooth devices to pair seamlessly and automatically ("Just Works" pairing).
- Device Name Update
  - Changed the default Bluetooth display name from `"HOG Host"` to `"BLE-USB HID Bridge"`.
- Other Improvements
  - Various minor bug fixes and stability enhancements.

## 20260813

- Changes in `hog_host_demo.c`
  - Fixed an issue where the BLE connection state and LED indication would mismatch when the BLE device was rapidly power-cycled.
  - Reordered function definitions to improve source code readability.

- Global Changes (All source files)
  - Code cleanup: Removed modification history comments such as `@add`, `@chg`, and `@del`.

## 20260810

### Added

- Pico 2 W (RP2350) support alongside the Pico W. `PICO_BOARD` picks the target
  and defaults to `pico2_w`.
- `ENABLE_USB_LOGGING`, `ENABLE_HEARTBEAT_LOGS` and `UART_BAUD_RATE` build
  options.

### Fixed

- Keyboards that place a descriptor after the CCC descriptor no longer stall in
  GATT discovery, via BTstack's `ENABLE_GATT_LEGACY_CCC_DISCOVERY`.
- The HID report descriptor cache grew from 500 bytes to 2 KB, so descriptors
  from keyboards with media keys or multi-device switching are not truncated.
- Core 0 no longer touches the CYW43-attached status LED before Core 1 has
  finished `cyw43_arch_init()`, which could hang Core 0 during boot.
- A 12.5-15 ms connection interval is requested once the link is encrypted,
  instead of accepting the device's power-saving default.
