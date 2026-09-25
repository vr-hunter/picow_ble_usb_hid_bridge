#ifndef BLE_BONDS_H
#define BLE_BONDS_H

#include <stdbool.h>
#include "btstack.h"
#include "hid_bridge.h"

// Load the persisted list of bonded addresses (call once at startup).
void ble_bonds_load(void);

// Persist the current list to flash (call after adding a new bond).
void ble_bonds_save(void);

// Find a bonded address; returns the entry index, or -1 if not bonded.
int ble_bonds_find(const bd_addr_t addr);

// Remember a bonded address. When the list is full, the oldest entry is
// evicted (FIFO) so the device just paired always fits.
void ble_bonds_add(const bd_addr_t addr, bd_addr_type_t addr_type);

// Forget every bonded device and clear the persisted list.
void ble_bonds_clear(void);

// Forget every bonded device, including the persisted list in flash.
void ble_bonds_forget(void);

// True if at least one bonded device is remembered.
bool ble_bonds_has_device(void);

// Number of remembered bonded devices.
uint8_t ble_bonds_count(void);

#endif // BLE_BONDS_H
