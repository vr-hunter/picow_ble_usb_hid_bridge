// USB device side (Core 0): initializes the TinyUSB device stack and runs its
// main loop. The loop services tud_task(), forwards queued HID reports,
// streams the shared log console to the CDC-ACM port, and handles the
// re-initialization requests raised by the BLE core when the READY set
// changes.
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

// Initialize the TinyUSB device stack on the configured roothub port (Core 0).
void usb_device_init(void);

// Run the USB device main loop (Core 0). Does not return.
void usb_device_main(void);

#endif // USB_DEVICE_H
