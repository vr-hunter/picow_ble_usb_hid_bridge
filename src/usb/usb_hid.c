#include "tusb.h"

#include "hid_bridge.h"
#include "log_console.h"
#include "report_queue.h"
#include "usb_hid.h"

// Dequeue and send one HID report from the queue to the USB host.
// Return true if a report was successfully sent, false otherwise.
// 'position' is the compact USB interface index (0..count-1); 'physical_slot'
// is the BLE slot whose report queue feeds it. The two differ after a
// lower-numbered device disconnects and the remaining ones renumber.
static bool send_hid_report(uint8_t position, uint8_t physical_slot)
{
    hid_report_t rpt;
    bool sent = false;

    if (report_queue_peek(physical_slot, &rpt)) {
        if (tud_suspended()) {
            tud_remote_wakeup();
            return sent;
        }
        if (tud_hid_n_ready(position)) {
            if (tud_hid_n_report(position, 0, rpt.report, rpt.report_len)) {
                USB_LOG("HID report sent pos %u <- slot %u (%u bytes)\n",
                        position, physical_slot, rpt.report_len);
                report_queue_advance(physical_slot);
                sent = true;
            }
        } else {
            static uint32_t not_ready_count[MAX_HID_DEVICES];
            not_ready_count[position]++;
            if (not_ready_count[position] <= 3 || (not_ready_count[position] % 200) == 0) {
                USB_LOG("Pos %u: HID not ready (%lu checks)\n",
                        position, (unsigned long)not_ready_count[position]);
            }
        }
    }

    return sent;
}

void usb_hid_task(void)
{
    usb_ready_snapshot_t snap;
    if (!hid_bridge_get_ready_snapshot(&snap)) return;
    for (uint8_t pos = 0; pos < snap.count; pos++) {
        send_hid_report(pos, snap.slot[pos]);
    }
}

// Invoked when sent REPORT successfully to host
// Application can use this to send the next report
// Note: For composite reports, report[0] is report ID
void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void)instance;
    (void)len;
    (void)report;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    // TODO not Implemented
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;

    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    USB_LOG("HID SET_REPORT (id=%u type=%u size=%u)\n", report_id, report_type, bufsize);
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}
