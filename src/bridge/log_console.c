#include <stdarg.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/critical_section.h"
#include "tusb.h"

#include "log_console.h"

static char log_msg[LOG_CONSOLE_SLOTS][LOG_CONSOLE_MSG_LEN];
static volatile uint32_t log_head = 0; // next slot to write (producer = Core 1)
static volatile uint32_t log_tail = 0; // next slot to read  (consumer = Core 0)
static critical_section_t lock = {0};

// CDC out staging: Core 0 copies a popped message here and streams it into the
// CDC endpoint. Only Core 0 touches these, so no lock is needed.
static char  cdc_out[LOG_CONSOLE_STAGE_LEN];
static uint32_t cdc_out_len = 0;
static uint32_t cdc_out_pos = 0;

void log_console_init(void)
{
    critical_section_init(&lock);
    log_head = 0;
    log_tail = 0;
    cdc_out_len = 0;
    cdc_out_pos = 0;
}

// Core 1: format a message into a local buffer (no lock), then copy it into
// the ring under the spinlock.
void log_console_push(const char *fmt, ...)
{
    char local[LOG_CONSOLE_MSG_LEN];
    va_list args;

    va_start(args, fmt);
    int n = vsnprintf(local, sizeof(local), fmt, args);
    va_end(args);
    if (n < 0) {
        return;
    }

    critical_section_enter_blocking(&lock);
    if ((log_head - log_tail) >= LOG_CONSOLE_SLOTS) {
        log_tail++; // full: drop the oldest message
    }
    memcpy(&log_msg[log_head % LOG_CONSOLE_SLOTS], local, sizeof(local));
    log_head++;
    critical_section_exit(&lock);
}

// The copy is made under the spinlock but the caller streams the bytes outside
// of it, so a port drop mid-drain leaves the rest buffered for the next pass.
bool log_console_pop(char *out, uint32_t maxlen)
{
    if (!out || maxlen == 0) {
        return false;
    }

    critical_section_enter_blocking(&lock);
    if (log_head == log_tail) {
        critical_section_exit(&lock);
        return false;
    }
    size_t copy = sizeof(log_msg[0]);
    if (copy > maxlen) copy = maxlen;
    memcpy(out, &log_msg[log_tail % LOG_CONSOLE_SLOTS], copy);
    out[copy - 1] = '\0';
    log_tail++;
    critical_section_exit(&lock);

    return true;
}

void log_console_cdc_reset(void)
{
    cdc_out_len = 0;
    cdc_out_pos = 0;
}

void log_console_cdc_load(const char *msg)
{
    if (!msg) {
        return;
    }
    uint32_t n = 0;
    for (const char *p = msg; *p && n < LOG_CONSOLE_STAGE_LEN - 2; p++) {
        if (*p == '\n') {
            cdc_out[n++] = '\r';
        }
        cdc_out[n++] = *p;
    }
    cdc_out_len = n;
    cdc_out_pos = 0;
}

// tud_task() is driven by the main loop, so this returns promptly. A port drop
// simply pauses the transfer (cdc_out_pos is kept) instead of dropping bytes.
void log_console_cdc_flush(void)
{
    if (cdc_out_pos >= cdc_out_len) {
        return;
    }
    if (!tud_cdc_connected()) {
        return;
    }
    while (cdc_out_pos < cdc_out_len) {
        uint32_t avail = tud_cdc_write_available();
        if (avail == 0) {
            break;
        }
        uint32_t chunk = cdc_out_len - cdc_out_pos;
        if (chunk > avail) chunk = avail;
        int n = (int)tud_cdc_write(&cdc_out[cdc_out_pos], chunk);
        if (n <= 0) {
            break;
        }
        cdc_out_pos += (uint32_t)n;
    }
    if (cdc_out_pos > 0 && cdc_out_pos < cdc_out_len) {
        tud_cdc_write_flush();
    }
}

bool log_console_cdc_pending(void)
{
    return cdc_out_pos < cdc_out_len;
}
