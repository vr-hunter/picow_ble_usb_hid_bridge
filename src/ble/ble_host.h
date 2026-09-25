// Core 1 entry point for the BLE host: initializes the CYW43 driver and
// BTstack, wires up the event handlers, timers and indicators, then runs the
// BTstack run loop until power off.
#ifndef BLE_HOST_H
#define BLE_HOST_H

// Entry point for CPU Core 1 (launched by Core 0 via multicore_launch_core1).
void ble_host_main(void);

#endif // BLE_HOST_H
