// Copyright © 2025 Shiomachi Software. All rights reserved.
#ifndef COMMON_H
#define COMMON_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/cyw43_arch.h"
#include "Type.h"

// [Logging]
// Both cores share a single serial console, so every message is tagged with the
// subsystem that emitted it. USB tracing is chatty and only useful when
// debugging enumeration, so it is compiled out unless CMake enables it.
#define SYS_LOG(...) printf("[SYS] " __VA_ARGS__)
#define BLE_LOG(...) printf("[BLE] " __VA_ARGS__)

#ifdef ENABLE_USB_LOGGING
#define USB_LOG(...) printf("[USB] " __VA_ARGS__)
#else
#define USB_LOG(...) ((void)0)
#endif

// [Definitions]
#include "hid_bridge.h"

// Maximum depth of each per-device HID report queue
#define CMN_QUE_DATA_MAX_HID_RPT 32

// Maximum size of the HID report data
#define CMN_HID_RPT_DATA_SIZE 512

// [Enumerations]
// There is one report queue per bridged device; the queue index equals the
// device slot index (0 .. MAX_HID_DEVICES-1). CMN_QUE_KIND_NUM is therefore the
// total number of queues. Every queue carries HID reports, so callers pass the
// device slot directly as the queue index.
typedef enum _E_CMN_QUE_KIND { 
    CMN_QUE_KIND_HID_RPT_0 = 0, // HID Report Queue, device slot 0
    CMN_QUE_KIND_NUM          // Number of queue types (= MAX_HID_DEVICES)
} E_CMN_QUE_KIND;

#pragma pack(1)

// [Structures]
// Queue control structure
typedef struct _ST_QUE {
    ULONG head; // Head index (Read position)
    ULONG tail; // Tail index (Write position)
    ULONG max;  // Maximum capacity of the queue
    PVOID pBuf; // Pointer to the data buffer
} ST_QUE;

// HID Report structure
typedef struct _ST_HID_RPT {
    uint8_t report_id;
    uint8_t report[CMN_HID_RPT_DATA_SIZE];
    uint16_t report_len;
} ST_HID_RPT;

#pragma pack()

// [Function Prototypes]
bool CMN_Enqueue(ULONG iQue, PVOID pData);
bool CMN_Dequeue(ULONG iQue, PVOID pData);
bool CMN_PeekQueue(ULONG iQue, PVOID pData);
void CMN_AdvanceQueue(ULONG iQue);
void CMN_ClearQueue(ULONG iQue);
void CMN_EntrySpinLock(void);
void CMN_ExitSpinLock(void);
void CMN_Init(void);

// READY-set snapshot shared between cores. Core 1 (BLE) publishes it whenever
// the set of connected/READY devices changes; Core 0 (USB) reads it to build
// its descriptors and to route forwarded reports. Both are atomic w.r.t. the
// shared spinlock.
void CMN_PublishReadySnapshot(const usb_ready_snapshot_t *snap);
bool CMN_GetReadySnapshot(usb_ready_snapshot_t *out);

// Drop every pending report across all device queues (used on USB re-init)
void CMN_ClearAllQueues(void);

#endif
