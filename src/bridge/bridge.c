#include <string.h>

#include "pico/stdlib.h"
#include "pico/critical_section.h"

#include "bridge.h"
#include "hid_bridge.h"

// Set by Core 1 whenever the READY set changes; Core 0 clears it after
// re-enumerating.
volatile bool g_usb_reinit_request = false;

static usb_ready_snapshot_t ready_snapshot = {0};
static critical_section_t snapshot_lock = {0};

void bridge_init(void)
{
    critical_section_init(&snapshot_lock);
    memset(&ready_snapshot, 0, sizeof(ready_snapshot));
    g_usb_reinit_request = false;
}

// Core 1 publishes the current READY set. Serialized on the shared spinlock.
void hid_bridge_publish_ready_snapshot(const usb_ready_snapshot_t *snap)
{
    if (!snap) {
        return;
    }

    critical_section_enter_blocking(&snapshot_lock);
    memcpy(&ready_snapshot, snap, sizeof(ready_snapshot));
    critical_section_exit(&snapshot_lock);
}

// Atomic copy of the current READY set.
bool hid_bridge_get_ready_snapshot(usb_ready_snapshot_t *out)
{
    if (!out) {
        return false;
    }

    critical_section_enter_blocking(&snapshot_lock);
    memcpy(out, &ready_snapshot, sizeof(*out));
    critical_section_exit(&snapshot_lock);

    return true;
}
