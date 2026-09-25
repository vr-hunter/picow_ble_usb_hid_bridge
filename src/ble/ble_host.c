#include <stddef.h>

#include "pico/cyw43_arch.h"
#include "btstack.h"

#include "ble_coordinator.h"
#include "ble_events.h"
#include "ble_host.h"
#include "indicators.h"
#include "hid_bridge.h"
#include "log_console.h"

#include "device_profile.h"

// BTstack HIDS descriptor storage: holds the HID report descriptors discovered
// from each connected device. Shared RAM, read by Core 0 via
// hid_bridge_get_report_descriptor().
static uint8_t hid_descriptor_storage[MAX_HID_DEVICES * 2048];

void ble_host_main(void)
{
    // Initialize the CYW43 driver architecture (enables BT because
    // CYW43_ENABLE_BLUETOOTH == 1 in btstack_config.h).
    if (cyw43_arch_init()) {
        BLE_LOG("failed to initialise cyw43_arch\n");
        return;
    }

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();

    att_server_init(profile_data, NULL, NULL);

    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    ble_events_init();
    ble_coordinator_init();
    indicators_init();

    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();
}

// Public cross-core accessor (declared in hid_bridge.h). Resolves the descriptor
// bytes for the device at the given compact position via the BTstack HIDS
// descriptor storage; the snapshot mapping is shared with Core 0.
const uint8_t *hid_bridge_get_report_descriptor(uint8_t position, uint16_t *len)
{
    usb_ready_snapshot_t snap;
    if (!hid_bridge_get_ready_snapshot(&snap)) return NULL;
    if (position >= snap.count) return NULL;

    uint16_t cid = snap.hids_cid[position];
    if (len) *len = hids_host_descriptor_storage_get_descriptor_len(cid, 0);
    return hids_host_descriptor_storage_get_descriptor_data(cid, 0);
}
