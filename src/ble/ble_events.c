#include <inttypes.h>

#include "btstack.h"
#include "btstack_event.h"

#include "ble_coordinator.h"
#include "ble_events.h"
#include "ble_bonds.h"
#include "log_console.h"

static btstack_packet_callback_registration_t hci_event_callback_registration;
static btstack_packet_callback_registration_t sm_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void sm_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

void ble_events_init(void)
{
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    sm_event_callback_registration.callback = &sm_packet_handler;
    sm_add_event_handler(&sm_event_callback_registration);
}

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
                    {
                        bd_addr_t local_addr;
                        gap_local_bd_addr(local_addr);
                        BLE_LOG("BTstack up and running on %s.\n", bd_addr_to_str(local_addr));
                    }
                    ble_coordinator_on_btstack_ready();
                    break;
                case GAP_EVENT_ADVERTISING_REPORT:
                    ble_coordinator_on_advertising_report(packet);
                    break;
                case HCI_EVENT_DISCONNECTION_COMPLETE:
                {
                    uint16_t con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
                    uint8_t reason = hci_event_disconnection_complete_get_reason(packet);
                    ble_coordinator_on_disconnection(con_handle, reason);
                    break;
                }
                case HCI_EVENT_META_GAP:
                    if (hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
                    {
                        uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
                        hci_con_handle_t con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
                        ble_coordinator_on_connection_complete(status, con_handle);
                    }
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
            ble_coordinator_on_rpa_resolved(rpa_addr_type, rpa_addr, resolved_identity_addr);
            break;
        }
        case SM_EVENT_IDENTITY_RESOLVING_FAILED:
        {
            bd_addr_t rpa_addr;
            sm_event_identity_resolving_failed_get_address(packet, rpa_addr);
            bd_addr_type_t rpa_addr_type = (bd_addr_type_t)(sm_event_identity_resolving_failed_get_addr_type(packet) & 1);
            ble_coordinator_on_rpa_resolve_failed(rpa_addr_type, rpa_addr);
            break;
        }
        case SM_EVENT_IDENTITY_CREATED:
        {
            bd_addr_t identity_addr;
            sm_event_identity_created_get_identity_address(packet, identity_addr);
            bd_addr_type_t identity_addr_type = sm_event_identity_created_get_identity_addr_type(packet);
            BLE_LOG("SM: Identity created: %s (type %u)\n", bd_addr_to_str(identity_addr), identity_addr_type);
            ble_bonds_add(identity_addr, identity_addr_type);
            break;
        }
        case SM_EVENT_PAIRING_COMPLETE:
            ble_coordinator_on_pairing_complete(sm_event_pairing_complete_get_status(packet));
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            ble_coordinator_on_reencryption_complete();
            break;
        default:
            break;
    }
}

void ble_events_on_hid_service(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
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
            ble_coordinator_on_hid_service_connected(
                cid,
                gattservice_subevent_hid_service_connected_get_status(packet),
                gattservice_subevent_hid_service_connected_get_num_instances(packet));
            break;
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            cid = gattservice_subevent_hid_service_disconnected_get_hids_cid(packet);
            ble_coordinator_on_hid_service_disconnected(cid);
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            cid = gattservice_subevent_hid_report_get_hids_cid(packet);
            ble_coordinator_on_hid_report(
                cid,
                gattservice_subevent_hid_report_get_report_id(packet),
                gattservice_subevent_hid_report_get_report(packet),
                gattservice_subevent_hid_report_get_report_len(packet));
            break;
        default:
            break;
    }
}
