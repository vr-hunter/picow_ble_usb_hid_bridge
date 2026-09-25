// Connection coordinator for the BLE host. Owns the device slots and the
// scan/connect lifecycle: it decides when to scan, which advertised device to
// connect to (bonded devices always, new devices only while the pairing
// window is open), drives pairing and GATT service setup, and publishes the
// READY snapshot consumed by the USB side whenever it changes.
#include <string.h>

#include "btstack.h"

#include "ble_coordinator.h"
#include "ble_bonds.h"
#include "ble_events.h"
#include "log_console.h"
#include "report_queue.h"

#define CONNECTION_TIMEOUT_MS 3000
#define SCAN_TIMEOUT_MS       5000

// Pairing mode: a window during which the bridge discovers NEW (unbonded) HID
// devices. Bonded devices always auto-reconnect, independent of this window.
#define PAIRING_MODE_DURATION_MS 10000

#define CONN_INTERVAL_MIN_UNITS 10
#define CONN_INTERVAL_MAX_UNITS 12
#define CONN_LATENCY_EVENTS     0
#define CONN_TIMEOUT_UNITS      300

#define MAX_PENDING_RPA_LOOKUPS 4

// Debug scaffolding: set to 1 to compile in the per-advertisement scan log and
// the 5s coordinator/slot heartbeat. 0 (default) keeps the runtime log clean.
#define BLE_HOST_DEBUG 0
#if BLE_HOST_DEBUG
#define HEARTBEAT_INTERVAL_MS 5000
#endif

typedef struct {
    bd_addr_t addr;
    bool has_hid_service;
    bool in_use;
} pending_rpa_lookup_t;

typedef enum {
    SLOT_IDLE = 0,
    SLOT_CONNECTING,
    SLOT_READY,
} slot_state_t;

typedef struct {
    slot_state_t state;
    hci_con_handle_t con_handle;
    uint16_t hids_cid;
    bd_addr_t adv_addr;
    bd_addr_type_t adv_addr_type;
} hid_slot_t;

typedef enum {
    COORD_SCANNING,
    COORD_CONNECTING,
    COORD_ALL_READY,
} coord_state_t;

//--------------------------------------------------------------------+
// GLOBAL & STATIC VARIABLES
//--------------------------------------------------------------------+
static hid_slot_t slots[MAX_HID_DEVICES];
static coord_state_t coord_state;
static int connecting_slot = -1;

static pending_rpa_lookup_t pending_rpa_lookups[MAX_PENDING_RPA_LOOKUPS];
static uint8_t pending_rpa_lookup_index = 0;

static btstack_timer_source_t connection_timer;
#if BLE_HOST_DEBUG
static btstack_timer_source_t heartbeat_timer;
#endif

// Pairing mode window. Non-zero end timestamp = window active.
static uint32_t pairing_mode_end_ms = 0;

extern volatile bool g_usb_reinit_request;

//--------------------------------------------------------------------+
// FUNCTION PROTOTYPES
//--------------------------------------------------------------------+
static void coordinator_start_scan(void);
static void coordinator_maybe_start_scan(void);
static void pairing_mode_enter(void);
static void pairing_mode_exit(void);
static bool pairing_mode_active(void);
static void coordinator_on_connect_success(int slot);
static void coordinator_on_connect_fail(int slot);
static void coordinator_on_disconnect(int slot);
static void coordinator_publish_snapshot(void);
static int find_first_idle_slot(void);
static int find_slot_by_cid(uint16_t cid);
static int find_slot_by_con_handle(hci_con_handle_t con_handle);
static bool any_slot_idle(void);
static void coordinator_connect(int slot);
static void coordinator_begin_connect(const bd_addr_t adv_addr, bd_addr_type_t adv_addr_type);
static void coordinator_scan_timeout(btstack_timer_source_t *ts);
static void coordinator_connection_timeout(btstack_timer_source_t *ts);
static void coordinator_handle_connection_error(int slot);
static void coordinator_request_connection_parameters(int slot);
static void coordinator_connect_to_hid_service(int slot);
static void coordinator_handle_hid_report(uint16_t cid, uint8_t report_id, const uint8_t *report, uint16_t report_len);
#if BLE_HOST_DEBUG
static void heartbeat_handler(btstack_timer_source_t *ts);
#endif

static bool is_resolvable_private_address(bd_addr_type_t addr_type, const bd_addr_t addr);
static bool adv_event_contains_hid_service(const uint8_t *packet);
static bool is_rpa_lookup_pending(const bd_addr_t addr);
static void clear_all_pending_rpa_lookups(void);
static void clear_pending_rpa_lookup(const bd_addr_t addr);
static void track_pending_rpa_lookup(const bd_addr_t addr, bool has_hid_service);
static bool get_and_clear_pending_rpa_has_hid_service(const bd_addr_t addr);

//--------------------------------------------------------------------+
// SLOT LOOKUP
//--------------------------------------------------------------------+

static int find_first_idle_slot(void)
{
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (slots[i].state == SLOT_IDLE) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_cid(uint16_t cid)
{
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (slots[i].state != SLOT_IDLE && slots[i].hids_cid == cid) {
            return i;
        }
    }
    return -1;
}

static int find_slot_by_con_handle(hci_con_handle_t con_handle)
{
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (slots[i].state != SLOT_IDLE && slots[i].con_handle == con_handle) {
            return i;
        }
    }
    return -1;
}

static bool any_slot_idle(void)
{
    return find_first_idle_slot() >= 0;
}

//--------------------------------------------------------------------+
// COORDINATOR
//--------------------------------------------------------------------+

static void coordinator_publish_snapshot(void)
{
    usb_ready_snapshot_t snap;
    snap.count = 0;
    for (int i = 0; i < MAX_HID_DEVICES && snap.count < MAX_HID_DEVICES; i++) {
        if (slots[i].state == SLOT_READY) {
            snap.hids_cid[snap.count] = slots[i].hids_cid;
            snap.report_len[snap.count] = hids_host_descriptor_storage_get_descriptor_len(slots[i].hids_cid, 0);
            snap.slot[snap.count] = (uint8_t)i;
            snap.count++;
        }
    }
    hid_bridge_publish_ready_snapshot(&snap);
    g_usb_reinit_request = true;
}

static void coordinator_on_connect_success(int slot)
{
    (void)slot;
    coordinator_publish_snapshot();
    coordinator_maybe_start_scan();
}

static void coordinator_on_connect_fail(int slot)
{
    slots[slot].state = SLOT_IDLE;
    slots[slot].con_handle = HCI_CON_HANDLE_INVALID;
    slots[slot].hids_cid = 0;
    if (connecting_slot == slot) {
        connecting_slot = -1;
    }
    coordinator_publish_snapshot();
    coordinator_maybe_start_scan();
}

static void coordinator_on_disconnect(int slot)
{
    (void)slot;
    coordinator_publish_snapshot();
    coordinator_maybe_start_scan();
}

static bool pairing_mode_active(void)
{
    return pairing_mode_end_ms != 0
        && btstack_run_loop_get_time_ms() < pairing_mode_end_ms;
}

static void pairing_mode_enter(void)
{
    pairing_mode_end_ms = btstack_run_loop_get_time_ms() + PAIRING_MODE_DURATION_MS;
    BLE_LOG("Pairing mode: %dms window started\n", PAIRING_MODE_DURATION_MS);
    // Don't disturb a connection in progress; the post-connect handler re-evaluates.
    if (coord_state != COORD_CONNECTING) coordinator_maybe_start_scan();
}

static void pairing_mode_exit(void)
{
    pairing_mode_end_ms = 0;
    BLE_LOG("Pairing mode ended\n");
    if (coord_state != COORD_CONNECTING) coordinator_maybe_start_scan();
}

// Decide whether to (re)start scanning right now. Scanning is needed to (a)
// reconnect bonded devices (always) and (b) discover new devices (pairing only).
// With neither, stay idle and stop any active scan to save RF/power. Called from
// the coordinator handlers (which own the connect lifecycle) and from the pairing
// enter/exit paths (already guarded against an in-flight connection).
static void coordinator_maybe_start_scan(void)
{
    if (!any_slot_idle()) { coord_state = COORD_ALL_READY; return; }
    if (pairing_mode_active() || ble_bonds_has_device()) {
        coordinator_start_scan();
    } else {
        gap_stop_scan();
        coord_state = COORD_ALL_READY;
    }
}

//--------------------------------------------------------------------+
// CONNECTION & SCANNING CONTROL
//--------------------------------------------------------------------+

static void coordinator_connect(int slot)
{
    BLE_LOG("Connecting slot %d to %s (type %u, Timeout %dms)...\n",
            slot, bd_addr_to_str(slots[slot].adv_addr), slots[slot].adv_addr_type, CONNECTION_TIMEOUT_MS);

    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &coordinator_connection_timeout);
    btstack_run_loop_add_timer(&connection_timer);

    gap_connect(slots[slot].adv_addr, slots[slot].adv_addr_type);
}

// Claim an idle slot for the given address and start connecting to it.
static void coordinator_begin_connect(const bd_addr_t adv_addr, bd_addr_type_t adv_addr_type)
{
    int slot = find_first_idle_slot();
    if (slot < 0) return;
    btstack_run_loop_remove_timer(&connection_timer);
    gap_stop_scan();
    memcpy(slots[slot].adv_addr, adv_addr, 6);
    slots[slot].adv_addr_type = adv_addr_type;
    slots[slot].state = SLOT_CONNECTING;
    connecting_slot = slot;
    coord_state = COORD_CONNECTING;
    coordinator_connect(slot);
}

static void coordinator_start_scan(void)
{
    BLE_LOG("Scanning for LE HID devices (Timeout %dms)...\n", SCAN_TIMEOUT_MS);
    coord_state = COORD_SCANNING;

    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, SCAN_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &coordinator_scan_timeout);
    btstack_run_loop_add_timer(&connection_timer);

    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void coordinator_scan_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    if (coord_state != COORD_SCANNING) return;
    if (!pairing_mode_active() && !ble_bonds_has_device()) {
        // Nothing left to find (pairing ended, no bonded devices): stop scanning.
        gap_stop_scan();
        coord_state = COORD_ALL_READY;
        return;
    }
    BLE_LOG("Scan timeout. Refreshing scan...\n");
    coordinator_start_scan();
}

static void coordinator_connection_timeout(btstack_timer_source_t *ts)
{
    (void)ts;
    if (connecting_slot < 0) return;
    BLE_LOG("Connection timeout for slot %d. Cancelling.\n", connecting_slot);
    gap_connect_cancel();
    coordinator_on_connect_fail(connecting_slot);
}

static void coordinator_handle_connection_error(int slot)
{
    BLE_LOG("Error on slot %d, disconnecting\n", slot);
    if (slots[slot].con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(slots[slot].con_handle);
    } else {
        coordinator_on_connect_fail(slot);
    }
}

static void coordinator_request_connection_parameters(int slot)
{
    if (slots[slot].con_handle == HCI_CON_HANDLE_INVALID) return;
    gap_request_connection_parameter_update(slots[slot].con_handle,
                                             CONN_INTERVAL_MIN_UNITS,
                                             CONN_INTERVAL_MAX_UNITS,
                                             CONN_LATENCY_EVENTS,
                                             CONN_TIMEOUT_UNITS);
}

static void coordinator_connect_to_hid_service(int slot)
{
    BLE_LOG("Search for HID service (slot %d).\n", slot);
    uint8_t status = hids_host_connect(slots[slot].con_handle, ble_events_on_hid_service,
                                        HID_PROTOCOL_MODE_REPORT, &slots[slot].hids_cid);
    if (status != ERROR_CODE_SUCCESS) {
        BLE_LOG("hids_host_connect failed (0x%02x)\n", status);
        coordinator_on_connect_fail(slot);
    }
}

//--------------------------------------------------------------------+
// ADDRESS HELPERS
//--------------------------------------------------------------------+

static bool is_resolvable_private_address(bd_addr_type_t addr_type, const bd_addr_t addr)
{
    return (addr_type == BD_ADDR_TYPE_LE_RANDOM) && ((addr[0] & 0xC0) == 0x40);
}

static bool adv_event_contains_hid_service(const uint8_t *packet)
{
    const uint8_t *ad_data = gap_event_advertising_report_get_data(packet);
    uint8_t ad_len = gap_event_advertising_report_get_data_length(packet);
    if (ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE)) {
        return true;
    }
    for (int i = 0; i + 3 <= ad_len;) {
        uint8_t len = ad_data[i];
        if (len == 0) break;
        if (i + 1 + len > ad_len) break;
        uint8_t type = ad_data[i + 1];
        if (type == 0x19 && len >= 3) {
            uint16_t appearance = (uint16_t)ad_data[i + 2] | ((uint16_t)ad_data[i + 3] << 8);
            if (appearance >= 0x03C0 && appearance <= 0x03CF) {
                return true;
            }
        }
        i += 1 + len;
    }
    return false;
}

static bool is_rpa_lookup_pending(const bd_addr_t addr)
{
    for (int i = 0; i < MAX_PENDING_RPA_LOOKUPS; i++) {
        if (pending_rpa_lookups[i].in_use && memcmp(pending_rpa_lookups[i].addr, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}

static void clear_all_pending_rpa_lookups(void)
{
    for (int i = 0; i < MAX_PENDING_RPA_LOOKUPS; i++) {
        pending_rpa_lookups[i].in_use = false;
    }
}

static void clear_pending_rpa_lookup(const bd_addr_t addr)
{
    for (int i = 0; i < MAX_PENDING_RPA_LOOKUPS; i++) {
        if (pending_rpa_lookups[i].in_use && memcmp(pending_rpa_lookups[i].addr, addr, 6) == 0) {
            pending_rpa_lookups[i].in_use = false;
        }
    }
}

static void track_pending_rpa_lookup(const bd_addr_t addr, bool has_hid_service)
{
    for (int i = 0; i < MAX_PENDING_RPA_LOOKUPS; i++) {
        if (pending_rpa_lookups[i].in_use && memcmp(pending_rpa_lookups[i].addr, addr, 6) == 0) {
            pending_rpa_lookups[i].has_hid_service = has_hid_service;
            return;
        }
    }
    memcpy(pending_rpa_lookups[pending_rpa_lookup_index].addr, addr, 6);
    pending_rpa_lookups[pending_rpa_lookup_index].has_hid_service = has_hid_service;
    pending_rpa_lookups[pending_rpa_lookup_index].in_use = true;
    pending_rpa_lookup_index = (pending_rpa_lookup_index + 1) % MAX_PENDING_RPA_LOOKUPS;
}

static bool get_and_clear_pending_rpa_has_hid_service(const bd_addr_t addr)
{
    for (int i = 0; i < MAX_PENDING_RPA_LOOKUPS; i++) {
        if (pending_rpa_lookups[i].in_use && memcmp(pending_rpa_lookups[i].addr, addr, 6) == 0) {
            pending_rpa_lookups[i].in_use = false;
            return pending_rpa_lookups[i].has_hid_service;
        }
    }
    return false;
}

//--------------------------------------------------------------------+
// HID REPORT HANDLING
//--------------------------------------------------------------------+

static void coordinator_handle_hid_report(uint16_t cid, uint8_t report_id,
                                          const uint8_t *report, uint16_t report_len)
{
    int slot = find_slot_by_cid(cid);
    if (slot < 0 || slots[slot].state != SLOT_READY) return;

    static uint32_t report_count[MAX_HID_DEVICES];
    report_count[slot]++;
    if (report_count[slot] <= 3 || (report_count[slot] % 100) == 0) {
        BLE_LOG("Slot %d: BLE report #%lu id=%u len=%u data=[%02x %02x %02x %02x]\n",
                slot, (unsigned long)report_count[slot], report_id, report_len,
                report_len > 0 ? report[0] : 0,
                report_len > 1 ? report[1] : 0,
                report_len > 2 ? report[2] : 0,
                report_len > 3 ? report[3] : 0);
    }

    hid_report_t rpt;
    rpt.report_id  = report_id;
    rpt.report_len = report_len;
    if (rpt.report_len > REPORT_DATA_SIZE) {
        rpt.report_len = REPORT_DATA_SIZE;
    }
    memcpy(rpt.report, report, rpt.report_len);
    if (!report_queue_push((uint8_t)slot, &rpt)) {
        // Queue full, drop
    }
}

//--------------------------------------------------------------------+
// PUBLIC API
//--------------------------------------------------------------------+

void ble_coordinator_init(void)
{
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        slots[i].state = SLOT_IDLE;
        slots[i].con_handle = HCI_CON_HANDLE_INVALID;
        slots[i].hids_cid = 0;
    }
    connecting_slot = -1;
    coord_state = COORD_ALL_READY;

#if BLE_HOST_DEBUG
    btstack_run_loop_set_timer_handler(&heartbeat_timer, &heartbeat_handler);
    btstack_run_loop_set_timer(&heartbeat_timer, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(&heartbeat_timer);
#endif
}

void ble_coordinator_on_btstack_ready(void)
{
    ble_bonds_load();
    // Enter pairing mode on boot so the bridge immediately scans for both bonded
    // (auto-reconnect) and new (pair) HID devices, then re-evaluates when the
    // window expires.
    pairing_mode_enter();
    coordinator_maybe_start_scan();
}

void ble_coordinator_on_advertising_report(const uint8_t *packet)
{
    if (coord_state != COORD_SCANNING) return;

    bd_addr_t adv_addr;
    gap_event_advertising_report_get_address(packet, adv_addr);
    bd_addr_type_t raw_addr_type = gap_event_advertising_report_get_address_type(packet);
    bd_addr_type_t adv_addr_type = (bd_addr_type_t)(raw_addr_type & 1);
    bool has_hid_service = adv_event_contains_hid_service(packet);

#if BLE_HOST_DEBUG
    // Diagnostic: log every advertisement (rate-limited to ~1/s) so
    // we can see exactly what the scan is receiving after a
    // disconnect — is the device re-advertising, and with what
    // address / type / HID flag / bonded match?
    {
        static uint32_t s_last_adv_diag_ms = 0;
        uint32_t now_ms = btstack_run_loop_get_time_ms();
        if (now_ms - s_last_adv_diag_ms >= 1000) {
            s_last_adv_diag_ms = now_ms;
            BLE_LOG("ADV %s type %u hid=%d bond=%d hb=%d n=%d\n",
                    bd_addr_to_str(adv_addr), adv_addr_type,
                    (int)has_hid_service, ble_bonds_find(adv_addr),
                    (int)ble_bonds_has_device(), ble_bonds_count());
        }
    }
#endif

    if (ble_bonds_has_device() && is_resolvable_private_address(adv_addr_type, adv_addr)) {
        if (!is_rpa_lookup_pending(adv_addr)) {
            track_pending_rpa_lookup(adv_addr, has_hid_service);
            sm_address_resolution_lookup(adv_addr_type, adv_addr);
        }
        return;
    }

    if (ble_bonds_has_device() && ble_bonds_find(adv_addr) >= 0) {
        int slot = find_first_idle_slot();
        if (slot < 0) return;
        BLE_LOG("Found bonded device %s directly (slot %d), connecting...\n",
                bd_addr_to_str(adv_addr), slot);
        coordinator_begin_connect(adv_addr, adv_addr_type);
        return;
    }

    // New (unbonded) HID devices are only accepted while the pairing window is
    // open. Known devices reconnect via the two bond branches above (address
    // match / RPA resolution), so this fallback must NOT fire on bonded devices
    // alone -- that flag is global and would accept any stranger.
    if (has_hid_service && pairing_mode_active()) {
        int slot = find_first_idle_slot();
        if (slot < 0) return;
        BLE_LOG("Found HID device %s (type %u, slot %d), connecting...\n",
                bd_addr_to_str(adv_addr), adv_addr_type, slot);
        coordinator_begin_connect(adv_addr, adv_addr_type);
    }
}

void ble_coordinator_on_disconnection(hci_con_handle_t con_handle, uint8_t reason)
{
    int slot = find_slot_by_con_handle(con_handle);
    if (slot < 0) {
        BLE_LOG("HCI disconnect (con_handle 0x%04x, reason 0x%02x) did not match any slot\n",
                con_handle, reason);
        return;
    }

    BLE_LOG("Slot %d disconnected (Reason: 0x%02x)\n", slot, reason);

    btstack_run_loop_remove_timer(&connection_timer);
    clear_all_pending_rpa_lookups();

    slots[slot].state = SLOT_IDLE;
    slots[slot].con_handle = HCI_CON_HANDLE_INVALID;
    slots[slot].hids_cid = 0;

    if (connecting_slot == slot) {
        connecting_slot = -1;
    }

    coordinator_on_disconnect(slot);
}

void ble_coordinator_on_connection_complete(uint8_t status, hci_con_handle_t con_handle)
{
    if (connecting_slot < 0 || slots[connecting_slot].state != SLOT_CONNECTING) return;
    btstack_run_loop_remove_timer(&connection_timer);

    if (status != ERROR_CODE_SUCCESS) {
        BLE_LOG("LE Connection failed (Status: 0x%02x)\n", status);
        coordinator_on_connect_fail(connecting_slot);
        return;
    }

    int slot = connecting_slot;
    slots[slot].con_handle = con_handle;
    sm_request_pairing(slots[slot].con_handle);
}

void ble_coordinator_on_pairing_complete(uint8_t status)
{
    int slot = connecting_slot;
    switch (status) {
        case ERROR_CODE_SUCCESS:
            BLE_LOG("Pairing complete, success\n");
            coordinator_request_connection_parameters(slot);
            coordinator_connect_to_hid_service(slot);
            break;
        case ERROR_CODE_CONNECTION_TIMEOUT:
            BLE_LOG("Pairing failed, timeout\n");
            coordinator_handle_connection_error(slot);
            break;
        case ERROR_CODE_PIN_OR_KEY_MISSING:
        case ERROR_CODE_AUTHENTICATION_FAILURE:
            BLE_LOG("Pairing failed (status 0x%02x). Clearing bond & restarting...\n", status);
            ble_bonds_forget();
            gap_delete_bonding(slots[slot].adv_addr_type, slots[slot].adv_addr);
            coordinator_handle_connection_error(slot);
            break;
        default:
            BLE_LOG("Pairing failed, status 0x%02x\n", status);
            coordinator_handle_connection_error(slot);
            break;
    }
}

void ble_coordinator_on_reencryption_complete(void)
{
    BLE_LOG("Re-encryption complete\n");
    int slot = connecting_slot;
    coordinator_request_connection_parameters(slot);
    coordinator_connect_to_hid_service(slot);
}

void ble_coordinator_on_rpa_resolved(bd_addr_type_t rpa_addr_type, const bd_addr_t rpa_addr,
                                     const bd_addr_t identity_addr)
{
    clear_pending_rpa_lookup(rpa_addr);

    BLE_LOG("SM: RPA %s resolved to Identity %s\n",
            bd_addr_to_str(rpa_addr), bd_addr_to_str(identity_addr));

    if (ble_bonds_has_device() && ble_bonds_find(identity_addr) >= 0) {
        if (coord_state == COORD_SCANNING) {
            int s = find_first_idle_slot();
            if (s >= 0) {
                BLE_LOG("Resolved RPA matches bonded peripheral! Connecting to slot %d...\n", s);
                coordinator_begin_connect(rpa_addr, rpa_addr_type);
            }
        }
    }
}

void ble_coordinator_on_rpa_resolve_failed(bd_addr_type_t rpa_addr_type, const bd_addr_t rpa_addr)
{
    bool adv_had_hid = get_and_clear_pending_rpa_has_hid_service(rpa_addr);

    if (adv_had_hid && pairing_mode_active() && coord_state == COORD_SCANNING) {
        int s = find_first_idle_slot();
        if (s >= 0) {
            BLE_LOG("SM: Unresolved RPA %s has HID service, connecting as new device (slot %d)...\n",
                    bd_addr_to_str(rpa_addr), s);
            coordinator_begin_connect(rpa_addr, rpa_addr_type);
        }
    }
}

void ble_coordinator_on_hid_service_connected(uint16_t cid, uint8_t status, uint16_t num_instances)
{
    int slot = find_slot_by_cid(cid);
    if (slot < 0) return;

    switch (status) {
        case ERROR_CODE_SUCCESS:
            BLE_LOG("Slot %d: HID service connected, %d services\n", slot, num_instances);

            if (ble_bonds_find(slots[slot].adv_addr) < 0) {
                ble_bonds_add(slots[slot].adv_addr, slots[slot].adv_addr_type);
            }
            ble_bonds_save();

            slots[slot].state = SLOT_READY;
            coordinator_on_connect_success(slot);
            break;
        default:
            BLE_LOG("Slot %d: HID service connection failed, status 0x%02x.\n", slot, status);
            coordinator_on_connect_fail(slot);
            break;
    }
}

void ble_coordinator_on_hid_service_disconnected(uint16_t cid)
{
    int slot = find_slot_by_cid(cid);
    if (slot < 0) return;

    BLE_LOG("Slot %d: HID service disconnected\n", slot);
    // Reliable disconnect signal: free the slot even if the raw HCI
    // disconnection complete did not match (e.g. con_handle mismatch),
    // otherwise the slot stays SLOT_READY and no reconnect is possible.
    if (slots[slot].state != SLOT_IDLE) {
        slots[slot].state = SLOT_IDLE;
        slots[slot].con_handle = HCI_CON_HANDLE_INVALID;
        slots[slot].hids_cid = 0;
        if (connecting_slot == slot) {
            connecting_slot = -1;
        }
        coordinator_on_disconnect(slot);
    }
}

void ble_coordinator_on_hid_report(uint16_t cid, uint8_t report_id,
                                   const uint8_t *report, uint16_t report_len)
{
    coordinator_handle_hid_report(cid, report_id, report, report_len);
}

void ble_coordinator_pairing_enter(void)
{
    pairing_mode_enter();
}

void ble_coordinator_pairing_exit(void)
{
    pairing_mode_exit();
}

bool ble_coordinator_pairing_active(void)
{
    return pairing_mode_active();
}

#if BLE_HOST_DEBUG
// Periodic diagnostic: proves Core 1 is alive and the CDC/log-replay path works,
// and shows the coordinator + per-slot state so we can see whether a dropped
// device's slot is ever freed (SLOT_READY stuck = link loss not detected).
static void heartbeat_handler(btstack_timer_source_t *ts)
{
    BLE_LOG("HB coord=%d | s0:%d ch=0x%04x | s1:%d ch=0x%04x\n",
            (int)coord_state,
            (int)slots[0].state, slots[0].con_handle,
            (int)slots[1].state, slots[1].con_handle);

    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}
#endif
