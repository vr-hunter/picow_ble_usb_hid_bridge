// Per-device HID report queue shared between the cores. Core 1 (BLE host)
// enqueues the reports it receives from a device; Core 0 (USB) peeks/advances
// them and forwards them to the USB host. There is one queue per device slot
// (0 .. MAX_HID_DEVICES-1); the queue index equals the slot index.
#ifndef REPORT_QUEUE_H
#define REPORT_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "hid_bridge.h"

// Maximum depth of each per-device report queue.
#define REPORT_QUEUE_DEPTH 32

// Maximum size of a single report's data (report id and length included).
#define REPORT_DATA_SIZE 512

// A single HID report as received from a BLE device.
typedef struct {
    uint8_t report_id;
    uint16_t report_len;
    uint8_t report[REPORT_DATA_SIZE];
} hid_report_t;

void report_queue_init(void);

// Core 1: enqueue a report for the given slot. Returns false if the queue is
// full (the report is dropped).
bool report_queue_push(uint8_t slot, const hid_report_t *report);

// Core 0: copy the oldest report of the given slot without removing it.
// Returns false if the queue is empty.
bool report_queue_peek(uint8_t slot, hid_report_t *report);

// Core 0: drop the report previously peeked for the given slot.
void report_queue_advance(uint8_t slot);

// Drop every pending report of the given slot.
void report_queue_clear(uint8_t slot);

// Drop every pending report across all slots (used on USB re-init).
void report_queue_clear_all(void);

#endif // REPORT_QUEUE_H
