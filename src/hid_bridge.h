// Copyright © 2026 Shiomachi Software. All rights reserved.
// Cross-core contract between the BLE host (Core 1) and the USB device (Core 0).
// The BLE side publishes a compact, spinlock-protected snapshot of which HID
// interfaces are READY and which BTstack HIDS client id backs each one; the USB
// side reads that snapshot to build its configuration/report descriptors and to
// route forwarded reports to the correct interface instance.
#ifndef HID_BRIDGE_H
#define HID_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include "Type.h"

// Maximum number of BLE HID devices bridged simultaneously. Each device is
// exposed to the PC as its own USB HID interface. Raising this grows the slot
// tables, the report queues, the USB endpoint range and the descriptor buffer.
#ifndef MAX_HID_DEVICES
#define MAX_HID_DEVICES 2
#endif

// Maximum number of previously-paired device addresses kept for auto-reconnect.
// Independent of MAX_HID_DEVICES: the list only stores addresses (no USB
// interface or report queue is allocated per entry), so it can be larger than
// the number of devices that can be bridged at once. The device mints a new
// random address per pairing, so a few stale entries are expected.
#ifndef MAX_KNOWN_DEVICES
#define MAX_KNOWN_DEVICES 8
#endif

// A READY device has completed GATT discovery, so its HID report descriptor is
// known and stable. The USB interface for that device is backed by hids_cid.
typedef struct {
    uint8_t  count;                 // number of READY devices (0..MAX_HID_DEVICES)
    uint16_t hids_cid[MAX_HID_DEVICES];  // BTstack HIDS client id per compact position
    uint16_t report_len[MAX_HID_DEVICES]; // report descriptor length per position
    uint8_t  slot[MAX_HID_DEVICES];    // physical slot index backing each compact position
} usb_ready_snapshot_t;

// Set by Core 1 whenever the READY set changes, so Core 0 re-enumerates the USB
// device and the PC re-reads the (changed) configuration + report descriptors.
extern volatile bool g_usb_reinit_request;

// --- Core 1 (BLE host) implements these; Core 0 (USB) calls them ------------
// Copy the current READY snapshot out under the shared spinlock. Always returns
// true (the snapshot is always valid).
bool hid_bridge_get_ready_snapshot(usb_ready_snapshot_t *out);

// Return a stable pointer to the HID report descriptor for the device at the
// given compact position, plus its length. Returns NULL if the position is out
// of range or the device is not READY. The returned pointer is valid for as long
// as that device stays connected (the descriptor bytes live in shared RAM).
const uint8_t *hid_bridge_get_report_descriptor(uint8_t position, uint16_t *len);

// --- Core 0 (USB) calls these to publish a re-init request ------------------
// (g_usb_reinit_request is written by Core 1 and cleared by Core 0.)

#endif // HID_BRIDGE_H
