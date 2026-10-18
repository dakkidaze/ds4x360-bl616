/*
 * DEBUG builds only (DEBUG_CONSOLE=1): 64 KB in-RAM console ring.
 *
 * UART is the primary log path: log_event.c's drain writes every deferred
 * record to the console UART unconditionally, in every build. This module is
 * the optional secondary path — the same text is also appended here and the
 * host may pop it through HID feature report 0xB4 (tools/hidlog.py).
 *
 * There is no USB transport of its own: no CDC function, no endpoints, no
 * class callbacks. Readers pull; nothing pushes.
 */

#include "debug_console.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_irq.h"
#include <string.h>
#include <stdio.h>
#include <sys/reent.h>
#include <sys/types.h>

#define LOG_RING_SIZE    65536u   /* power of two */

static uint8_t  log_ring[LOG_RING_SIZE];
static volatile uint32_t log_head = 0;   /* write index (producers) */
static volatile uint32_t log_tail = 0;   /* read index (0xB4 reader) */

/* SDK console tap (see g_console_tap in components/libc/vsnprintf.c) */
extern void (*g_console_tap)(char character);
static void console_tap(char character);

void debug_console_init(void)
{
    log_head = 0;
    log_tail = 0;
    g_console_tap = console_tap;
}

/* Pop up to maxlen bytes from the log ring (HID feature-report reader). */
uint16_t debug_console_read(uint8_t *dst, uint16_t maxlen)
{
    uint32_t head, tail, avail, contig, n;
    uintptr_t key = bflb_irq_save();
    head = log_head;
    tail = log_tail;
    avail = (head >= tail) ? (head - tail) : (LOG_RING_SIZE - tail + head);
    if (avail == 0 || maxlen == 0) {
        bflb_irq_restore(key);
        return 0;
    }
    contig = LOG_RING_SIZE - tail;
    n = avail;
    if (n > maxlen) n = maxlen;
    if (n > contig) n = contig;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = log_ring[(tail + i) & (LOG_RING_SIZE - 1)];
    log_tail = (tail + n) & (LOG_RING_SIZE - 1);
    bflb_irq_restore(key);
    return (uint16_t)n;
}

static void ring_put(const char *buf, int len)
{
    uintptr_t key = bflb_irq_save();
    for (int i = 0; i < len; i++) {
        uint32_t next = (log_head + 1) & (LOG_RING_SIZE - 1);
        if (next == log_tail) {
            /* full: drop oldest byte to keep the newest log lines */
            log_tail = (log_tail + 1) & (LOG_RING_SIZE - 1);
        }
        log_ring[log_head] = (uint8_t)buf[i];
        log_head = next;
    }
    bflb_irq_restore(key);
}

/* R01: entry point for the deferred text ring in log_event.c. Same ring and
 * same overflow policy as printf's tap, so ordering with direct printf() is
 * preserved on a best-effort basis. Called from log_task only. */
void debug_console_write(const char *buf, int len)
{
    if (buf && len > 0)
        ring_put(buf, len);
}

int _write(int fd, char *ptr, int len)
{
    (void)fd;
    if (ptr && len > 0)
        ring_put(ptr, len);
    return len;
}

/* newlib's printf goes through the reentrant _write_r (a weak alias to
 * _console_write_r in syscalls_simple_io.c), NOT _write. Override both. */
ssize_t _write_r(struct _reent *r, int fd, const void *ptr, size_t len)
{
    (void)r; (void)fd;
    if (ptr && len > 0)
        ring_put((const char *)ptr, (int)len);
    return (ssize_t)len;
}

/* The SDK's printf (components/libc/vsnprintf.c) writes to the UART console
 * directly, bypassing newlib stdio. It calls g_console_tap per character. */
static void console_tap(char character)
{
    ring_put(&character, 1);
}

/* R01: still created by main.c in DEBUG_CONSOLE builds. The ring is
 * consumer-driven (each GET_REPORT(0xB4) pops it), so there is no endpoint
 * to pace and no buffer to flush here; the task only exists so the build
 * topology does not need a second conditional in main.c. */
void debug_console_task(void *arg)
{
    (void)arg;
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}
