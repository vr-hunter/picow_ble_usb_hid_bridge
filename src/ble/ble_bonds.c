#include <string.h>

#include "btstack.h"
#include "btstack_tlv.h"

#include "ble_bonds.h"
#include "log_console.h"

// TLV tag used for the bonded-device list in flash. The tag value is kept
// stable across refactors so previously flashed lists stay readable.
#define TLV_TAG_BONDED_DEVICES ((((uint32_t) 'H') << 24) | (((uint32_t) 'O') << 16) | (((uint32_t) 'G') << 8) | 'D')

typedef struct {
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} bonded_device_t;

typedef struct {
    uint8_t count;
    bonded_device_t entries[MAX_KNOWN_DEVICES];
} bonded_list_t;

static bonded_list_t bonded_list;
static bool has_bonded_device = false;

static const btstack_tlv_t *btstack_tlv_instance;
static void *btstack_tlv_context;

void ble_bonds_load(void)
{
    btstack_tlv_get_instance(&btstack_tlv_instance, &btstack_tlv_context);
    if (btstack_tlv_instance) {
        int len = btstack_tlv_instance->get_tag(btstack_tlv_context, TLV_TAG_BONDED_DEVICES,
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

void ble_bonds_save(void)
{
    btstack_tlv_get_instance(&btstack_tlv_instance, &btstack_tlv_context);
    if (btstack_tlv_instance) {
        btstack_tlv_instance->store_tag(btstack_tlv_context, TLV_TAG_BONDED_DEVICES,
                                        (const uint8_t *)&bonded_list, sizeof(bonded_list));
    }
}

int ble_bonds_find(const bd_addr_t addr)
{
    for (int i = 0; i < bonded_list.count; i++) {
        if (memcmp(bonded_list.entries[i].addr, addr, 6) == 0) {
            return i;
        }
    }
    return -1;
}

void ble_bonds_add(const bd_addr_t addr, bd_addr_type_t addr_type)
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

void ble_bonds_clear(void)
{
    bonded_list.count = 0;
    has_bonded_device = false;
}

void ble_bonds_forget(void)
{
    btstack_tlv_get_instance(&btstack_tlv_instance, &btstack_tlv_context);
    if (btstack_tlv_instance) {
        btstack_tlv_instance->delete_tag(btstack_tlv_context, TLV_TAG_BONDED_DEVICES);
    }
    ble_bonds_clear();
}

bool ble_bonds_has_device(void)
{
    return has_bonded_device;
}

uint8_t ble_bonds_count(void)
{
    return bonded_list.count;
}
