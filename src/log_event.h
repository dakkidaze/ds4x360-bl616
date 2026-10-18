#ifndef LOG_EVENT_H
#define LOG_EVENT_H

#include <stdint.h>

/*
 * R01: fixed-size event ring for contexts that must not format or print
 * (USB ISR / completion callbacks, Bluetooth receive callbacks).
 *
 * Producers only write one 8-byte slot inside a very short IRQ-masked
 * critical section: no vsnprintf, no UART, no allocation, no waiting.
 * A task drains the ring later and formats it with printf.
 *
 * Overflow policy: drop the OLDEST event so the newest are kept, and count
 * the drops. A non-zero drop count means "some events are missing", never
 * "nothing happened".
 */

enum log_event_id {
    LOG_EVT_NONE = 0,
    LOG_EVT_USB_STATE,     /* a = event code, b = 0 */
    LOG_EVT_BT_FSM,        /* a = old state, b = new state */
    LOG_EVT_BT_LINK,       /* a = 1 connected / 0 disconnected, b = acl handle */
    LOG_EVT_HID_FEATURE,   /* a = report id, b = payload length */
    LOG_EVT_USB_EVT_CFG,   /* a = configured count snapshot, b = 0 */
    LOG_EVT_AUDIO_STREAM,  /* a: 0 spk open, 1 spk close, 2 mic open, 3 mic close,
                            *    4 spk endpoint fault (F0 net), 5 mic endpoint fault */
    LOG_EVT_BT_MISC,       /* a = source line in bt_hid_host.c, b = 0 */
    LOG_EVT_MAX
};

typedef struct {
    uint16_t id;
    uint16_t a;
    uint32_t b;
} log_evt_t;

/* ISR- and callback-safe. Never blocks, never formats. */
void log_event_put(uint16_t id, uint16_t a, uint32_t b);

/* Task context: pop one event (FIFO). Returns 1 if an event was returned. */
int log_event_pop(log_evt_t *out);

/* Number of events discarded because the ring was full. */
uint32_t log_event_dropped(void);

/* Task context: format and print everything currently queued, then report the
 * dropped count if it changed. Safe to call periodically. */
void log_event_drain_print(void);

/* Test/diagnostic hooks (no effect on the ring itself). */
uint32_t log_event_pending(void);
uint32_t log_event_capacity(void);
void log_event_reset(void);

/*
 * R01 residual: bounded TEXT ring for task-context logs.
 *
 * LOG_INF/LOG_WRN/LOG_DBG are formatted at the call site with vsnprintf into
 * a small stack buffer, then the finished text is appended to a fixed 4 KB
 * byte ring. The append happens inside a short IRQ-masked section: no UART,
 * no malloc, no scheduling, no printf. It is therefore safe if a caller
 * happens to be in an ISR, although the intended producers are tasks.
 *
 * Ownership: every context may produce (tasks, and an ISR by accident); only
 * log_task() (lowest application priority) consumes, by calling
 * log_text_drain(). See src/main.c.
 *
 * Overflow policy: the ring never splits a record. If the new record does not
 * fit, the NEW record is dropped and log_text_dropped() is incremented
 * (older records are kept). Drop-new keeps the masked section O(1) — dropping
 * the oldest would mean walking and discarding an unbounded number of old
 * records while interrupts are disabled. A non-zero drop count means the log
 * is incomplete, never that nothing happened.
 *
 * A record is stored as [uint16 length][length bytes], length excluding NUL.
 */
/* Longest single record, including the inserted CR/LF. 384 is sized so the
 * [DIAG] line (23 numeric fields) still fits with 10-digit values; longer
 * output is truncated with one record, never split. */
#define LOG_TEXT_MSG_MAX 384u

/* Format into the text ring. Non-blocking, allocation-free. */
void log_text_printf(const char *fmt, ...);

/* Task context: move at most a bounded amount of queued text to the output
 * stage. The console UART is written in every build; DEBUG_CONSOLE builds
 * additionally mirror the same records into the 64 KB console ring.
 * Returns the number of records consumed. Not reentrant: log_task only. */
uint32_t log_text_drain(void);

/* Records discarded because the ring was full. */
uint32_t log_text_dropped(void);

/* Bytes currently queued (0..capacity-1). */
uint32_t log_text_pending(void);

/* Drop everything queued (test/diagnostic only). */
void log_text_reset(void);

#endif /* LOG_EVENT_H */
