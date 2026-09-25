#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "pico/cyw43_arch.h"
#include "btstack.h"

#include "ble_coordinator.h"
#include "hid_bridge.h"
#include "indicators.h"

// User button, wired GP28 <-> GND (active-low, internal pull-up). GPIO0 (BOOTSEL)
// cannot be used here: it is the QSPI flash CS, driven by the XIP controller, so
// gpio_get(0) would read the flash-CS line instead of the button.
#define BUTTON_PIN 28
#define BUTTON_POLL_INTERVAL_MS 30
#define BUTTON_DEBOUNCE_MS 40

// LED: pairing mode -> rapid LED_PAIRING_BLINK_MS blink; normal mode ->
// 0 devices = solid ON, n>=1 = off LED_GAP_MS then {on LED_ON_MS, off
// LED_OFF_MS} x n, repeated.
#define LED_TICK_MS 50
#define LED_PAIRING_BLINK_MS 200
#define LED_GAP_MS 2000
#define LED_ON_MS 500
#define LED_OFF_MS 500

static btstack_timer_source_t led_timer;
static btstack_timer_source_t button_timer;

// Button debounce state (polled on Core 1).
static bool button_raw_prev = false;
static bool button_stable = false;
static uint32_t button_raw_since_change_ms = 0;

// LED pattern phase anchor (normal-mode count pattern).
static uint32_t led_cycle_start_ms = 0;

static void led_timer_handler(btstack_timer_source_t *ts);
static void button_timer_handler(btstack_timer_source_t *ts);

void indicators_init(void)
{
    led_cycle_start_ms = btstack_run_loop_get_time_ms();

    btstack_run_loop_set_timer_handler(&led_timer, &led_timer_handler);
    btstack_run_loop_set_timer(&led_timer, LED_TICK_MS);
    btstack_run_loop_add_timer(&led_timer);

    // User button: input with pull-up (active-low). Must be set up before the
    // first button_timer poll.
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    btstack_run_loop_set_timer_handler(&button_timer, &button_timer_handler);
    btstack_run_loop_set_timer(&button_timer, BUTTON_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(&button_timer);
}

static void led_timer_handler(btstack_timer_source_t *ts)
{
    uint32_t now = btstack_run_loop_get_time_ms();

    // Detect pairing-window expiry so the scan policy can re-evaluate promptly.
    static bool pairing_was_active = false;
    bool pairing_active = ble_coordinator_pairing_active();
    if (pairing_was_active && !pairing_active) {
        ble_coordinator_pairing_exit();
    }
    pairing_was_active = pairing_active;

    usb_ready_snapshot_t snap;
    hid_bridge_get_ready_snapshot(&snap);
    uint8_t n = snap.count;

    bool on;
    if (pairing_active) {
        // Rapid blink while the pairing window is open.
        on = ((now / LED_PAIRING_BLINK_MS) % 2) == 0;
    } else if (n == 0) {
        // Solid ON = powered, idle, no devices connected.
        on = true;
    } else {
        // Count pattern: off LED_GAP_MS, then {on LED_ON_MS, off LED_OFF_MS} x n, repeat.
        uint32_t period = LED_GAP_MS + (uint32_t)n * (LED_ON_MS + LED_OFF_MS);
        uint32_t t = (now - led_cycle_start_ms) % period;
        if (t < LED_GAP_MS) {
            on = false;
        } else {
            on = ((t - LED_GAP_MS) % (LED_ON_MS + LED_OFF_MS)) < LED_ON_MS;
        }
    }

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    btstack_run_loop_set_timer(ts, LED_TICK_MS);
    btstack_run_loop_add_timer(ts);
}

// Polls BUTTON_PIN (active-low, internal pull-up) and debounces to reject
// contact bounce. A confirmed press edge opens the pairing window.
static void button_timer_handler(btstack_timer_source_t *ts)
{
    uint32_t now = btstack_run_loop_get_time_ms();
    bool raw = !gpio_get(BUTTON_PIN);   // active-low: true = pressed

    if (raw != button_raw_prev) {
        button_raw_prev = raw;
        button_raw_since_change_ms = now;   // restart the debounce window
    }

    if ((now - button_raw_since_change_ms) >= BUTTON_DEBOUNCE_MS
        && button_raw_prev != button_stable) {
        button_stable = button_raw_prev;
        if (button_stable) {
            ble_coordinator_pairing_enter();
        }
    }

    btstack_run_loop_set_timer(ts, BUTTON_POLL_INTERVAL_MS);
    btstack_run_loop_add_timer(ts);
}
