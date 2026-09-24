# Troubleshooting

## LED signal interpreting

| LED       | Meaning                                                 |
|-----------|---------------------------------------------------------|
| Off       | Core 1 has not finished starting the wireless chip.     |
| Blinking  | Scanning for a device, or reconnecting to a bonded one. |
| Steady on | Discovery finished; input is being forwarded.           |

## Reading the logs

Logs go to the USB CDC-ACM serial port, so no adapter or extra wiring is needed
beyond the USB cable. On Linux it appears as a `/dev/ttyACM*` device, on
Windows/macOS as a serial COM port; open it with any terminal program (for
example `minicom`, `screen`, or PuTTY). Set the baud rate to 115200 — it is
ignored for a USB serial port, but most terminal programs require a value.

Both cores share the console, so each line is tagged with the subsystem that
wrote it: `[SYS]`, `[BLE]` or `[USB]`. The first lines, printed before the host
opens the port, are dropped until the terminal is attached.

## A keyboard pairs, then nothing happens

Pairing succeeds, `[BLE] Search for HID service.` is printed, and nothing
follows. The keyboard keeps flashing its pairing light, eventually sleeps, and
the link drops about 30 seconds later.

BTstack normally discovers the Client Characteristic Configuration (CCC)
descriptor with a shortcut that assumes the CCC is the last descriptor of a
characteristic. Keyboards that place a Report Reference after it — which is
common — leave the state machine unable to send the write that enables
notifications, so discovery never finishes and the security manager times out.

The firmware defines `ENABLE_GATT_LEGACY_CCC_DISCOVERY` in `btstack_config.h` to
select the older two-step discovery instead. It costs one extra round trip and
copes with descriptors in any order.

## The keyboard re-appears on the PC when the BLE link comes up

Expected. Once the bridge has the HID report descriptor of the BLE device, it
disconnects and reconnects itself so the PC re-reads that descriptor and sees
the real keyboard rather than the placeholder one.
