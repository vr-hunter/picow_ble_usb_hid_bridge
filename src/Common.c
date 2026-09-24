// Copyright © 2025 Shiomachi Software. All rights reserved.
#include "Common.h" 

// [File Scope Variables]
static ST_QUE f_astQue[CMN_QUE_KIND_NUM] = {0}; // Array of queue control structures
static ST_HID_RPT f_astQueData_hid[MAX_HID_DEVICES][CMN_QUE_DATA_MAX_HID_RPT] = {0}; // Per-slot HID ring buffers
static usb_ready_snapshot_t f_readySnapshot = {0}; // Current READY-set snapshot
static critical_section_t f_stSpinLock = {0}; // Spinlock structure

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
