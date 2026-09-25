// Connection coordinator for the BLE host. Owns the device slots and the
// scan/connect lifecycle: it decides when to scan, which advertised device to
// connect to (bonded devices always, new devices only while the pairing
// window is open), drives pairing and GATT service setup, and publishes the
// READY snapshot consumed by the USB side whenever it changes.
#ifndef BLE_COORDINATOR_H
#define BLE_COORDINATOR_H

#include <stdbool.h>
#include "btstack.h"
#include "hid_bridge.h"

// One-time setup of the slots and scan policy (call from Core 1).
void ble_coordinator_init(void);

// --- Event entry points (called by the BLE event handlers) -------------------

// BTstack is up and working: start the scan policy.
void ble_coordinator_on_btstack_ready(void);

// A new advertisement was received while scanning.
void ble_coordinator_on_advertising_report(const uint8_t *packet);

// The link for the given connection handle went down.
void ble_coordinator_on_disconnection(hci_con_handle_t con_handle, uint8_t reason);

// An outgoing LE connection attempt finished (GAP subevent).
void ble_coordinator_on_connection_complete(uint8_t status, hci_con_handle_t con_handle);

// Pairing of the currently connecting device finished with the given status.
void ble_coordinator_on_pairing_complete(uint8_t status);

// Re-encryption of the currently connecting device finished.
void ble_coordinator_on_reencryption_complete(void);

// A resolvable private address resolved to an identity address.
void ble_coordinator_on_rpa_resolved(bd_addr_type_t rpa_addr_type, const bd_addr_t rpa_addr,
                                     const bd_addr_t identity_addr);

// A resolvable private address could not be resolved.
void ble_coordinator_on_rpa_resolve_failed(bd_addr_type_t rpa_addr_type, const bd_addr_t rpa_addr);

// The HIDS client for the given cid connected / disconnected / sent a report.
void ble_coordinator_on_hid_service_connected(uint16_t cid, uint8_t status, uint16_t num_instances);
void ble_coordinator_on_hid_service_disconnected(uint16_t cid);
void ble_coordinator_on_hid_report(uint16_t cid, uint8_t report_id, const uint8_t *report, uint16_t report_len);

// --- Pairing window ----------------------------------------------------------
// A window during which the bridge discovers NEW (unbonded) HID devices.
// Bonded devices always auto-reconnect, independent of this window.
void ble_coordinator_pairing_enter(void);
void ble_coordinator_pairing_exit(void);
bool ble_coordinator_pairing_active(void);

#endif // BLE_COORDINATOR_H
