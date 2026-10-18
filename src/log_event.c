#include "log_event.h"
#include "debug_log.h"
#include <stdarg.h>
#include <string.h>

#ifdef LOG_EVENT_HOST_TEST
#define LOG_EVT_ENTER()   0u
#define LOG_EVT_EXIT(k)   ((void)(k))
#else
#include "bflb_irq.h"
#include "bflb_uart.h"
#define LOG_EVT_ENTER()   bflb_irq_save()
#define LOG_EVT_EXIT(k)   bflb_irq_restore(k)
#endif

#ifdef DEBUG_CONSOLE
#include "debug_console.h"
#endif

#define LOG_EVT_RING 64u   /* power of two; one slot stays empty as sentinel */

static log_evt_t        ring[LOG_EVT_RING];
static volatile uint32_t head = 0;      /* next slot to write */
static volatile uint32_t tail = 0;      /* next slot to read */
static volatile uint32_t dropped = 0;

static inline uint32_t ring_next(uint32_t i)
{
    return (i + 1u) & (LOG_EVT_RING - 1u);
}

void log_event_put(uint16_t id, uint16_t a, uint32_t b)
{
    uintptr_t key = LOG_EVT_ENTER();
    uint32_t next = ring_next(head);

    if (next == tail) {
        /* Full: discard the oldest so the newest survive. */
        tail = ring_next(tail);
        dropped++;
    }

    ring[head].id = id;
    ring[head].a  = a;
    ring[head].b  = b;
    head = next;

    LOG_EVT_EXIT(key);
}

int log_event_pop(log_evt_t *out)
{
    uintptr_t key;
    int got = 0;

    key = LOG_EVT_ENTER();
    if (head != tail) {
        *out = ring[tail];
        tail = ring_next(tail);
        got = 1;
    }
    LOG_EVT_EXIT(key);

    return got;
}

uint32_t log_event_dropped(void)
{
    return dropped;
}

uint32_t log_event_pending(void)
{
    uintptr_t key = LOG_EVT_ENTER();
    uint32_t h = head, t = tail;
    LOG_EVT_EXIT(key);
    return (h >= t) ? (h - t) : (LOG_EVT_RING - t + h);
}

uint32_t log_event_capacity(void)
{
    return LOG_EVT_RING - 1u;
}

void log_event_reset(void)
{
    uintptr_t key = LOG_EVT_ENTER();
    head = 0;
    tail = 0;
    dropped = 0;
    LOG_EVT_EXIT(key);
}

/* R01: sink for the SDK USB stack (usb_config.h). Deliberately does not
 * format: these calls happen in EP0 handlers and in the USB interrupt. */
volatile uint32_t g_usb_log_cnt = 0;

void usb_log_sink(const char *fmt, ...)
{
    (void)fmt;
    g_usb_log_cnt++;
}

static const char *usb_state_name(uint16_t code)
{
    switch (code) {
    case 0: return "CONFIGURED";
    case 1: return "RESET";
    case 2: return "SUSPEND";
    case 3: return "RESUME";
    case 4: return "CONNECTED";
    case 5: return "DISCONNECTED";
    default: return "OTHER";
    }
}

void log_event_drain_print(void)
{
    log_evt_t e;
    uint32_t n = 0;
    static uint32_t reported_drops = 0;

    /* Bounded work per call: this runs in task context and must not starve
     * the caller's loop during an event storm. */
    while (n < 24u && log_event_pop(&e)) {
        n++;
        switch (e.id) {
        case LOG_EVT_USB_STATE:
            LOG_INF("[EVT] usb %s\n", usb_state_name(e.a));
            break;
        case LOG_EVT_BT_FSM:
            LOG_INF("[EVT] bt fsm %u -> %u\n",
                    (unsigned)e.a, (unsigned)e.b);
            break;
        case LOG_EVT_BT_LINK:
            LOG_INF("[EVT] bt link %s handle=0x%04lx\n",
                    e.a ? "up" : "down", (unsigned long)e.b);
            break;
        case LOG_EVT_HID_FEATURE:
            LOG_INF("[EVT] hid feature id=0x%02x len=%lu\n",
                    (unsigned)e.a, (unsigned long)e.b);
            break;
        case LOG_EVT_USB_EVT_CFG:
            LOG_INF("[EVT] usb configured #%lu\n", (unsigned long)e.b);
            break;
        case LOG_EVT_AUDIO_STREAM: {
            static const char *names[6] = { "spk open", "spk close",
                                            "mic open", "mic close",
                                            "spk ep fault", "mic ep fault" };
            LOG_INF("[EVT] audio %s\n",
                    e.a < 6 ? names[e.a] : "?");
            break;
        }
        case LOG_EVT_BT_MISC:
            LOG_INF("[EVT] bt@bt_hid_host.c:%u lvl=%lu\n",
                    (unsigned)e.a, (unsigned long)e.b);
            break;
        default:
            LOG_DBG("[EVT] id=%u a=%u b=%lu\n",
                    (unsigned)e.id, (unsigned)e.a, (unsigned long)e.b);
            break;
        }
    }

    if (n > 0u) {
        uint32_t d = dropped;
        if (d != reported_drops) {
            reported_drops = d;
            LOG_WRN("[EVT] ring full: %lu events dropped so far\n",
                    (unsigned long)d);
        }
    }
}

/* ------------------------------------------------------------------ *
 * R01 residual: bounded text ring for task-context LOG_INF/WRN/DBG.
 *
 * See log_event.h for the ownership and overflow contract. This is a
 * separate ring from the event ring above: events are fixed 8-byte records
 * written from hard callbacks, text is variable-length and written from
 * tasks.
 * ------------------------------------------------------------------ */

#define LOG_TEXT_RING          4096u   /* power of two */
#define LOG_TEXT_DRAIN_RECORDS 16u     /* records per drain call */
#define LOG_TEXT_DRAIN_BYTES   256u    /* UART byte budget per drain call */

static char             text_ring[LOG_TEXT_RING];
static volatile uint32_t text_head = 0;    /* next byte to write */
static volatile uint32_t text_tail = 0;    /* next byte to read */
static volatile uint32_t text_dropped = 0;

/* Output staging. Only log_text_drain() (log_task) touches these. */
static char     tx_buf[LOG_TEXT_MSG_MAX];
static uint16_t tx_len = 0;
static uint16_t tx_off = 0;

/* Reduce a ring byte offset to [0, LOG_TEXT_RING). Records are stored as
 * [2-byte length][payload] and either part may wrap, so callers legitimately
 * pass unnormalised sums such as text_tail + 2. Normalising before any
 * subtraction is what keeps `LOG_TEXT_RING - pos` from underflowing. */
static inline uint32_t text_ring_pos(uint32_t pos)
{
    return pos % LOG_TEXT_RING;   /* power of two: compiles to a mask */
}

static inline uint32_t text_used_locked(void)
{
    uint32_t h = text_ring_pos(text_head), t = text_ring_pos(text_tail);
    return (h >= t) ? (h - t) : (LOG_TEXT_RING - t + h);
}

/* Copy n bytes out of the ring at an arbitrary offset. */
static void text_ring_read_locked(uint32_t pos, char *dst, uint32_t n)
{
    uint32_t first;

    pos = text_ring_pos(pos);
    first = LOG_TEXT_RING - pos;
    if (first > n)
        first = n;
    memcpy(dst, &text_ring[pos], first);
    if (n > first)
        memcpy(dst + first, &text_ring[0], n - first);
}

/* Append n bytes at head, wrapping. Caller holds the IRQ mask. */
static void text_ring_write_locked(const char *src, uint32_t n)
{
    uint32_t first;

    text_head = text_ring_pos(text_head);
    first = LOG_TEXT_RING - text_head;
    if (first > n)
        first = n;
    memcpy(&text_ring[text_head], src, first);
    if (n > first)
        memcpy(&text_ring[0], src + first, n - first);
    text_head = text_ring_pos(text_head + n);
}

/* printf()'s console sink emits CR before LF (SDK out_console). Reproduce
 * that here so a serial terminal renders the deferred text exactly like the
 * old synchronous output. In-place, backward, bounded; if inserting the CRs
 * would not fit, the text is kept unchanged rather than truncated. */
static uint32_t text_add_crlf(char *msg, uint32_t n)
{
    uint32_t extra = 0, w;

    for (uint32_t i = 0; i < n; i++) {
        if (msg[i] == '\n' && (i == 0u || msg[i - 1u] != '\r'))
            extra++;
    }
    if (extra == 0u)
        return n;
    if (n + extra > LOG_TEXT_MSG_MAX - 1u)
        return n;

    w = n + extra;
    msg[w] = '\0';
    for (uint32_t i = n; i > 0u; i--) {
        char c = msg[i - 1u];
        msg[--w] = c;
        if (c == '\n' && (i < 2u || msg[i - 2u] != '\r'))
            msg[--w] = '\r';
    }
    return n + extra;
}

void log_text_printf(const char *fmt, ...)
{
    char msg[LOG_TEXT_MSG_MAX];
    va_list ap;
    int n;
    uint32_t len, need, key;
    char hdr[2];

    if (fmt == NULL)
        return;

    /* Formatting happens outside the masked section. vsnprintf uses only
     * this stack buffer: no locks, no allocation. */
    va_start(ap, fmt);
    n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if ((uint32_t)n > sizeof msg - 1u)
        n = (int)(sizeof msg - 1u);

    len = text_add_crlf(msg, (uint32_t)n);
    need = len + 2u;

    key = LOG_EVT_ENTER();
    if (need > (LOG_TEXT_RING - 1u) - text_used_locked()) {
        /* Full: keep the older context, drop this record. */
        text_dropped++;
        LOG_EVT_EXIT(key);
        return;
    }
    hdr[0] = (char)(len & 0xFFu);
    hdr[1] = (char)((len >> 8) & 0xFFu);
    text_ring_write_locked(hdr, 2u);
    text_ring_write_locked(msg, len);
    LOG_EVT_EXIT(key);
}

/* Pop one record into dst (NUL-terminated). Returns length or 0. */
static int text_pop(char *dst, uint32_t cap, uint32_t *out_len)
{
    uint32_t key, used, len;
    char hdr[2];
    int got = 0;

    key = LOG_EVT_ENTER();
    used = text_used_locked();
    if (used >= 2u) {
        text_ring_read_locked(text_tail, hdr, 2u);
        len = (uint32_t)(uint8_t)hdr[0] | ((uint32_t)(uint8_t)hdr[1] << 8);
        if (len == 0u || len >= cap || (len + 2u) > used) {
            /* Cannot happen unless the ring was corrupted; fail safe. */
            text_head = 0;
            text_tail = 0;
            text_dropped++;
        } else {
            uint32_t payload = text_ring_pos(text_tail + 2u);

            text_ring_read_locked(payload, dst, len);
            dst[len] = '\0';
            text_tail = text_ring_pos(text_tail + 2u + len);
            *out_len = len;
            got = 1;
        }
    }
    LOG_EVT_EXIT(key);
    return got;
}

#if !defined(LOG_EVENT_HOST_TEST)
extern struct bflb_device_s *console;

/* Write one byte only if the TX FIFO already has room, atomically.
 *
 * bflb_uart_putchar() does not report "full" — on a full FIFO it spins for
 * up to 100 ms (bflb_uart.c). Testing bflb_uart_txready() a moment earlier is
 * therefore not a non-blocking guarantee: an interrupt (LOG_ERR's synchronous
 * printf, or a tick-driven task switch) can run in the gap and take the last
 * free slot, after which putchar() blocks.
 *
 * Masking interrupts makes the check and the write one step. This is sound on
 * this single-core part: the console TX FIFO is written only by software on
 * this core, so under the mask its occupancy can only fall (the shifter
 * drains it). A txready() that is true under the mask is still true when
 * putchar() samples it, so putchar() cannot enter its wait loop. The window
 * covers exactly one byte, keeping the IRQ-off latency to a few cycles even
 * at the 256-byte drain budget.
 *
 * bflb_irq_save()/restore() record and restore the previous mask state, so
 * the helper cannot un-mask an outer critical section: if it is ever reached
 * with interrupts already disabled, it stays disabled. LOG_ERR does not use
 * this helper at all — it is synchronous printf() straight to the SDK UART
 * writer — so it can neither be blocked by this mask nor deadlock it. */
static int uart_put_byte_nowait(struct bflb_device_s *dev, unsigned char ch)
{
    uintptr_t key = bflb_irq_save();
    int ret = -1;

    if (bflb_uart_txready(dev))
        ret = bflb_uart_putchar(dev, ch);

    bflb_irq_restore(key);
    return ret;
}

/* Push at most budget bytes into the console UART without blocking: stop as
 * soon as the TX FIFO is full and resume on the next drain call. Used in
 * every build, DEBUG_CONSOLE included — UART is the primary log path. */
static uint32_t uart_emit(const char *buf, uint32_t len, uint32_t *budget)
{
    uint32_t sent = 0;

    if (console == NULL)
        return len;   /* no console configured: discard */

    while (sent < len && *budget > 0u) {
        if (uart_put_byte_nowait(console, (unsigned char)buf[sent]) < 0)
            break;   /* FIFO full: resume on the next drain call */
        sent++;
        (*budget)--;
    }
    return sent;
}
#endif

uint32_t log_text_drain(void)
{
    uint32_t records = 0;
#if !defined(LOG_EVENT_HOST_TEST)
    uint32_t budget = LOG_TEXT_DRAIN_BYTES;
#endif

    /* Bounded work per call so a log storm cannot starve the caller. */
    while (records < LOG_TEXT_DRAIN_RECORDS) {
        if (tx_off >= tx_len) {
            uint32_t len = 0;
            if (!text_pop(tx_buf, sizeof tx_buf, &len))
                break;
            tx_len = (uint16_t)len;
            tx_off = 0;
            records++;
#ifdef DEBUG_CONSOLE
            /* Secondary sink: hand the whole record to the 64 KB ring once,
             * at pop time. A partial UART write resumes the same staging
             * buffer, so pushing here cannot duplicate it. */
            debug_console_write(tx_buf, (int)tx_len);
#endif
        }

#if defined(LOG_EVENT_HOST_TEST)
        /* Host test: no UART, no console ring. */
        fwrite(&tx_buf[tx_off], 1u, (size_t)(tx_len - tx_off), stdout);
        tx_off = tx_len;
#else
        /* Every build writes to the UART. The staging buffer is owned by the
         * drain task, and uart_emit() masks interrupts around each FIFO
         * check/write pair so another logger cannot steal the last slot. */
        tx_off = (uint16_t)(tx_off + uart_emit(&tx_buf[tx_off],
                                               (uint32_t)(tx_len - tx_off),
                                               &budget));
        if (tx_off < tx_len)
            break;   /* TX FIFO full: resume next call, keep record order */
#endif
    }
    return records;
}

uint32_t log_text_dropped(void)
{
    return text_dropped;
}

uint32_t log_text_pending(void)
{
    uintptr_t key = LOG_EVT_ENTER();
    uint32_t used = text_used_locked();
    LOG_EVT_EXIT(key);
    return used;
}

void log_text_reset(void)
{
    uintptr_t key = LOG_EVT_ENTER();
    text_head = 0;
    text_tail = 0;
    text_dropped = 0;
    LOG_EVT_EXIT(key);
    tx_len = 0;
    tx_off = 0;
}
