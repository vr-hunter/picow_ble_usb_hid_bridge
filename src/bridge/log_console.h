// Shared serial console. Both cores write tagged messages; the BLE core
// (Core 1) pushes into a ring buffer instead of printf'ing, because the USB
// CDC port drops output while Core 0 re-enumerates. Core 0 stages each message
// byte-by-byte into the CDC endpoint and only advances once the bytes are
// accepted, so a port drop pauses (not truncates) the transfer and messages
// emitted during a gap are replayed once the port comes back.
#ifndef LOG_CONSOLE_H
#define LOG_CONSOLE_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

// [Logging]
// SYS_LOG goes straight to printf (used by Core 0 before the CDC console is up
// and for boot messages). BLE_LOG (Core 1) and USB_LOG (Core 0) are routed
// through the console. USB tracing is chatty and only useful when debugging
// enumeration, so it is compiled out unless CMake enables it.
#define SYS_LOG(...) printf("[SYS] " __VA_ARGS__)
#define BLE_LOG(...) log_console_push("[BLE] " __VA_ARGS__)

#ifdef ENABLE_USB_LOGGING
#define USB_LOG(...) printf("[USB] " __VA_ARGS__)
#else
#define USB_LOG(...) ((void)0)
#endif

// Number of messages retained in the ring across a port gap.
#define LOG_CONSOLE_SLOTS 32
// Maximum length of one buffered message (including the trailing NUL).
#define LOG_CONSOLE_MSG_LEN 128
// Number of bytes staged for streaming into the CDC endpoint at once.
#define LOG_CONSOLE_STAGE_LEN 512

void log_console_init(void);

// Core 1: format a message and buffer it. If the ring is full the oldest
// message is dropped so the most recent ones are kept.
void log_console_push(const char *fmt, ...);

// Core 0: copy the oldest buffered message out and advance the read pointer.
// Returns false when the ring is empty.
bool log_console_pop(char *out, uint32_t maxlen);

// Core 0: discard any partially-sent staged message (call right before
// tud_disconnect, since accepted bytes are lost on re-enumeration).
void log_console_cdc_reset(void);

// Core 0: stage a (null-terminated) message for CDC streaming. Each '\n' is
// expanded to "\r\n" so the serial terminal gets CRLF line endings.
void log_console_cdc_load(const char *msg);

// Core 0: stream as much of the staged message into the CDC endpoint as is
// currently available; the remainder is re-attempted on the next call.
void log_console_cdc_flush(void);

// Core 0: true while any staged bytes are still waiting to be sent.
bool log_console_cdc_pending(void);

#endif // LOG_CONSOLE_H
