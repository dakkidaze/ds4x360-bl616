#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>
#include "log_event.h"

/*
 * Build-time log level control.
 *
 * Define LOG_LEVEL before including this header (or via -DLOG_LEVEL=N):
 *   0 = silent   (errors only)
 *   1 = warnings + errors
 *   2 = info + warnings + errors          (default for release)
 *   3 = debug + info + warnings + errors  (development)
 *
 * OUTPUT MODEL (R01)
 * ------------------
 * LOG_DBG / LOG_INF / LOG_WRN are NON-BLOCKING. The call formats with
 * vsnprintf into a small stack buffer and appends the text to the bounded
 * 4 KB ring in log_event.c. Nothing is written to the UART or USB from the
 * caller. log_task() (lowest application priority, src/main.c) drains the
 * ring and performs the actual output:
 *   - deferred text is always written to the console UART, non-blocking,
 *     with a per-drain byte budget;
 *   - DEBUG_CONSOLE builds additionally append it to the 64 KB console ring
 *     (debug_console_write) behind the optional HID feature 0xB4 reader.
 * The text is preserved exactly, including the CR before LF that the old
 * printf() output inserted. If the ring is full the newest record is dropped
 * and log_text_dropped() is incremented; the counter is reported in [DIAG].
 *
 * LOG_ERR stays SYNCHRONOUS printf() on purpose: a panic/error must reach the
 * wire before a reset or hang, and must not be lost to a full ring. It may
 * block on the UART, so it must not appear on per-report or per-audio-frame
 * paths. configASSERT and the fatal paths in FreeRTOSConfig.h also stay
 * synchronous (they are outside this header and deliberately unchanged).
 * Consequence: a LOG_ERR can appear before LOG_INF lines that were queued
 * earlier but not yet drained.
 *
 * The enqueue path (log_text_printf) takes only a short IRQ-masked section
 * around the ring copy and never schedules or allocates, so an accidental
 * call from an ISR is safe. LOG_ISR therefore maps to the same non-blocking
 * enqueue instead of printf.
 */

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#if LOG_LEVEL >= 3
#define LOG_DBG(fmt, ...) log_text_printf(fmt, ##__VA_ARGS__)
#else
#define LOG_DBG(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 2
#define LOG_INF(fmt, ...) log_text_printf(fmt, ##__VA_ARGS__)
#else
#define LOG_INF(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= 1
#define LOG_WRN(fmt, ...) log_text_printf(fmt, ##__VA_ARGS__)
#else
#define LOG_WRN(fmt, ...) ((void)0)
#endif

/* Synchronous by design: see the output model above. */
#define LOG_ERR(fmt, ...) printf(fmt, ##__VA_ARGS__)

/* ISR context: the enqueue path is safe there; kept as a distinct name so
 * ISR call sites remain visible. Gate unchanged (level 3). */
#if LOG_LEVEL >= 3
#define LOG_ISR(fmt, ...) log_text_printf(fmt, ##__VA_ARGS__)
#else
#define LOG_ISR(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_LOG_H */
