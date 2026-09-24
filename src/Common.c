// Copyright © 2025 Shiomachi Software. All rights reserved.
#include "Common.h"
#include "tusb.h"
#include <stdarg.h>

// [File Scope Variables]
static ST_QUE f_astQue[CMN_QUE_KIND_NUM] = {0}; // Array of queue control structures
static ST_HID_RPT f_astQueData_hid[MAX_HID_DEVICES][CMN_QUE_DATA_MAX_HID_RPT] = {0}; // Per-slot HID ring buffers
static usb_ready_snapshot_t f_readySnapshot = {0}; // Current READY-set snapshot
static critical_section_t f_stSpinLock = {0}; // Spinlock structure

// [BLE log replay ring]
// Core 1 (BLE) pushes formatted messages here instead of printf'ing directly.
// Core 0 (USB) drains them to the CDC port only while it is connected, so
// messages emitted during a USB re-enumeration gap are replayed once the port
// comes back instead of being dropped. SPSC + shared spinlock.
static char f_astLogMsg[LOG_RING_SLOTS][LOG_RING_MSG_LEN] = {0};
static volatile uint32_t f_ulLogHead = 0; // next slot to write (producer = Core 1)
static volatile uint32_t f_ulLogTail = 0; // next slot to read  (consumer = Core 0)

// CDC out staging: Core 0 copies a popped message here and streams it into the
// CDC endpoint. Only Core 0 touches these, so no lock is needed.
static char  f_cdcOut[LOG_CDC_OUT_LEN] = {0};
static uint32_t f_cdcOutLen = 0;
static uint32_t f_cdcOutPos = 0;

// Enqueues data into the specified (per-slot) queue. iQue is the device slot.
bool CMN_Enqueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;

    if (iQue >= CMN_QUE_KIND_NUM) {
        return false;
    }

    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;

    CMN_EntrySpinLock(); // Acquire spinlock

    if ((pstQue->head == (pstQue->tail + 1) % pstQue->max)) {
        // Queue is full
    }
    else {
        memcpy(&pstHidRpt[pstQue->tail], pData, sizeof(ST_HID_RPT));
        pstQue->tail = (pstQue->tail + 1) % pstQue->max;
        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Dequeues data from the specified (per-slot) queue. iQue is the device slot.
bool CMN_Dequeue(ULONG iQue, PVOID pData)
{
    bool bRet = false;

    if (iQue >= CMN_QUE_KIND_NUM) {
        return false;
    }

    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty
    }
    else {
        memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
        pstQue->head = (pstQue->head + 1) % pstQue->max;
        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Peeks at the data from the specified (per-slot) queue without removing it.
bool CMN_PeekQueue(ULONG iQue, PVOID pData)
{
    bool bRet = false;

    if (iQue >= CMN_QUE_KIND_NUM) {
        return false;
    }

    ST_QUE *pstQue = &f_astQue[iQue];
    ST_HID_RPT *pstHidRpt = (ST_HID_RPT *)pstQue->pBuf;

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty
    }
    else {
        memcpy(pData, &pstHidRpt[pstQue->head], sizeof(ST_HID_RPT));
        bRet = true;
    }

    CMN_ExitSpinLock(); // Release spinlock

    return bRet;
}

// Advances the queue's read pointer (head)
void CMN_AdvanceQueue(ULONG iQue)
{
    if (iQue >= CMN_QUE_KIND_NUM) {
        return;
    }

    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock(); // Acquire spinlock

    if (pstQue->head == pstQue->tail) {
        // Queue is empty
    }
    else {
        pstQue->head = (pstQue->head + 1) % pstQue->max;
    }

    CMN_ExitSpinLock(); // Release spinlock
}

// Clears all data from the specified queue.
void CMN_ClearQueue(ULONG iQue)
{
    if (iQue >= CMN_QUE_KIND_NUM) {
        return;
    }

    ST_QUE *pstQue = &f_astQue[iQue];

    CMN_EntrySpinLock(); // Acquire spinlock

    // Reset head and tail pointers to empty the queue
    pstQue->head = 0;
    pstQue->tail = 0;

    CMN_ExitSpinLock(); // Release spinlock
}

// Enters a critical section (spinlock).
void CMN_EntrySpinLock(void)
{
    critical_section_enter_blocking(&f_stSpinLock);
}

// Exits the critical section (spinlock)
void CMN_ExitSpinLock(void)
{
    critical_section_exit(&f_stSpinLock);
}

// Initializes the common library
void CMN_Init(void)
{
    // One HID report queue per device slot
    for (ULONG i = 0; i < CMN_QUE_KIND_NUM; i++) {
        f_astQue[i].head = 0;
        f_astQue[i].tail = 0;
        f_astQue[i].max  = CMN_QUE_DATA_MAX_HID_RPT;
        f_astQue[i].pBuf = (PVOID)&f_astQueData_hid[i][0];
    }

    critical_section_init(&f_stSpinLock);
    memset(&f_readySnapshot, 0, sizeof(f_readySnapshot));
    f_ulLogHead = 0;
    f_ulLogTail = 0;
}

// Core 1 publishes the current READY set. Serialized on the shared spinlock.
void CMN_PublishReadySnapshot(const usb_ready_snapshot_t *snap)
{
    if (!snap) {
        return;
    }

    CMN_EntrySpinLock();
    memcpy(&f_readySnapshot, snap, sizeof(f_readySnapshot));
    CMN_ExitSpinLock();
}

// Atomic copy of the current READY set.
bool CMN_GetReadySnapshot(usb_ready_snapshot_t *out)
{
    if (!out) {
        return false;
    }

    CMN_EntrySpinLock();
    memcpy(out, &f_readySnapshot, sizeof(*out));
    CMN_ExitSpinLock();

    return true;
}

// Public cross-core accessor (declared in hid_bridge.h). Forwards to the
// snapshot storage; the descriptor bytes themselves are resolved by Core 1 via
// the BTstack HIDS descriptor storage (see hid_bridge_get_report_descriptor).
bool hid_bridge_get_ready_snapshot(usb_ready_snapshot_t *out)
{
    return CMN_GetReadySnapshot(out);
}

// Drop every pending report across all device queues (used on USB re-init)
void CMN_ClearAllQueues(void)
{
    for (ULONG i = 0; i < CMN_QUE_KIND_NUM; i++) {
        CMN_ClearQueue(i);
    }
}

// Core 1: format a BLE message into a local buffer (no lock), then copy it into
// the ring under the shared spinlock. If the ring is full the oldest message is
// dropped so the most recent ones are kept.
void LOG_RingPush(const char *fmt, ...)
{
    char local[LOG_RING_MSG_LEN];
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(local, sizeof(local), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }

    CMN_EntrySpinLock();
    if ((f_ulLogHead - f_ulLogTail) >= LOG_RING_SLOTS) {
        f_ulLogTail++; // full: drop the oldest message
    }
    memcpy(&f_astLogMsg[f_ulLogHead % LOG_RING_SLOTS], local, sizeof(local));
    f_ulLogHead++;
    CMN_ExitSpinLock();
}

// Core 0: copy the oldest buffered message out and advance the read pointer.
// Returns false when the ring is empty. The copy is made under the spinlock but
// the caller prints outside of it, so a port drop mid-drain leaves the rest
// buffered for the next pass.
bool LOG_RingPop(char *out, uint32_t maxlen)
{
    if (!out || maxlen == 0) {
        return false;
    }

    CMN_EntrySpinLock();
    if (f_ulLogHead == f_ulLogTail) {
        CMN_ExitSpinLock();
        return false;
    }
    size_t copy = sizeof(f_astLogMsg[0]);
    if (copy > maxlen) copy = maxlen;
    memcpy(out, &f_astLogMsg[f_ulLogTail % LOG_RING_SLOTS], copy);
    out[copy - 1] = '\0';
    f_ulLogTail++;
    CMN_ExitSpinLock();

    return true;
}

// Discard any partially-sent staged message (called right before tud_disconnect,
// since bytes accepted into the endpoint will be lost on the re-enumeration).
void LOG_CdcOutReset(void)
{
    f_cdcOutLen = 0;
    f_cdcOutPos = 0;
}

// Core 0: stage a (null-terminated) message for CDC streaming. Each '\n' is
// expanded to "\r\n" so the serial terminal gets CRLF line endings (the old
// printf path produced CRLF; a bare LF makes the cursor drift right).
void LOG_CdcOutLoad(const char *msg)
{
    if (!msg) {
        return;
    }
    uint32_t n = 0;
    for (const char *p = msg; *p && n < LOG_CDC_OUT_LEN - 2; p++) {
        if (*p == '\n') {
            f_cdcOut[n++] = '\r';
        }
        f_cdcOut[n++] = *p;
    }
    f_cdcOutLen = n;
    f_cdcOutPos = 0;
}

// Core 0: stream as much of the staged message into the CDC endpoint as is
// currently available. tud_task() is driven by the main loop, so this returns
// promptly; the remainder is re-attempted on the next call. A port drop simply
// pauses the transfer (f_cdcOutPos is kept) instead of dropping the tail bytes.
void LOG_CdcOutFlush(void)
{
    if (f_cdcOutPos >= f_cdcOutLen) {
        return;
    }
    if (!tud_cdc_connected()) {
        return;
    }
    while (f_cdcOutPos < f_cdcOutLen) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            break;
        }
        uint32_t chunk = f_cdcOutLen - f_cdcOutPos;
        if (chunk > avail) chunk = avail;
        int n = (int)tud_cdc_write(&f_cdcOut[f_cdcOutPos], chunk);
        if (n <= 0) {
            break;
        }
        f_cdcOutPos += (uint32_t)n;
    }
    if (f_cdcOutPos > 0 && f_cdcOutPos < f_cdcOutLen) {
        tud_cdc_write_flush();
    }
}

// Core 0: true while any staged bytes are still waiting to be sent.
bool LOG_CdcOutPending(void)
{
    return f_cdcOutPos < f_cdcOutLen;
}
