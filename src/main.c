/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "bsp/board_api.h"
#include "tusb.h"
#include "Common.h"

//--------------------------------------------------------------------+
// MACROS
//--------------------------------------------------------------------+
#define USB_REINIT_STABILIZATION_DELAY 100 // ms
#define HEARTBEAT_INTERVAL 5000 // ms

//--------------------------------------------------------------------+
// GLOBAL VARIABLES
//--------------------------------------------------------------------+
volatile bool g_usb_reinit_request = false; // Flag to request USB re-initialization when BLE HID connection is established

//--------------------------------------------------------------------+
// FUNCTION PROTOTYPES
//--------------------------------------------------------------------+
void usb_dev_main(void);
void hid_task(void);
bool send_hid_report(ULONG slot);

extern void ble_host_main(void);

/*------------- MAIN -------------*/
int main(void)
{
    board_init();  

    // init device stack on configured roothub port
    tud_init(BOARD_TUD_RHPORT);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }      
    
    stdio_init_all();
    CMN_Init();

    SYS_LOG("BLE to USB HID bridge starting\n");

    // Initialize to lock out CPU Core 0 when btstack writes to flash memory on CPU Core 1
    flash_safe_execute_core_init();

    SYS_LOG("Launching the BLE host on Core 1\n");
    multicore_launch_core1(ble_host_main);

    usb_dev_main();

    return 0;
}

//--------------------------------------------------------------------+
// Main loop for the USB device (runs on Core0).
//--------------------------------------------------------------------+
// This function loops indefinitely, handling USB events and HID tasks.
// It also handles USB re-initialization requests from Core1.
void usb_dev_main(void)
{
    SYS_LOG("Entering the USB device main loop on Core 0\n");

    while (1)
    {
#ifdef ENABLE_HEARTBEAT_LOGS
        // Periodic proof that Core 0 is still servicing its main loop.
        static uint32_t last_heartbeat = 0;
        if (board_millis() - last_heartbeat >= HEARTBEAT_INTERVAL) {
            last_heartbeat = board_millis();
            SYS_LOG("Heartbeat (Core 0 running)\n");
        }
#endif

        // Handle USB re-initialization request from Core1 (BLE host) without blocking
        static enum {
            USB_REINIT_IDLE = 0,
            USB_REINIT_WAIT_STABILIZATION
        } usb_reinit_state = USB_REINIT_IDLE;
        static uint32_t usb_reinit_start_ms = 0;

        if (g_usb_reinit_request) {
            g_usb_reinit_request = false;
            USB_LOG("Re-initialization requested by the BLE host\n");
            if (usb_reinit_state == USB_REINIT_IDLE) {
                if (tud_mounted()) {
                    tud_disconnect();
                    usb_reinit_start_ms = board_millis();
                    usb_reinit_state = USB_REINIT_WAIT_STABILIZATION;
                } else {
                    CMN_ClearAllQueues();
                    tud_connect();
                }
            }
        }

        if (usb_reinit_state == USB_REINIT_WAIT_STABILIZATION) {
            if (board_millis() - usb_reinit_start_ms >= USB_REINIT_STABILIZATION_DELAY) {
                usb_reinit_state = USB_REINIT_IDLE;
                CMN_ClearAllQueues();
                tud_connect();
            }
        }

        tud_task();          // Run TinyUSB device task
        hid_task();          // Run HID report sending task
    }
}

//--------------------------------------------------------------------+
// Device callbacks
//--------------------------------------------------------------------+

// Invoked when device is mounted
void tud_mount_cb(void)
{
    USB_LOG("Device mounted\n");
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    USB_LOG("Device unmounted\n");
}

// Invoked when usb bus is suspended
// remote_wakeup_en : if host allow us  to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    USB_LOG("Bus suspended (remote wakeup %s)\n", remote_wakeup_en ? "allowed" : "denied");
    (void) remote_wakeup_en;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    USB_LOG("Bus resumed\n");
}

//--------------------------------------------------------------------+
// USB HID
//--------------------------------------------------------------------+

// Dequeue and send one HID report from the queue to the USB host.
// return true if a report was successfully sent, false otherwise.
bool send_hid_report(ULONG slot)
{
    static ST_HID_RPT stHidRpt;
    bool bRet = false;

    if (CMN_PeekQueue(slot, &stHidRpt)) {
        if ( tud_suspended()) {
            tud_remote_wakeup();
            return bRet;
        }
        if (tud_hid_n_ready((uint8_t)slot)) {
            if (tud_hid_n_report((uint8_t)slot, 0, stHidRpt.report, stHidRpt.report_len)) {
                USB_LOG("HID report sent on slot %lu (%u bytes)\n", (unsigned long)slot, stHidRpt.report_len);
                CMN_AdvanceQueue(slot);
                bRet = true;
            }
        } else {
            static uint32_t not_ready_count[CMN_QUE_KIND_NUM];
            not_ready_count[slot]++;
            if (not_ready_count[slot] <= 3 || (not_ready_count[slot] % 200) == 0) {
                USB_LOG("Slot %lu: HID not ready (%lu checks)\n", (unsigned long)slot, (unsigned long)not_ready_count[slot]);
            }
        }
    }

    return bRet;
}

//--------------------------------------------------------------------+
// HID TASK
//--------------------------------------------------------------------+
void hid_task(void)
{
    for (ULONG slot = 0; slot < CMN_QUE_KIND_NUM; slot++) {
        send_hid_report(slot);
    }
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void) instance;
    (void) len;
    (void) report;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    // TODO not Implemented
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    USB_LOG("HID SET_REPORT (id=%u type=%u size=%u)\n", report_id, report_type, bufsize);
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;
}