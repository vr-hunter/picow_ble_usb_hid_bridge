// BTstack event dispatch for the BLE host. Thin layer: it parses the raw
// HCI/SM/GATT packets and forwards the meaningful facts to the connection
// coordinator (and the bond store); all decisions live in the coordinator.
#ifndef BLE_EVENTS_H
#define BLE_EVENTS_H

#include "btstack.h"

// Register the HCI and SM event handlers with BTstack (call from Core 1 init).
void ble_events_init(void);

// BTstack HIDS client handler (passed to hids_host_connect); forwards HID
// service connect/disconnect/report events to the coordinator.
void ble_events_on_hid_service(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

#endif // BLE_EVENTS_H
