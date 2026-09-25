#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "bsp/board_api.h"

#include "ble_host.h"
#include "bridge.h"
#include "log_console.h"
#include "report_queue.h"
#include "usb_device.h"

int main(void)
{
    board_init();

    // Initialize the USB device stack (TinyUSB) on Core 0.
    usb_device_init();

    stdio_init_all();

    // Set up the shared cross-core bridge state before Core 1 starts using it.
    bridge_init();
    report_queue_init();
    log_console_init();

    SYS_LOG("BLE to USB HID bridge starting\n");

    // Initialize to lock out CPU Core 0 when btstack writes to flash memory on
    // CPU Core 1.
    flash_safe_execute_core_init();

    SYS_LOG("Launching the BLE host on Core 1\n");
    multicore_launch_core1(ble_host_main);

    usb_device_main();

    return 0;
}
