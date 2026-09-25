#include <string.h>

#include "pico/stdlib.h"
#include "pico/critical_section.h"

#include "report_queue.h"

static hid_report_t data[MAX_HID_DEVICES][REPORT_QUEUE_DEPTH];
static uint32_t head[MAX_HID_DEVICES] = {0}; // read position
static uint32_t tail[MAX_HID_DEVICES] = {0}; // write position
static critical_section_t lock = {0};

void report_queue_init(void)
{
    memset(head, 0, sizeof(head));
    memset(tail, 0, sizeof(tail));
    critical_section_init(&lock);
}

bool report_queue_push(uint8_t slot, const hid_report_t *report)
{
    if (slot >= MAX_HID_DEVICES || !report) {
        return false;
    }

    bool ok = false;
    critical_section_enter_blocking(&lock);
    if ((tail[slot] + 1) % REPORT_QUEUE_DEPTH != head[slot]) {
        data[slot][tail[slot]] = *report;
        tail[slot] = (tail[slot] + 1) % REPORT_QUEUE_DEPTH;
        ok = true;
    }
    critical_section_exit(&lock);

    return ok;
}

bool report_queue_peek(uint8_t slot, hid_report_t *report)
{
    if (slot >= MAX_HID_DEVICES || !report) {
        return false;
    }

    bool ok = false;
    critical_section_enter_blocking(&lock);
    if (head[slot] != tail[slot]) {
        *report = data[slot][head[slot]];
        ok = true;
    }
    critical_section_exit(&lock);

    return ok;
}

void report_queue_advance(uint8_t slot)
{
    if (slot >= MAX_HID_DEVICES) {
        return;
    }

    critical_section_enter_blocking(&lock);
    if (head[slot] != tail[slot]) {
        head[slot] = (head[slot] + 1) % REPORT_QUEUE_DEPTH;
    }
    critical_section_exit(&lock);
}

void report_queue_clear(uint8_t slot)
{
    if (slot >= MAX_HID_DEVICES) {
        return;
    }

    critical_section_enter_blocking(&lock);
    head[slot] = 0;
    tail[slot] = 0;
    critical_section_exit(&lock);
}

void report_queue_clear_all(void)
{
    for (uint8_t i = 0; i < MAX_HID_DEVICES; i++) {
        report_queue_clear(i);
    }
}
