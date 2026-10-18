#ifndef DEBUG_CONSOLE_H
#define DEBUG_CONSOLE_H

/*
 * DEBUG builds only (DEBUG_CONSOLE=1): optional secondary log path.
 *
 * PRIMARY: UART. Every build — release included — writes deferred log text
 * to the console UART from log_event.c's drain. DEBUG_CONSOLE does not
 * change that, and it adds no USB function of its own.
 *
 * SECONDARY (DEBUG_CONSOLE only): the same text is appended to a 64 KB
 * in-RAM ring that the host can pop through HID feature report 0xB4
 * (tools/hidlog.py). That reader is optional: the ring is the only added
 * sink, and the USB descriptor/endpoint layout is identical in every build
 * (audio intf 0/1/2: speaker OUT 0x01, mic IN 0x82; gamepad intf 3 with
 * interrupt IN 0x84).
 */

#include <stdint.h>
#include <stdbool.h>

void debug_console_init(void);

/* R01: kept for main.c; the ring is consumer-driven (0xB4 pops it), so this
 * task has no transport to pump. */
void debug_console_task(void *arg);

/* Pop up to maxlen bytes of buffered console log (HID feature report 0xB4). */
uint16_t debug_console_read(uint8_t *dst, uint16_t maxlen);

/* R01: append already-formatted deferred log text to the same 64 KB ring.
 * Non-blocking; called from log_task (log_event.c), never from an ISR. */
void debug_console_write(const char *buf, int len);

#endif /* DEBUG_CONSOLE_H */
