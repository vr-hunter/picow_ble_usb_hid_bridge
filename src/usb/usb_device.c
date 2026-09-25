#include "pico/stdlib.h"
#include "bsp/board_api.h"
#include "tusb.h"

#include "bridge.h"
#include "hid_bridge.h"
#include "log_console.h"
#include "report_queue.h"
#include "usb_device.h"
#include "usb_hid.h"

// Let the bus settle after tud_disconnect() before reconnecting.
#define USB_REINIT_STABILIZATION_DELAY_MS 100

#define HEARTBEAT_INTERVAL_MS 5000

void usb_device_init(void)
{
    // Initialize device stack on configured roothub port
    tud_init(BOARD_TUD_RHPORT);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }
}

// Main loop for the USB device (runs on Core 0).
// Loops indefinitely, handling USB events and HID report forwarding.
// Also handles USB re-initialization requests from Core 1 (BLE host).
void usb_device_main(void)
{
    SYS_LOG("Entering the USB device main loop on Core 0\n");

    while (1)
    {
#ifdef ENABLE_HEARTBEAT_LOGS
        // Periodic proof that Core 0 is still servicing its main loop.
        static uint32_t last_heartbeat = 0;
        if (board_millis() - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
            last_heartbeat = board_millis();
            SYS_LOG("Heartbeat (Core 0 running)\n");
        }
#endif

        // Handle USB re-initialization request from Core 1 without blocking
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
                    log_console_cdc_reset(); // discard partial stage; lost on re-enumeration
                    tud_disconnect();
                    usb_reinit_start_ms = board_millis();
                    usb_reinit_state = USB_REINIT_WAIT_STABILIZATION;
                } else {
                    report_queue_clear_all();
                    tud_connect();
                }
            }
        }

        if (usb_reinit_state == USB_REINIT_WAIT_STABILIZATION) {
            if (board_millis() - usb_reinit_start_ms >= USB_REINIT_STABILIZATION_DELAY_MS) {
                usb_reinit_state = USB_REINIT_IDLE;
                report_queue_clear_all();
                tud_connect();
            }
        }

        tud_task();          // Run TinyUSB device task
        usb_hid_task();      // Run HID report forwarding task

        // Stream the staged CDC log bytes (resumable across a port drop), then
        // pop the next buffered message into the staging buffer if idle.
        log_console_cdc_flush();
        if (!log_console_cdc_pending()) {
            char logbuf[LOG_CONSOLE_MSG_LEN];
            if (log_console_pop(logbuf, sizeof(logbuf))) {
                log_console_cdc_load(logbuf);
            }
        }
    }
}

// Device callbacks

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
// remote_wakeup_en : if host allows us to perform remote wakeup
// Within 7ms, device must draw an average of current less than 2.5 mA from bus
void tud_suspend_cb(bool remote_wakeup_en)
{
    USB_LOG("Bus suspended (remote wakeup %s)\n", remote_wakeup_en ? "allowed" : "denied");
    (void)remote_wakeup_en;
}

// Invoked when usb bus is resumed
void tud_resume_cb(void)
{
    USB_LOG("Bus resumed\n");
}
