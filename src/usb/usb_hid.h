// USB HID forwarding (Core 0): reads the READY snapshot, forwards the queued
// BLE HID reports to the matching USB HID interface instances, and implements
// the TinyUSB HID application callbacks.
#ifndef USB_HID_H
#define USB_HID_H

// Service one round of HID report forwarding (called from the USB main loop).
void usb_hid_task(void);

#endif // USB_HID_H
