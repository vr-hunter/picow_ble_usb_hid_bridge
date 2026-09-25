/*
 * Copyright (C) 2020 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "hog_host_demo.c"

#include <inttypes.h>
#include <stdio.h>
#include <btstack_tlv.h>

#include "btstack_config.h"
#include "btstack.h"

#include "hog_host_demo.h"
#include "bt_example_common.h"
#include "pico/cyw43_arch.h"
#include "Common.h"
#include "hid_bridge.h"

//--------------------------------------------------------------------+
// CONSTANTS & TYPES
//--------------------------------------------------------------------+
#define CONNECTION_TIMEOUT_MS 3000
#define SCAN_TIMEOUT_MS       5000
#define RECONNECT_DELAY_MS    300
#define LED_BLINKING_INTERVAL_MS 200

#define CONN_INTERVAL_MIN_UNITS 10
#define CONN_INTERVAL_MAX_UNITS 12
#define CONN_LATENCY_EVENTS     0
#define CONN_TIMEOUT_UNITS      300

#define TLV_TAG_HOGD ((((uint32_t) 'H') << 24) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')

#define MAX_PENDING_RPA_LOOKUPS 4

// Debug scaffolding: set to 1 to compile in the per-advertisement scan log and
// the 5s coordinator/slot heartbeat. 0 (default) keeps the runtime log clean.
#define HOG_HOST_DEBUG 0

typedef struct {
    bd_addr_t addr;
    bool has_hid_service;
    bool in_use;
} pending_rpa_lookup_t;

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} le_device_addr_t;

typedef struct {
    uint8_t count;
    le_device_addr_t entries[MAX_KNOWN_DEVICES];
} bonded_list_t;

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

static bonded_list_t bonded_list;
static bool has_bonded_device = false;

static pending_rpa_lookup_t pending_rpa_lookups[MAX_PENDING_RPA_LOOKUPS];
static uint8_t pending_rpa_lookup_index = 0;

static uint8_t hid_descriptor_storage[MAX_HID_DEVICES * 2048];

static btstack_timer_source_t connection_timer;
static btstack_timer_source_t led_timer;
#if HOG_HOST_DEBUG
static btstack_timer_source_t heartbeat_timer;

#define HEARTBEAT_INTERVAL_MS 5000
#endif

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static const btstack_tlv_t * btstack_tlv_singleton_impl;
static void * btstack_tlv_singleton_context;

extern volatile bool g_usb_reinit_request;

//--------------------------------------------------------------------+
// FUNCTION PROTOTYPES
//--------------------------------------------------------------------+
void ble_host_main(void);

int btstack_main(int argc, const char * argv[]);

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static void coordinator_start_scan(void);
static void coordinator_on_connect_success(int slot);
static void coordinator_on_connect_fail(int slot);
static void coordinator_on_disconnect(int slot);
static void coordinator_publish_snapshot(void);
static int find_first_idle_slot(void);
static int find_slot_by_cid(uint16_t cid);
static int find_slot_by_con_handle(hci_con_handle_t con_handle);
static bool any_slot_idle(void);

static void hog_start_connect(void);
static void hog_connect(int slot);
static void hog_start_scan(void);
static void hog_scan_timeout(btstack_timer_source_t * ts);
static void hog_connection_timeout(btstack_timer_source_t * ts);
static void hog_reconnect_timeout(btstack_timer_source_t * ts);
#if HOG_HOST_DEBUG
static void heartbeat_handler(btstack_timer_source_t * ts);
#endif
static void handle_outgoing_connection_error(int slot);
static void request_hid_connection_parameters(int slot);

static void load_bonded_list(void);
static void save_bonded_list(void);
static int find_bonded_entry(const bd_addr_t addr);
static void add_bonded_entry(const bd_addr_t addr, bd_addr_type_t addr_type);
static void clear_bonded_list(void);

static bool is_resolvable_private_address(bd_addr_type_t addr_type, const bd_addr_t addr);
static bool adv_event_contains_hid_service(const uint8_t * packet);
static bool is_rpa_lookup_pending(const bd_addr_t addr);
static void clear_all_pending_rpa_lookups(void);
static void clear_pending_rpa_lookup(const bd_addr_t addr);
static void track_pending_rpa_lookup(const bd_addr_t addr, bool has_hid_service);
static bool get_and_clear_pending_rpa_has_hid_service(const bd_addr_t addr);

static void hid_handle_input_report(uint16_t cid, uint8_t service_index, uint8_t report_id, const uint8_t * report, uint16_t report_len);

const uint8_t *hid_bridge_get_report_descriptor(uint8_t position, uint16_t *len);

static void led_timer_handler(btstack_timer_source_t * ts);

//--------------------------------------------------------------------+
// PUBLIC APIs & ENTRY POINTS
//--------------------------------------------------------------------+

void ble_host_main(void)
{
    (void)ble_bridge_bt_example_init();
    ble_bridge_bt_example_main();
    btstack_run_loop_execute();
}

//--------------------------------------------------------------------+
// INITIALIZATION
//--------------------------------------------------------------------+

int btstack_main(int argc, const char * argv[])
{
    (void)argc;
    (void)argv;

    l2cap_init();

    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);

    gatt_client_init();

    att_server_init(profile_data, NULL, NULL);

    hids_host_init(hid_descriptor_storage, sizeof(hid_descriptor_storage));

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);

    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        slots[i].state = SLOT_IDLE;
        slots[i].con_handle = HCI_CON_HANDLE_INVALID;
        slots[i].hids_cid = 0;
    }
    bonded_list.count = 0;
    has_bonded_device = false;

    btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
    btstack_run_loop_set_timer(&led_timer, LED_BLINKING_INTERVAL_MS);
    btstack_run_loop_add_timer(&led_timer);

#if HOG_HOST_DEBUG
    btstack_run_loop_set_timer_handler(&heartbeat_timer, &heartbeat_handler);
    btstack_run_loop_set_timer(&heartbeat_timer, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(&heartbeat_timer);
#endif

    hci_power_control(HCI_POWER_ON);
    return 0;
}

//--------------------------------------------------------------------+
// EVENT HANDLERS
//--------------------------------------------------------------------+

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);
    uint8_t event;

    switch (packet_type) {
        case HCI_EVENT_PACKET:
            event = hci_event_packet_get_type(packet);
            switch (event) {
                case BTSTACK_EVENT_STATE:
                    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
                    hog_start_connect();
                    break;
                case GAP_EVENT_ADVERTISING_REPORT:
                {
                    if (coord_state != COORD_SCANNING) break;

                    bd_addr_t adv_addr;
                    gap_event_advertising_report_get_address(packet, adv_addr);
                    bd_addr_type_t raw_addr_type = gap_event_advertising_report_get_address_type(packet);
                    bd_addr_type_t adv_addr_type = (bd_addr_type_t)(raw_addr_type & 1);
                    bool has_hid_service = adv_event_contains_hid_service(packet);

#if HOG_HOST_DEBUG
                    // Diagnostic: log every advertisement (rate-limited to ~1/s) so
                    // we can see exactly what the scan is receiving after a
                    // disconnect — is the device re-advertising, and with what
                    // address / type / HID flag / bonded match?
                    {
                        static uint32_t s_last_adv_diag_ms = 0;
                        uint32_t now_ms = btstack_run_loop_get_time_ms();
                        if (now_ms - s_last_adv_diag_ms >= 1000) {
                            s_last_adv_diag_ms = now_ms;
                            int bond_idx = find_bonded_entry(adv_addr);
                            BLE_LOG("ADV %s type %u hid=%d bond=%d hb=%d n=%d\n",
                                    bd_addr_to_str(adv_addr), adv_addr_type,
                                    (int)has_hid_service, bond_idx,
                                    (int)has_bonded_device, bonded_list.count);
                        }
                    }
#endif

                    if (has_bonded_device && is_resolvable_private_address(adv_addr_type, adv_addr)) {
                        if (!is_rpa_lookup_pending(adv_addr)) {
                            track_pending_rpa_lookup(adv_addr, has_hid_service);
                            sm_address_resolution_lookup(adv_addr_type, adv_addr);
                        }
                        break;
                    }

                    if (has_bonded_device && find_bonded_entry(adv_addr) >= 0) {
                        int slot = find_first_idle_slot();
                        if (slot < 0) break;
                        BLE_LOG("Found bonded device %s directly (slot %d), connecting...\n",
                                bd_addr_to_str(adv_addr), slot);
                        btstack_run_loop_remove_timer(&connection_timer);
                        gap_stop_scan();
                        memcpy(slots[slot].adv_addr, adv_addr, 6);
                        slots[slot].adv_addr_type = adv_addr_type;
                        slots[slot].state = SLOT_CONNECTING;
                        connecting_slot = slot;
                        coord_state = COORD_CONNECTING;
                        hog_connect(slot);
                        break;
                    }

                    if (has_hid_service) {
                        int slot = find_first_idle_slot();
                        if (slot < 0) break;
                        BLE_LOG("Found HID device %s (type %u, slot %d), connecting...\n",
                                bd_addr_to_str(adv_addr), adv_addr_type, slot);
                        btstack_run_loop_remove_timer(&connection_timer);
                        gap_stop_scan();
                        memcpy(slots[slot].adv_addr, adv_addr, 6);
                        slots[slot].adv_addr_type = adv_addr_type;
                        slots[slot].state = SLOT_CONNECTING;
                        connecting_slot = slot;
                        coord_state = COORD_CONNECTING;
                        hog_connect(slot);
                        break;
                    }
                    break;
                }
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                {
                    uint16_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
                    uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
                    int slot = find_slot_by_con_handle(con_handle);
                    if (slot < 0) {
                        BLE_LOG("HCI disconnect (con_handle 0x%04x, reason 0x%02x) did not match any slot\n",
                                con_handle, reason);
                        break;
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
                    break;
                }
                case HCI_EVENT_META_GAP:
                    if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
                    if (connecting_slot < 0 || slots[connecting_slot].state != SLOT_CONNECTING) return;
                    btstack_run_loop_remove_timer(&connection_timer);
                    {
                        uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
                        if (status != ERROR_CODE_SUCCESS) {
                            BLE_LOG("LE Connection failed (Status: 0x%02x)\n", status);
                            coordinator_on_connect_fail(connecting_slot);
                            break;
                        }
                    }
                    int slot = connecting_slot;
                    slots[slot].con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                    sm_request_pairing(slots[slot].con_handle);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;

    bool connect_to_service = false;
    int slot = connecting_slot;

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_JUST_WORKS_REQUEST:
            BLE_LOG("Just works requested\n");
            sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            BLE_LOG("Confirming numeric comparison: %06" PRIu32 "\n", sm_event_numeric_comparison_request_get_passkey(packet));
            sm_numeric_comparison_confirm(sm_event_numeric_comparison_request_get_handle(packet));
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            BLE_LOG("Display Passkey: %06" PRIu32 "\n", sm_event_passkey_display_number_get_passkey(packet));
            break;
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
        {
            bd_addr_t resolved_identity_addr;
            bd_addr_t rpa_addr;
            sm_event_identity_resolving_succeeded_get_identity_address(packet, resolved_identity_addr);
            sm_event_identity_resolving_succeeded_get_address(packet, rpa_addr);
            bd_addr_type_t rpa_addr_type = (bd_addr_type_t)(sm_event_identity_resolving_succeeded_get_addr_type(packet) & 1);

            clear_pending_rpa_lookup(rpa_addr);

            BLE_LOG("SM: RPA %s resolved to Identity %s\n",
                    bd_addr_to_str(rpa_addr), bd_addr_to_str(resolved_identity_addr));

            if (has_bonded_device && find_bonded_entry(resolved_identity_addr) >= 0) {
                if (coord_state == COORD_SCANNING) {
                    int s = find_first_idle_slot();
                    if (s >= 0) {
                        BLE_LOG("Resolved RPA matches bonded peripheral! Connecting to slot %d...\n", s);
                        btstack_run_loop_remove_timer(&connection_timer);
                        gap_stop_scan();
                        memcpy(slots[s].adv_addr, rpa_addr, 6);
                        slots[s].adv_addr_type = rpa_addr_type;
                        slots[s].state = SLOT_CONNECTING;
                        connecting_slot = s;
                        coord_state = COORD_CONNECTING;
                        hog_connect(s);
                    }
                }
            }
            break;
        }
        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
        {
            bd_addr_t rpa_addr;
            sm_event_identity_resolving_failed_get_address(packet, rpa_addr);
            bd_addr_type_t rpa_addr_type = (bd_addr_type_t)(sm_event_identity_resolving_failed_get_addr_type(packet) & 1);
            bool adv_had_hid = get_and_clear_pending_rpa_has_hid_service(rpa_addr);

            if (adv_had_hid && coord_state == COORD_SCANNING) {
                int s = find_first_idle_slot();
                if (s >= 0) {
                    BLE_LOG("SM: Unresolved RPA %s has HID service, connecting as new device (slot %d)...\n",
                            bd_addr_to_str(rpa_addr), s);
                    btstack_run_loop_remove_timer(&connection_timer);
                    gap_stop_scan();
                    memcpy(slots[s].adv_addr, rpa_addr, 6);
                    slots[s].adv_addr_type = rpa_addr_type;
                    slots[s].state = SLOT_CONNECTING;
                    connecting_slot = s;
                    coord_state = COORD_CONNECTING;
                    hog_connect(s);
                }
            }
            break;
        }
        case SM_EVENT_IDENTITY_CREATED:
        {
            bd_addr_t identity_addr;
            sm_event_identity_created_get_identity_address(packet, identity_addr);
            bd_addr_type_t identity_addr_type = sm_event_identity_created_get_identity_addr_type(packet);
            BLE_LOG("SM: Identity created: %s (type %u)\n", bd_addr_to_str(identity_addr), identity_addr_type);
            add_bonded_entry(identity_addr, identity_addr_type);
            break;
        }
        case SM_EVENT_PAIRING_COMPLETE:
            switch (sm_event_pairing_complete_get_status(packet)) {
                case ERROR_CODE_SUCCESS:
                    BLE_LOG("Pairing complete, success\n");
                    request_hid_connection_parameters(slot);
                    connect_to_service = true;
                    break;
                case ERROR_CODE_CONNECTION_TIMEOUT:
                    BLE_LOG("Pairing failed, timeout\n");
                    handle_outgoing_connection_error(slot);
                    break;
                case ERROR_CODE_PIN_OR_KEY_MISSING:
                case ERROR_CODE_AUTHENTICATION_FAILURE:
                    BLE_LOG("Pairing failed (status 0x%02x). Clearing bond & restarting...\n",
                            sm_event_pairing_complete_get_status(packet));
                    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);
                    if (btstack_tlv_singleton_impl) {
                        btstack_tlv_singleton_impl->delete_tag(btstack_tlv_singleton_context, TLV_TAG_HOGD);
                    }
                    gap_delete_bonding(slots[slot].adv_addr_type, slots[slot].adv_addr);
                    clear_bonded_list();
                    handle_outgoing_connection_error(slot);
                    break;
                default:
                    BLE_LOG("Pairing failed, status 0x%02x\n", sm_event_pairing_complete_get_status(packet));
                    handle_outgoing_connection_error(slot);
                    break;
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            BLE_LOG("Re-encryption complete\n");
            request_hid_connection_parameters(slot);
            connect_to_service = true;
            break;
        default:
            break;
    }

    if (connect_to_service && slot >= 0) {
        BLE_LOG("Search for HID service (slot %d).\n", slot);
        uint8_t status = hids_host_connect(slots[slot].con_handle, handle_gatt_client_event,
                                            HID_PROTOCOL_MODE_REPORT, &slots[slot].hids_cid);
        if (status != ERROR_CODE_SUCCESS) {
            BLE_LOG("hids_host_connect failed (0x%02x)\n", status);
            coordinator_on_connect_fail(slot);
        }
    }
}

static void handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(packet_type);
    UNUSED(channel);
    UNUSED(size);

    if (hci_event_packet_get_type(packet) != HCI_EVENT_GATTSERVICE_META) {
        return;
    }

    uint8_t subevent = hci_event_gattservice_meta_get_subevent_code(packet);

    uint16_t cid;
    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            break;
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            cid = gattservice_subevent_hid_report_get_hids_cid(packet);
            break;
        default:
            return;
    }

    int slot = find_slot_by_cid(cid);
    if (slot < 0) return;

    uint8_t status;

    switch (subevent) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            switch (status) {
                case ERROR_CODE_SUCCESS:
                    BLE_LOG("Slot %d: HID service connected, %d services\n", slot,
                            gattservice_subevent_hid_service_connected_get_num_instances(packet));

                    if (find_bonded_entry(slots[slot].adv_addr) < 0) {
                        add_bonded_entry(slots[slot].adv_addr, slots[slot].adv_addr_type);
                    }
                    save_bonded_list();

                    slots[slot].state = SLOT_READY;
                    coordinator_on_connect_success(slot);
                    break;
                default:
                    BLE_LOG("Slot %d: HID service connection failed, status 0x%02x.\n", slot, status);
                    coordinator_on_connect_fail(slot);
                    break;
            }
            break;

        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
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
            break;

        case GATTSERVICE_SUBEVENT_HID_REPORT:
            hid_handle_input_report(
                cid,
                gattservice_subevent_hid_report_get_service_index(packet),
                gattservice_subevent_hid_report_get_report_id(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;

        default:
            break;
    }
}

//--------------------------------------------------------------------+
// COORDINATOR
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
    CMN_PublishReadySnapshot(&snap);
    g_usb_reinit_request = true;
}

static void coordinator_on_connect_success(int slot)
{
    (void)slot;
    coordinator_publish_snapshot();

    if (any_slot_idle()) {
        coordinator_start_scan();
    } else {
        coord_state = COORD_ALL_READY;
    }
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

    if (any_slot_idle()) {
        coordinator_start_scan();
    } else {
        coord_state = COORD_ALL_READY;
    }
}

static void coordinator_on_disconnect(int slot)
{
    (void)slot;
    coordinator_publish_snapshot();

    if (any_slot_idle()) {
        coordinator_start_scan();
    } else {
        coord_state = COORD_ALL_READY;
    }
}

static void coordinator_start_scan(void)
{
    hog_start_scan();
}

//--------------------------------------------------------------------+
// CONNECTION & SCANNING CONTROL
//--------------------------------------------------------------------+

static void hog_start_connect(void)
{
    load_bonded_list();
    coordinator_start_scan();
}

static void hog_connect(int slot)
{
    BLE_LOG("Connecting slot %d to %s (type %u, Timeout %dms)...\n",
            slot, bd_addr_to_str(slots[slot].adv_addr), slots[slot].adv_addr_type, CONNECTION_TIMEOUT_MS);

    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, CONNECTION_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_connection_timeout);
    btstack_run_loop_add_timer(&connection_timer);

    gap_connect(slots[slot].adv_addr, slots[slot].adv_addr_type);
}

static void hog_start_scan(void)
{
    BLE_LOG("Scanning for LE HID devices (Timeout %dms)...\n", SCAN_TIMEOUT_MS);
    coord_state = COORD_SCANNING;

    btstack_run_loop_remove_timer(&connection_timer);
    btstack_run_loop_set_timer(&connection_timer, SCAN_TIMEOUT_MS);
    btstack_run_loop_set_timer_handler(&connection_timer, &hog_scan_timeout);
    btstack_run_loop_add_timer(&connection_timer);

    gap_set_scan_parameters(0, 48, 48);
    gap_start_scan();
}

static void hog_scan_timeout(btstack_timer_source_t * ts)
{
    UNUSED(ts);
    if (coord_state != COORD_SCANNING) return;
    BLE_LOG("Scan timeout. Refreshing scan...\n");
    hog_start_scan();
}

static void hog_connection_timeout(btstack_timer_source_t * ts)
{
    UNUSED(ts);
    if (connecting_slot < 0) return;
    BLE_LOG("Connection timeout for slot %d. Cancelling.\n", connecting_slot);
    gap_connect_cancel();
    coordinator_on_connect_fail(connecting_slot);
}

static void hog_reconnect_timeout(btstack_timer_source_t * ts)
{
    UNUSED(ts);
    coordinator_start_scan();
}

static void handle_outgoing_connection_error(int slot)
{
    BLE_LOG("Error on slot %d, disconnecting\n", slot);
    if (slots[slot].con_handle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(slots[slot].con_handle);
    } else {
        coordinator_on_connect_fail(slot);
    }
}

static void request_hid_connection_parameters(int slot)
{
    if (slots[slot].con_handle == HCI_CON_HANDLE_INVALID) return;
    gap_request_connection_parameter_update(slots[slot].con_handle,
                                             CONN_INTERVAL_MIN_UNITS,
                                             CONN_INTERVAL_MAX_UNITS,
                                             CONN_LATENCY_EVENTS,
                                             CONN_TIMEOUT_UNITS);
}

//--------------------------------------------------------------------+
// BOND MANAGEMENT
//--------------------------------------------------------------------+

static void load_bonded_list(void)
{
    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);
    if (btstack_tlv_singleton_impl) {
        int len = btstack_tlv_singleton_impl->get_tag(btstack_tlv_singleton_context, TLV_TAG_HOGD,
                                                      (uint8_t *)&bonded_list, sizeof(bonded_list));
        if (len == sizeof(bonded_list) && bonded_list.count > 0 && bonded_list.count <= MAX_KNOWN_DEVICES) {
            has_bonded_device = true;
            BLE_LOG("Loaded %d bonded device(s)\n", bonded_list.count);
        } else {
            bonded_list.count = 0;
            has_bonded_device = false;
        }
    } else {
        bonded_list.count = 0;
        has_bonded_device = false;
    }
}

static void save_bonded_list(void)
{
    btstack_tlv_get_instance(&btstack_tlv_singleton_impl, &btstack_tlv_singleton_context);
    if (btstack_tlv_singleton_impl) {
        btstack_tlv_singleton_impl->store_tag(btstack_tlv_singleton_context, TLV_TAG_HOGD,
                                              (const uint8_t *)&bonded_list, sizeof(bonded_list));
    }
}

static int find_bonded_entry(const bd_addr_t addr)
{
    for (int i = 0; i < bonded_list.count; i++) {
        if (memcmp(bonded_list.entries[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static void add_bonded_entry(const bd_addr_t addr, bd_addr_type_t addr_type)
{
    // The device mints a new random address each time it is (re-)paired, so the
    // list fills with stale addresses from earlier pairings of the same device.
    // When it is full, evict the oldest entry (FIFO) so the device we just paired
    // always fits and is remembered for auto-reconnect.
    if (bonded_list.count >= MAX_KNOWN_DEVICES) {
        BLE_LOG("Bonded list full; evicting oldest %s\n",
                bd_addr_to_str(bonded_list.entries[0].addr));
        for (uint8_t i = 0; i + 1 < bonded_list.count; i++) {
            bonded_list.entries[i] = bonded_list.entries[i + 1];
        }
        bonded_list.count--;
    }
    memcpy(bonded_list.entries[bonded_list.count].addr, addr, 6);
    bonded_list.entries[bonded_list.count].addr_type = (bd_addr_type_t)(addr_type & 1);
    bonded_list.count++;
    has_bonded_device = true;
}

static void clear_bonded_list(void)
{
    bonded_list.count = 0;
    has_bonded_device = false;
}

//--------------------------------------------------------------------+
// ADDRESS HELPERS
//--------------------------------------------------------------------+

static bool is_resolvable_private_address(bd_addr_type_t addr_type, const bd_addr_t addr)
{
    return (addr_type == BD_ADDR_TYPE_LE_RANDOM) && ((addr[0] & 0xC0) == 0x40);
}

static bool adv_event_contains_hid_service(const uint8_t * packet)
{
    const uint8_t * ad_data = gap_event_advertising_report_get_data(packet);
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

static void hid_handle_input_report(uint16_t cid, uint8_t service_index, uint8_t report_id,
                                    const uint8_t * report, uint16_t report_len)
{
    UNUSED(service_index);

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

    static ST_HID_RPT stHidRpt;
    stHidRpt.report_id  = report_id;
    stHidRpt.report_len = report_len;
    if (stHidRpt.report_len > CMN_HID_RPT_DATA_SIZE) {
        stHidRpt.report_len = CMN_HID_RPT_DATA_SIZE;
    }
    memcpy(stHidRpt.report, report, stHidRpt.report_len);
    if (!CMN_Enqueue((ULONG)slot, &stHidRpt)) {
        // Queue full, drop
    }
}

//--------------------------------------------------------------------+
// CROSS-CORE API
//--------------------------------------------------------------------+

const uint8_t *hid_bridge_get_report_descriptor(uint8_t position, uint16_t *len)
{
    usb_ready_snapshot_t snap;
    if (!CMN_GetReadySnapshot(&snap)) return NULL;
    if (position >= snap.count) return NULL;

    uint16_t cid = snap.hids_cid[position];
    if (len) *len = hids_host_descriptor_storage_get_descriptor_len(cid, 0);
    return hids_host_descriptor_storage_get_descriptor_data(cid, 0);
}

//--------------------------------------------------------------------+
// LED BLINKING TIMER HANDLER
//--------------------------------------------------------------------+

static void led_timer_handler(btstack_timer_source_t * ts)
{
    static bool led_state = false;

    usb_ready_snapshot_t snap;
    CMN_GetReadySnapshot(&snap);

    if (snap.count > 0) {
        if (!led_state) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            led_state = true;
        }
    } else {
        led_state = !led_state;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_state);
    }

    btstack_run_loop_set_timer(ts, LED_BLINKING_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}

#if HOG_HOST_DEBUG
// Periodic diagnostic: proves Core 1 is alive and the CDC/log-replay path works,
// and shows the coordinator + per-slot state so we can see whether a dropped
// device's slot is ever freed (SLOT_READY stuck = link loss not detected).
static void heartbeat_handler(btstack_timer_source_t * ts)
{
    BLE_LOG("HB coord=%d | s0:%d ch=0x%04x | s1:%d ch=0x%04x\n",
            (int)coord_state,
            (int)slots[0].state, slots[0].con_handle,
            (int)slots[1].state, slots[1].con_handle);

    btstack_run_loop_set_timer(ts, HEARTBEAT_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}
#endif
