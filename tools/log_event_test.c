/*
 * Host-side test for src/log_event.c (R01 event ring).
 *
 * Build: gcc -O2 -Wall -Wextra -DLOG_EVENT_HOST_TEST \
 *            -o /tmp/opencode/log_event_test tools/log_event_test.c src/log_event.c
 *
 * Covers: empty pop, FIFO order, wrap-around, overflow (drop-oldest) and the
 * dropped counter. The ring is the only inter-context piece of R01 that can
 * be verified without hardware.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "../src/log_event.h"

static int failures = 0;

static void expect(const char *what, int cond)
{
    if (!cond)
        failures++;
    printf("%-46s %s\n", what, cond ? "PASS" : "FAIL");
}

/* ------------------------------------------------------------------ *
 * Text-ring boundary helpers.
 *
 * A record is [u16 length][payload]; either part may wrap. The ring is
 * private to log_event.c, so the only way to reach a given byte offset is
 * to emit known-length records and drain them. Drain output goes to
 * stdout in the host build, so it is captured (or discarded) via the fd.
 * ------------------------------------------------------------------ */
static int   cap_saved = -1;
static FILE *cap_file  = NULL;

static void capture_begin(void)
{
    fflush(stdout);
    cap_saved = dup(fileno(stdout));
    cap_file = tmpfile();
    if (cap_saved < 0 || cap_file == NULL) {
        failures++;
        return;
    }
    dup2(fileno(cap_file), fileno(stdout));
}

static uint32_t capture_end(char *dst, uint32_t cap)
{
    uint32_t n = 0;

    fflush(stdout);
    if (cap_saved >= 0) {
        dup2(cap_saved, fileno(stdout));
        close(cap_saved);
        cap_saved = -1;
    }
    if (cap_file != NULL) {
        rewind(cap_file);
        n = (uint32_t)fread(dst, 1, (size_t)(cap - 1u), cap_file);
        fclose(cap_file);
        cap_file = NULL;
    }
    dst[n] = '\0';
    return n;
}

static void drain_all(void)
{
    while (log_text_drain() != 0)
        ;
}

/* Move head and tail to exactly `target` bytes into the ring. The largest
 * record is 2 + 383 bytes, so a handful of drains covers any offset. */
static int advance_to(uint32_t target)
{
    static char fill[LOG_TEXT_MSG_MAX];
    uint32_t done = 0;

    log_text_reset();
    while (done < target) {
        uint32_t remaining = target - done;
        uint32_t chunk = remaining > 385u ? 385u : remaining;
        uint32_t len;

        /* Every record advances by at least 3 bytes; avoid leaving a 1- or
         * 2-byte remainder that no record can represent. */
        if (remaining - chunk == 1u || remaining - chunk == 2u)
            chunk -= 3u;
        len = chunk - 2u;
        memset(fill, 'A', (size_t)len);
        fill[len] = '\0';
        log_text_printf("%s", fill);
        capture_begin();
        drain_all();
        (void)capture_end(fill, LOG_TEXT_MSG_MAX);   /* discard setup output */
        if (log_text_pending() != 0)
            return 0;
        done += chunk;
    }
    return done == target;
}

/* Emit one record and compare exactly what the drain produced. */
static int emit_expect(const char *text)
{
    char out[2048];
    uint32_t n, want = (uint32_t)strlen(text);

    log_text_printf("%s", text);
    capture_begin();
    drain_all();
    n = capture_end(out, sizeof out);
    return n == want && memcmp(out, text, want) == 0;
}

int main(void)
{
    log_evt_t e;
    uint32_t cap = log_event_capacity();

    /* 1. empty ring */
    log_event_reset();
    expect("empty: pending == 0", log_event_pending() == 0);
    expect("empty: pop returns 0", log_event_pop(&e) == 0);

    /* 2. FIFO order and payload */
    log_event_reset();
    log_event_put(LOG_EVT_BT_FSM, 1, 2);
    log_event_put(LOG_EVT_BT_FSM, 2, 3);
    log_event_put(LOG_EVT_HID_FEATURE, 0x05, 12);
    expect("fifo: pending == 3", log_event_pending() == 3);

    expect("fifo: first pop ok", log_event_pop(&e) == 1);
    expect("fifo: first id", e.id == LOG_EVT_BT_FSM);
    expect("fifo: first a/b", e.a == 1 && e.b == 2);
    expect("fifo: second pop ok", log_event_pop(&e) == 1);
    expect("fifo: second a/b", e.a == 2 && e.b == 3);
    expect("fifo: third pop ok", log_event_pop(&e) == 1);
    expect("fifo: third id/len", e.id == LOG_EVT_HID_FEATURE && e.b == 12);
    expect("fifo: now empty", log_event_pop(&e) == 0);

    /* 3. wrap-around: many put/pop cycles must keep order */
    log_event_reset();
    int order_ok = 1;
    for (uint32_t i = 0; i < 200; i++) {
        log_event_put(LOG_EVT_USB_STATE, (uint16_t)(i & 0xFFFF), i);
        if (log_event_pop(&e) != 1 || e.a != (uint16_t)(i & 0xFFFF) || e.b != i)
            order_ok = 0;
    }
    expect("wrap: 200 cycles preserve order", order_ok);
    expect("wrap: nothing pending", log_event_pending() == 0);
    expect("wrap: no drops", log_event_dropped() == 0);

    /* 4. overflow: capacity+10 puts with no pop drops the 10 oldest */
    log_event_reset();
    uint32_t total = cap + 10u;
    for (uint32_t i = 0; i < total; i++)
        log_event_put(LOG_EVT_USB_STATE, 0, i);
    expect("overflow: pending == capacity", log_event_pending() == cap);
    expect("overflow: dropped == 10", log_event_dropped() == 10u);
    expect("overflow: oldest pop is #10", log_event_pop(&e) == 1 && e.b == 10u);

    uint32_t last = 0;
    while (log_event_pop(&e))
        last = e.b;
    expect("overflow: newest kept", last == total - 1u);

    /* 5. reset clears counters and contents */
    log_event_reset();
    expect("reset: pending == 0", log_event_pending() == 0);
    expect("reset: dropped == 0", log_event_dropped() == 0);

    /* 6. text ring: record starts on the last byte, so the 2-byte header
     * wraps and the payload begins at offset 1. */
    expect("text: advance to 4095", advance_to(4095u) == 1);
    expect("text: hdr-wrapped record", emit_expect("HELLO"));
    expect("text: hdr-wrapped drained",
           log_text_pending() == 0 && log_text_dropped() == 0);

    /* 7. text ring: record starts one byte before the last byte; the
     * header still fits but the payload starts at offset 0. */
    expect("text: advance to 4094", advance_to(4094u) == 1);
    expect("text: one-before-last record", emit_expect("WORLD"));
    expect("text: one-before-last drained",
           log_text_pending() == 0 && log_text_dropped() == 0);

    /* 8. text ring: record fills exactly to the wrap boundary. */
    expect("text: advance to 4084", advance_to(4084u) == 1);
    expect("text: fill-to-wrap record", emit_expect("0123456789"));
    expect("text: fill-to-wrap drained",
           log_text_pending() == 0 && log_text_dropped() == 0);
    /* The next record therefore starts at offset 0. */
    expect("text: post-wrap record", emit_expect("NEXT"));

    /* 9. text ring: a sequence that crosses the boundary repeatedly. */
    expect("text: advance to 4000", advance_to(4000u) == 1);
    {
        int wrap_ok = 1;

        for (uint32_t i = 0; i < 80u; i++) {
            char text[160];
            uint32_t len = 1u + (i * 37u) % 150u;

            memset(text, (int)('a' + (i % 26u)), (size_t)len);
            text[len] = '\0';
            if (!emit_expect(text))
                wrap_ok = 0;
        }
        expect("text: 80 records wrap repeatedly", wrap_ok);
    }
    expect("text: repeated wraps drained",
           log_text_pending() == 0 && log_text_dropped() == 0);

    printf("---- %s (%d failures) ----\n", failures ? "FAILED" : "ALL PASS",
           failures);
    return failures ? 1 : 0;
}
