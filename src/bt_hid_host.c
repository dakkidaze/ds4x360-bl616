#include "bt_hid_host.h"
#include "ds4_protocol.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/conn.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/sdp.h>
#include <bluetooth/hci_host.h>
#include "conn_internal.h"
#include "keys.h"
#include "settings.h"
#include <string.h>
#include "debug_log.h"
#include "log_event.h"
#include <errno.h>
#include "bflb_efuse.h"
#include "bflb_irq.h"
#include "hci_core.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "bl616_glb.h"

/* R01: everything in this file runs in Bluetooth receive/state callbacks, the
 * HCI tx-done callback or BT send hot paths. Route every LOG_* to the event
 * ring instead of printf: the message text is dropped (no formatting, no
 * UART), and the source line becomes the event code — use the line number to
 * identify it, or read the file at that line. */
#undef LOG_DBG
#undef LOG_INF
#undef LOG_WRN
#undef LOG_ERR
#define LOG_DBG(...) ((void)0)   /* debug level stays off: no event flood */
#define LOG_INF(...) log_event_put(LOG_EVT_BT_MISC, (uint16_t)__LINE__, 0)
#define LOG_WRN(...) log_event_put(LOG_EVT_BT_MISC, (uint16_t)__LINE__, 1)
#define LOG_ERR(...) log_event_put(LOG_EVT_BT_MISC, (uint16_t)__LINE__, 2)


#define BT_INQUIRY_LEN         5
#define BT_MAX_DISCOVERED      8
#define L2CAP_BR_MTU           672

/* ---------- feature report cache (N05) ----------
 *
 * The cache is published data, not a shared scratch buffer: USB must never
 * hold a pointer into it. Each slot is a version that is only observable
 * while `len` is non-zero, and every transition (store or clear) happens in
 * one bflb_irq_save() window, so a reader either sees the previous complete
 * version or the next complete one:
 *
 *   writer: [unpublish len=0] -> [bounded memcpy <= FEATURE_DATA_MAX]
 *           -> [report_id] -> [publish len]      (all IRQs masked)
 *   reader: bt_hid_host_feature_snapshot() copies report_id-matched bytes
 *           and the length from the *same* window, never waiting on the
 *           writer (PRIMASK means no writer can interleave at all).
 *
 * A store therefore does not clear payload bytes a reader may currently be
 * copying: the reader's copy is bounded by the length it latched, and the
 * slot cannot change underneath it. A disconnect clear publishes "empty" by
 * dropping every length in one short window instead of memset()ing ~6 KB.
 */

#define FEATURE_CACHE_SLOTS  24
#define FEATURE_DATA_MAX     256

static struct {
    uint8_t  report_id;
    uint8_t  data[FEATURE_DATA_MAX];
    uint16_t len;      /* 0 = no published version in this slot */
} feature_cache[FEATURE_CACHE_SLOTS];

static void feature_cache_clear(void)
{
    uintptr_t key = bflb_irq_save();
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++)
        feature_cache[i].len = 0;
    bflb_irq_restore(key);
}

static void feature_cache_store(uint8_t report_id,
                                const uint8_t *data, uint16_t len)
{
    if (len > FEATURE_DATA_MAX)
        len = FEATURE_DATA_MAX;

    uintptr_t key = bflb_irq_save();

    int slot = -1;
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id && feature_cache[i].len > 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
            if (feature_cache[i].len == 0) {
                slot = i;
                break;
            }
        }
    }
    if (slot < 0)
        slot = 0;

    /* Unpublish, then build the new version in place with a fixed-size
     * copy, then publish. Bounded: no allocation, no blocking, no UART. */
    feature_cache[slot].len = 0;
    memcpy(feature_cache[slot].data, data, len);
    feature_cache[slot].report_id = report_id;
    feature_cache[slot].len = len;   /* commit */

    bflb_irq_restore(key);
}

/* ---------- persistent blacklist ----------
 *
 * When the user clears bonds (BOOT long-press), all bonded MACs are added
 * to a persistent blacklist stored in flash. This prevents the controller
 * from silently re-pairing via PS-only button press (controller-initiated
 * reconnect). The user must put the controller into PS+Share pairing mode
 * (dongle-initiated inquiry) to re-pair, which removes the MAC from the
 * blacklist on successful L2CAP HID connection.
 * Mirrors DS5Dongle's bt_cleared_addrs / BLCK TLV mechanism. */

#define BLACKLIST_MAX     8
#define NV_BLACKLIST_KEY  "DS4_BLK"

static bt_addr_t blacklist_addrs[BLACKLIST_MAX];
static int       blacklist_count;
static volatile bool blacklist_dirty;

static void blacklist_load(void)
{
    size_t real_len = 0;
    int ret = bt_settings_get_bin(NV_BLACKLIST_KEY,
                                  (u8_t *)blacklist_addrs,
                                  sizeof(blacklist_addrs), &real_len);
    if (ret == 0 && real_len > 0 &&
        (real_len % sizeof(bt_addr_t)) == 0) {
        blacklist_count = (int)(real_len / sizeof(bt_addr_t));
        if (blacklist_count > BLACKLIST_MAX)
            blacklist_count = BLACKLIST_MAX;
        LOG_INF("[BT] Blacklist: loaded %d entries\n", blacklist_count);
    } else {
        blacklist_count = 0;
    }
}

static void blacklist_persist(void)
{
    if (blacklist_count == 0) {
        settings_delete(NV_BLACKLIST_KEY);
    } else {
        bt_settings_set_bin(NV_BLACKLIST_KEY,
                            (const u8_t *)blacklist_addrs,
                            blacklist_count * sizeof(bt_addr_t));
    }
}

static bool blacklist_contains(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0)
            return true;
    }
    return false;
}

static void __attribute__((unused)) blacklist_add(const bt_addr_t *addr)
{
    if (blacklist_count >= BLACKLIST_MAX || blacklist_contains(addr))
        return;
    bt_addr_copy(&blacklist_addrs[blacklist_count++], addr);
    char a[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, a, sizeof(a));
    LOG_INF("[BT] Blacklist: added %s\n", a);
}

static void blacklist_remove(const bt_addr_t *addr)
{
    for (int i = 0; i < blacklist_count; i++) {
        if (bt_addr_cmp(addr, &blacklist_addrs[i]) == 0) {
            for (int j = i; j < blacklist_count - 1; j++)
                bt_addr_copy(&blacklist_addrs[j], &blacklist_addrs[j + 1]);
            blacklist_count--;
            LOG_INF("[BT] Blacklist: removed entry\n");
            return;
        }
    }
}

/* ---------- multi-controller memory ---------- */

#define MAX_BONDED_CONTROLLERS 8
#define NV_ACTIVE_CTRL_KEY     "active_ctrl"

static bt_addr_t bonded_list[MAX_BONDED_CONTROLLERS];
static uint8_t   bonded_count = 0;
static uint8_t   active_idx   = 0;

static void bonded_list_collect_cb(const struct bt_br_bond_info *info,
                                   void *user_data)
{
    (void)user_data;
    if (bonded_count < MAX_BONDED_CONTROLLERS) {
        bt_addr_copy(&bonded_list[bonded_count], info->addr);
        bonded_count++;
    }
}

static void bonded_list_load(void)
{
    bonded_count = 0;
    bt_br_foreach_bond(bonded_list_collect_cb, NULL);

    size_t real_len = 0;
    uint8_t stored_idx = 0;
    int ret = bt_settings_get_bin(NV_ACTIVE_CTRL_KEY, &stored_idx,
                                  sizeof(stored_idx), &real_len);
    if (ret == 0 && real_len == 1 && stored_idx < bonded_count) {
        active_idx = stored_idx;
    } else {
        active_idx = 0;
    }

    LOG_INF("[BT] Bonded list: %d controllers, active_idx=%d\n",
           bonded_count, active_idx);
    for (int i = 0; i < bonded_count; i++) {
        char a[BT_ADDR_STR_LEN];
        bt_addr_to_str(&bonded_list[i], a, sizeof(a));
        LOG_INF("[BT]   [%d]%s %s\n", i, (i == active_idx) ? "*" : " ", a);
    }
}

static void bonded_list_persist_idx(void)
{
    bt_settings_set_bin(NV_ACTIVE_CTRL_KEY, &active_idx, sizeof(active_idx));
}

static int bonded_list_find_addr(const bt_addr_t *addr)
{
    for (int i = 0; i < bonded_count; i++) {
        if (bt_addr_cmp(&bonded_list[i], addr) == 0)
            return i;
    }
    return -1;
}

/* ---------- internal state ---------- */

static struct {
    volatile enum bt_hid_host_state state;
    struct bt_conn *conn;
    bt_addr_t target_addr;
    bool has_target;

    struct bt_l2cap_br_chan ctrl_chan;
    struct bt_l2cap_br_chan intr_chan;
    volatile bool ctrl_connected;
    volatile bool intr_connected;
    /* Channel generation (R04): bumped on every interrupt-channel open,
     * close and teardown. A disconnect/reconnect pair on the same ACL keeps
     * hid_ctx.conn and does not move app_tx_epoch, so the epoch alone cannot
     * tell an old CID/MTU from the current one; this counter can. */
    volatile uint32_t intr_gen;
    volatile bool ctrl_accepting;
    volatile bool intr_accepting;
    bool hid_service_found;

    volatile bool outgoing;

    volatile bool connect_pending;
    volatile TickType_t connect_pending_tick;
    bt_addr_t     pending_addr;

    volatile bool fallback_retry;

    volatile bool sdp_pending;

    /* Security establishment deferred to task context */
    volatile bool security_pending;
    volatile bool security_done;
    volatile TickType_t acl_connect_tick;
    volatile TickType_t security_done_tick;

    /* Paced handshake: send feature GETs one at a time */
    const uint8_t *handshake_ids;
    volatile uint8_t handshake_count;
    volatile uint8_t handshake_next;
    volatile bool handshake_send_failed;

    bt_hid_input_cb_t  input_cb;
    bt_hid_state_cb_t  state_cb;
    bt_hid_primer_cb_t primer_cb;
    uint16_t filter_vid;
    uint16_t filter_pid;
} hid_ctx;

/* L2CAP requires headroom for ACL + L2CAP headers (typically ~8 bytes).
 * BT_L2CAP_BUF_SIZE adds this headroom to the payload MTU. */
#ifndef BT_L2CAP_BUF_SIZE
#define BT_L2CAP_BUF_SIZE(mtu) ((mtu) + 8)
#endif

struct net_buf_pool hid_tx_pool;
struct net_buf_pool hid_rx_pool;

/* R01: Bluetooth receive callbacks must not format or print. These counters
 * are updated in RX context and read from a task. */
volatile uint32_t g_bt_intr_pkts   = 0;   /* input reports received */
volatile uint32_t g_bt_intr_bad    = 0;   /* unexpected interrupt-channel data */
volatile uint32_t g_bt_ctrl_pkts   = 0;   /* control-channel handshakes */
volatile uint32_t g_bt_feature_pkts = 0;  /* feature reports received */
volatile uint32_t g_bt_handshake_err = 0; /* GET_REPORT send failures */
volatile uint32_t g_bt_tx_pool_empty = 0; /* drain: net_buf pool exhausted */
volatile uint32_t g_bt_tx_send_err = 0;   /* drain: L2CAP send failures */
static bool first_output_logged = false;

/* ---------- Application-level TX FIFO (CAN_SEND_NOW equivalent) ----------
 *
 * DS5Dongle uses BTstack's L2CAP_EVENT_CAN_SEND_NOW for flow control:
 *   bt_write() → enqueue to send_fifo (10-deep)
 *   CAN_SEND_NOW fires → dequeue one → l2cap_send → if more, request again
 *
 * Zephyr equivalent:
 *   bt_hid_host_send_output() → enqueue to app_tx_fifo (10-deep)
 *   on_hid_output_sent() fires when HCI ACKs the packet → drain next one
 *
 * This ensures only ONE packet is in-flight in the BT stack at any time,
 * preventing net_buf pool exhaustion and HCI credit starvation.
 *
 * R04 ownership model — one writer per item, no blocking anywhere, and no
 * BT TX work in any latency-critical context:
 *
 *   producers (bt_task keepalive, micfeed/audio task, usb_task, primer):
 *       write one FIFO slot + app_tx_tail under APP_TX_LOCK(), then request
 *       the TX service (app_tx_kick()). They never allocate a net_buf, never
 *       submit and never block. The 1 ms mic feed therefore cannot be
 *       stalled by an empty controller TX pool. When the ring is full a
 *       producer drops the oldest frame (advances head) under the same lock;
 *       that is the only head move outside the send path.
 *   TX service task ("bttx", priority below bt/audio/USB):
 *       the ONLY context that allocates the net_buf and calls the L2CAP send
 *       path. It keeps at most one packet in the stack (app_tx_inflight) and
 *       owns every head move of the send path. It never runs in a producer,
 *       in the BT workqueue or in an L2CAP callback.
 *   TX-complete callback (BT tx-complete workqueue task):
 *       publishes completion (app_tx_done) only if the token it was called
 *       with equals the token of the submit currently in flight, then wakes
 *       the service task. It never submits and never waits, so a completion
 *       belonging to a previous connection — or to an earlier submit —
 *       cannot release the newer one.
 *   reset (disconnect / force-disconnect / switch / drop-stale):
 *       under the same lock, bumps app_tx_epoch, clears head/tail/inflight
 *       and detaches hid_ctx.conn; the released reference is unref'd after
 *       the lock is dropped. A pump that observed the old epoch re-validates
 *       before submitting and aborts, so a frame from the old session is
 *       never sent on the new one.
 *
 * The submit path holds a bt_conn reference for the duration of the send and
 * re-validates under the lock that this reference is still the current
 * connection and that the epoch did not move, so a disconnect between
 * validation and submit cannot make it send through a stale/NULL channel.
 * Tokens reject stale completions; they are not a lifetime guard.
 *
 * R04 hand-off protocol — validation and enqueue are one atomic step.
 * Holding the ACL reference keeps the connection object alive, but it does
 * not keep an L2CAP channel instance from being torn down and its CID
 * reassigned on that same ACL. The old shape therefore reopened the very
 * window it was trying to close: the last generation check ran, the lock was
 * dropped, and a potentially blocking submit (bounded TX-context wait inside
 * bt_conn_send_cb()) ran afterwards; an interrupt-channel disconnect/
 * reconnect landing in that gap retargeted the captured old CID. Re-checking
 * immediately before the same blocking call would only narrow the window.
 *
 * The protocol removes the window instead of narrowing it:
 *   1. Everything that can wait (net_buf_alloc, net_buf_reserve) happens
 *      BEFORE the final check.
 *   2. APP_TX_LOCK() (interrupt mask; no ISR, no other task, no BT RX or
 *      workqueue context can run) covers the frame-identity/session/CID/MTU/
 *      generation validation, the payload copy AND the hand-off itself.
 *   3. The hand-off is bt_l2cap_br_chan_send_handoff(), which cannot block:
 *      it takes its TX context with K_NO_WAIT and appends to conn->tx_queue
 *      with a ZERO tick timeout (-ENOBUFS when the queue is full, buffer
 *      consumed). There is no blocking operation left between the last check
 *      and the enqueue.
 *   4. That same call re-resolves the captured CID to a channel object on the
 *      connection and refuses it unless it is the exact channel instance the
 *      app captured (and its TX CID still matches). A retired CID that was
 *      unassigned or reassigned to another channel can therefore never
 *      receive the PDU. The SDK has no per-channel generation (callers own
 *      their channel objects and may reuse them for a new connection), so
 *      the incarnation counter lives with the owner: intr_gen.
 *   5. If the generation moved, the frame is dropped by identity and counted
 *      (g_bt_tx_stale_meta) before any buffer is handed over.
 *
 * R04 frame identity: an index is a storage location, never a frame. Each
 * enqueue stamps the slot with a monotonic `frame_id` and the submit captures
 * id + length + CID + MTU + interrupt-channel generation + session (conn and
 * epoch) in one window, then re-validates all of them together with the
 * payload copy after allocating the net_buf. A recycled slot differs in id
 * (and usually length), so it can no longer be sent with the previous frame's
 * length; a disconnect/reconnect of the interrupt channel on the same ACL
 * keeps conn and epoch but moves the generation (and the CID/MTU), so a
 * captured channel is likewise refused. Drop/retire decisions are taken by
 * id, and the retry budget is keyed by id, so no path can act on a frame it
 * did not observe. Rejects are counted per class:
 *   g_bt_tx_stale_frame  slot recycled (id mismatch)
 *   g_bt_tx_stale_conn   session changed (conn/epoch/intr_connected)
 *   g_bt_tx_stale_meta   channel changed (CID/MTU/generation/length, or the
 *                        SDK refusing the captured CID at hand-off)
 *   g_bt_tx_oversize     frame cannot fit the (current) channel MTU
 *
 * app_tx_head only advances on a successful hand-off (or an explicit drop).
 * The old code advanced head provisionally and rolled it back on failure,
 * which erased the whole queue when a producer refilled the last slot before
 * the rollback.
 *
 * APP_TX_LOCK() is a short FreeRTOS critical section (interrupt mask). It is
 * never held across net_buf_alloc(), a log call or anything else that can
 * block; it IS held across the non-blocking L2CAP hand-off (a header push, a
 * zero-timeout queue append and a semaphore give), which is what makes the
 * last validation and the enqueue indivisible. All writers are task context
 * (producers are FreeRTOS tasks, the completion runs in the BT workqueue
 * task); counters use __atomic so reads stay consistent if a caller ever moves
 * into an ISR.
 */
#define APP_TX_DEPTH      10
/* One slot carries the HID transport byte plus a full L2CAP SDU, so every
 * report the interrupt channel can carry fits a slot. That removes the old
 * "direct" path, which allocated and submitted from the producer context. */
#define APP_TX_DATA_SZ    (L2CAP_BR_MTU)   /* 672 = 1 HID hdr + 671 payload */
/* Consecutive *hard* failures for the frame at the head before it is dropped.
 * Bounded so a permanently broken link cannot spin the queue. Pool-empty and
 * would-block outcomes do not consume this budget (see app_tx_fail()). */
#define APP_TX_RETRY_MAX  4
#define APP_TX_SLOT_NONE  0xFF
/* Service task wake period: backstop for retrying a would-blocked frame and
 * for retiring a submit the controller never completed. */
#define APP_TX_SERVICE_TICK_MS  10
/* Service task: below bt (MAX-3) and audio, so it never delays them; above
 * led/log so a queued report drains promptly when the higher tasks sleep.
 *
 * R11: 512 words (2 KB) is a conservative configuration value, not a measured
 * margin — raised from 320 words (1.25 KB) and to be confirmed with
 * main_log_stack_watermarks() on hardware (same convention as the micfeed
 * task stack in main.c). The priority is unchanged. */
#define APP_TX_TASK_STACK_WORDS 512
#define APP_TX_TASK_PRIORITY    (tskIDLE_PRIORITY + 3)

#define APP_TX_LOCK()     taskENTER_CRITICAL()
#define APP_TX_UNLOCK()   taskEXIT_CRITICAL()

static uint8_t  app_tx_buf[APP_TX_DEPTH][APP_TX_DATA_SZ];
static uint16_t app_tx_len[APP_TX_DEPTH];

/* Frame identity (R04). A slot index is a *storage location*, not a frame:
 * the ring wraps, and a higher-priority producer may refill a slot that a
 * submit has already picked. Every enqueue therefore stamps the slot with a
 * monotonic, never-zero `frame_id`; the submit captures the id together with
 * the length/CID/MTU/session it was read with and re-validates the id at the
 * commit point. An index match alone is not identity: a recycled slot keeps
 * its index but carries a different id (and a different length). A dropped
 * frame publishes id 0, which no submit ever captures. */
static uint32_t app_tx_frame_id[APP_TX_DEPTH];
static uint32_t app_tx_frame_next = 0;   /* monotonic, never 0 */

/* Ring + in-flight state: guarded by APP_TX_LOCK(). */
static volatile uint8_t  app_tx_head = 0;   /* oldest frame, not yet accepted */
static volatile uint8_t  app_tx_tail = 0;   /* next slot to fill */
static volatile bool     app_tx_inflight = false; /* one submit owns the chan */
static volatile uint8_t  app_tx_inflight_slot = APP_TX_SLOT_NONE;
static volatile bool     app_tx_done = false;     /* controller ACKed inflight */
static volatile uint32_t app_tx_token = 0;        /* token of that submit */
static volatile uint32_t app_tx_token_next = 0;   /* monotonic, never 0 */
static volatile uint32_t app_tx_epoch = 0;        /* bumped on every reset */
/* Retry budget, owned by the frame identity in app_tx_retry_id (0 = none).
 * Keyed by frame id, not by slot, so a recycled ring slot can never inherit
 * another frame's budget. Reset only on a successful hand-off or when the
 * frame at the head changes. */
static volatile uint8_t  app_tx_retry = 0;        /* failed attempts, head */
static volatile uint32_t app_tx_retry_id = 0;
static volatile TickType_t app_tx_inflight_since = 0; /* when it was submitted */

/* TX service wake-up. Never given with a timeout and never taken by a
 * producer, so it cannot block a producer either. */
static SemaphoreHandle_t app_tx_wake;
static TaskHandle_t      app_tx_task_handle;

/* A submit that the controller never completes (broken ACL, dropped
 * num_completed_packets event) must not hold the queue forever. Retired by
 * the service task; a late completion for the retired token is then counted
 * as stale and ignored. */
#define APP_TX_INFLIGHT_TIMEOUT_MS  500

/* R04 counters. Non-static so the diagnostics lane can export them; updated
 * with atomics because producers live in several task contexts. */
volatile uint32_t g_bt_tx_submitted   = 0; /* packets handed to L2CAP */
volatile uint32_t g_bt_tx_stale_done  = 0; /* completions for an older token */
volatile uint32_t g_bt_tx_retry       = 0; /* hard failures retried */
volatile uint32_t g_bt_tx_retry_drop  = 0; /* frames dropped, retry exhausted */
volatile uint32_t g_bt_tx_drop_full   = 0; /* frames dropped, FIFO full */
volatile uint32_t g_bt_tx_direct_drop = 0; /* report larger than a slot */
volatile uint32_t g_bt_tx_inflight_timeout = 0; /* submits retired on timeout */
volatile uint32_t g_bt_tx_would_block = 0; /* pool empty / send would block */
volatile uint32_t g_bt_tx_stale_conn  = 0; /* session changed before submit */
/* Frame-identity reject classes (R04). */
volatile uint32_t g_bt_tx_stale_frame = 0; /* slot recycled: frame_id mismatch */
volatile uint32_t g_bt_tx_stale_meta  = 0; /* CID/MTU/channel generation moved */
volatile uint32_t g_bt_tx_oversize    = 0; /* frame dropped: len > channel MTU */
/* Control-channel (feature/handshake) send attempts that failed or were
 * refused: includes the pre-hand-off session/MTU rejection (buffer released
 * by hid_ctrl_send()), every hand-off error (buffer consumed by the stack),
 * and the pre-hand-off HID TX-pool exhaustion in the control helpers (nothing
 * allocated, nothing to release). The retryable would-block below is included
 * as well. */
volatile uint32_t g_bt_ctrl_send_err  = 0;
/* Subset of g_bt_ctrl_send_err: the "not accepted, retry later" class, i.e.
 * -ENOBUFS from the non-blocking control hand-off (TX queue full or no TX
 * context) and net_buf_alloc(&hid_tx_pool, K_NO_WAIT) returning NULL in
 * bt_hid_host_get_feature()/set_feature()/set_feature_crc(). hid_ctrl_send()
 * reports -ENOBUFS to callers as BT_HID_SEND_WOULD_BLOCK and the buffer is
 * consumed; on pool exhaustion there was never a buffer to consume. Either
 * way the caller (or the handshake tick) re-sends with a fresh PDU. */
volatile uint32_t g_bt_ctrl_would_block = 0;

#define APP_TX_CNT_INC(c) \
    __atomic_add_fetch((volatile uint32_t *)&(c), 1U, __ATOMIC_RELAXED)

/* Interrupt-channel generation. Written from BT RX/state-callback context,
 * read from the TX service task, so both sides use atomics. The value is
 * only ever compared for equality, never ordered against the payload. */
static inline uint32_t intr_gen_load(void)
{
    return __atomic_load_n(&hid_ctx.intr_gen, __ATOMIC_RELAXED);
}

static inline void intr_gen_bump(void)
{
    __atomic_add_fetch(&hid_ctx.intr_gen, 1U, __ATOMIC_RELAXED);
}

/* bt_conn_tx_cb_t: SDK declaration from conn_internal.h */
static void app_tx_kick(void);
static void app_tx_fifo_reset(void);
static void app_tx_service(void);
static void app_tx_task(void *arg);

/* BR/EDR verified hand-off from l2cap_br.c (R04).
 *
 * Same consumption rule as bt_l2cap_br_chan_send() — `buf` is consumed on
 * every return except a NULL buffer — but the call never waits: the TX context
 * is probed with K_NO_WAIT and the ACL TX queue is appended to with a zero
 * tick timeout, so a full queue is reported as -ENOBUFS instead of waited on.
 * Before queueing it re-checks that the channel is
 * still bound to the live ACL *and* that the captured CID still resolves to
 * this exact channel object, so a CID that was retired and reassigned during
 * the submit cannot receive the PDU.
 *
 * This is the only path the TX service task uses: it validates the frame and
 * calls this in the same non-preemptible window (APP_TX_LOCK), so there is no
 * blocking step between the last generation check and the enqueue. The
 * control-channel path uses it the same way: final session/channel validation,
 * live-CID capture and the enqueue all happen in one APP_TX_LOCK() window, and
 * the enqueue below is zero-timeout, so it cannot wait there either. */
extern int bt_l2cap_br_chan_send_handoff(struct bt_l2cap_chan *chan,
                                         uint16_t cid, struct net_buf *buf,
                                         bt_conn_tx_cb_t cb, void *user_data);

static void on_hid_output_sent(struct bt_conn *conn, void *user_data)
{
    /* HCI controller acknowledged the single packet in flight. Publish the
     * completion and wake the TX service task; this context never submits and
     * never waits (it is the BT workqueue, which also frees TX contexts).
     * Callback context: counters only, no printf. */
    (void)conn;

    uint32_t token = (uint32_t)(uintptr_t)user_data;
    bool mine = false;

    APP_TX_LOCK();
    if (app_tx_inflight && token != 0 && token == app_tx_token) {
        app_tx_done = true;      /* the service task clears the claim */
        mine = true;
    }
    APP_TX_UNLOCK();

    if (!mine) {
        /* Completion from a previous connection or an earlier submit: it
         * must not release the packet that is in flight now. */
        APP_TX_CNT_INC(g_bt_tx_stale_done);
        return;
    }

    app_tx_kick();
}

/* Advance head past `slot` unless it was already dropped/consumed, and
 * retire that slot's frame identity in the same step (id 0 = "no frame
 * here"). Caller holds APP_TX_LOCK(). The send path moves head here (and on
 * an explicit drop) only; a producer advances head solely to discard the
 * oldest frame when the ring is full. */
static void app_tx_head_advance_locked(uint8_t slot)
{
    if (slot != APP_TX_SLOT_NONE && app_tx_head == slot &&
        app_tx_head != app_tx_tail) {
        app_tx_frame_id[slot] = 0;
        app_tx_head = (uint8_t)((app_tx_head + 1) % APP_TX_DEPTH);
    }
}

/* Consume the in-flight submit when the controller acknowledged it and
 * release the claim. Returns true when the service task may submit again.
 * Head advances here — never on submit — so a frame that was not accepted
 * stays queued. */
static bool app_tx_consume_done(void)
{
    bool ready;

    APP_TX_LOCK();
    if (!app_tx_inflight) {
        ready = true;
    } else if (app_tx_done) {
        app_tx_head_advance_locked(app_tx_inflight_slot);
        app_tx_inflight = false;
        app_tx_inflight_slot = APP_TX_SLOT_NONE;
        app_tx_done = false;
        app_tx_token = 0;
        app_tx_retry = 0;                 /* budget belonged to the frame */
        app_tx_retry_id = 0;
        ready = true;
    } else {
        ready = false;                    /* still outstanding */
    }
    APP_TX_UNLOCK();
    return ready;
}

/* Record one failed attempt for the frame that was just submitted and release
 * the claim. `frame_id` identifies that exact frame: if the head has been
 * recycled to another frame in the meantime, its budget is left alone instead
 * of being charged for someone else's failure.
 * `would_block` (net_buf pool empty / controller TX context exhausted) keeps
 * the frame and does NOT touch the retry budget; a hard send error does, and
 * the frame is dropped once APP_TX_RETRY_MAX hard failures pile up so the
 * queue can never be wedged behind an unsendable frame. */
static void app_tx_fail(uint32_t token, uint32_t frame_id, bool would_block)
{
    bool dropped = false;

    APP_TX_LOCK();
    if (app_tx_inflight && app_tx_token == token) {
        app_tx_inflight = false;
        app_tx_inflight_slot = APP_TX_SLOT_NONE;
        app_tx_done = false;
        app_tx_token = 0;

        if (!would_block && app_tx_head != app_tx_tail &&
            app_tx_frame_id[app_tx_head] == frame_id) {
            if (app_tx_retry_id != frame_id) {
                app_tx_retry = 0;          /* counter follows this exact frame */
                app_tx_retry_id = frame_id;
            }
            if (app_tx_retry < 0xFF)
                app_tx_retry++;
            if (app_tx_retry >= APP_TX_RETRY_MAX) {
                if (app_tx_head != app_tx_tail) {
                    app_tx_head_advance_locked(app_tx_head);
                    dropped = true;
                }
                app_tx_retry = 0;
                app_tx_retry_id = 0;
            }
        }
    }
    APP_TX_UNLOCK();

    if (would_block) {
        APP_TX_CNT_INC(g_bt_tx_would_block);
        return;
    }

    APP_TX_CNT_INC(g_bt_tx_send_err);
    if (dropped)
        APP_TX_CNT_INC(g_bt_tx_retry_drop);
    else
        APP_TX_CNT_INC(g_bt_tx_retry);
}

/* Submit the frame at the head. Returns true when a packet was handed to
 * L2CAP (the caller may continue), false when it stopped (empty / one in
 * flight / would-block / failure). Service task context only.
 *
 * Identity rule (R04): a ring index is only a storage location.
 * Everything the frame is made of — frame_id, length, payload, session (conn +
 * epoch) and channel state (CID + MTU + generation) — is captured in ONE lock
 * window, and re-validated in ONE lock window after the net_buf allocation,
 * together with the payload copy and the non-blocking L2CAP hand-off. Any
 * mismatch aborts without submitting and without touching a frame that the
 * ring may have recycled meanwhile. Because the hand-off is inside that same
 * window and cannot wait, no channel teardown/reconnect can land between the
 * last generation check and the enqueue. */
static bool app_tx_submit_head(void)
{
    struct bt_conn *conn;
    struct net_buf *buf;
    uint8_t  idx;
    uint16_t len, cid, mtu;
    uint32_t epoch, fid, gen, token;
    bool     stale_session = false;
    bool     stale_frame = false;
    bool     stale_meta = false;
    bool     oversize = false;
    bool     handoff = false;
    int      err = 0;

    /* Validate the session and pin the connection for the whole submit. The
     * reference is what keeps the connection slot from being recycled while
     * this frame is on its way into the stack. */
    APP_TX_LOCK();
    if (app_tx_inflight || app_tx_head == app_tx_tail) {
        APP_TX_UNLOCK();
        return false;
    }
    conn = hid_ctx.conn;
    if (!conn || !hid_ctx.intr_connected ||
        hid_ctx.intr_chan.chan.conn != conn ||
        hid_ctx.intr_chan.tx.cid == 0) {
        APP_TX_UNLOCK();
        return false;   /* no session: keep the frames, reset clears them */
    }
    idx   = app_tx_head;
    len   = app_tx_len[idx];
    fid   = app_tx_frame_id[idx];
    cid   = hid_ctx.intr_chan.tx.cid;
    mtu   = hid_ctx.intr_chan.tx.mtu;
    if (mtu == 0)                   /* not negotiated: the configured MTU */
        mtu = L2CAP_BR_MTU;
    epoch = app_tx_epoch;
    gen   = intr_gen_load();
    /* Take the connection reference before any early return below: every path
     * that leaves this lock window must release exactly the reference it took,
     * including the (unreachable) zero-id branch. */
    bt_conn_ref(conn);
    if (fid == 0) {
        /* A queued slot must always carry a published identity; treating a
         * head without one as unsendable (instead of sending whatever bytes
         * happen to be there) keeps a latch bug from becoming a bad report. */
        app_tx_head_advance_locked(idx);
        APP_TX_UNLOCK();
        bt_conn_unref(conn);
        APP_TX_CNT_INC(g_bt_tx_stale_frame);
        return false;
    }
    APP_TX_UNLOCK();

    if (len > mtu) {
        /* Cannot ever fit this channel: drop instead of burning the retry
         * budget on a send that is guaranteed to return -EMSGSIZE. Retire by
         * identity: if the slot already carries another frame, that frame is
         * not ours to drop. */
        APP_TX_LOCK();
        if (app_tx_head == idx && app_tx_frame_id[idx] == fid)
            app_tx_head_advance_locked(idx);
        APP_TX_UNLOCK();
        bt_conn_unref(conn);
        APP_TX_CNT_INC(g_bt_tx_oversize);
        APP_TX_CNT_INC(g_bt_tx_retry_drop);
        return false;
    }

    buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        /* Pool exhausted is a would-block, not a frame failure: keep the
         * frame, keep the budget, retry on the next service tick. */
        bt_conn_unref(conn);
        APP_TX_CNT_INC(g_bt_tx_pool_empty);
        APP_TX_CNT_INC(g_bt_tx_would_block);
        return false;
    }
    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);

    /* Re-validate under the lock: a disconnect (or a new connection) may
     * have happened while the buffer was being allocated, and the interrupt
     * channel may have been torn down and reopened on the same ACL (same
     * conn, same epoch, new CID/MTU). The payload copy also stays inside the
     * lock so a producer can never tear the frame. */
    APP_TX_LOCK();
    {
        uint16_t cur_mtu = hid_ctx.intr_chan.tx.mtu;

        if (cur_mtu == 0)
            cur_mtu = L2CAP_BR_MTU;

        if (app_tx_inflight || app_tx_head != idx ||
            app_tx_frame_id[idx] != fid) {
            /* Slot recycled / claim taken: this is no longer our frame. */
            stale_frame = true;
        } else if (app_tx_epoch != epoch || hid_ctx.conn != conn ||
                   !hid_ctx.intr_connected ||
                   hid_ctx.intr_chan.chan.conn != conn) {
            stale_session = true;
        } else if (cid != hid_ctx.intr_chan.tx.cid ||
                   hid_ctx.intr_chan.tx.cid == 0 ||
                   mtu != cur_mtu || gen != intr_gen_load()) {
            /* Same ACL, different channel: the captured CID/MTU belong to a
             * channel instance that no longer exists. */
            stale_meta = true;
        } else if (app_tx_len[idx] != len) {
            /* Identity matched but the length moved: reject rather than
             * fabricate a frame out of two different publishes. */
            stale_meta = true;
        } else if (len > cur_mtu) {
            /* MTU shrank while the buffer was being allocated. */
            oversize = true;
        } else {
            net_buf_add_mem(buf, app_tx_buf[idx], len);
            token = ++app_tx_token_next;
            if (token == 0)         /* 0 is reserved for "no submit" */
                token = ++app_tx_token_next;
            app_tx_token = token;
            app_tx_inflight = true;
            app_tx_inflight_slot = idx;
            app_tx_inflight_since = xTaskGetTickCount();
            app_tx_done = false;
            if (app_tx_retry_id != fid) {
                app_tx_retry = 0;   /* counter follows this exact frame */
                app_tx_retry_id = fid;
            }

            /* R04 hand-off protocol: the enqueue happens HERE, in the same
             * non-preemptible window as the validation above, and the call is
             * guaranteed not to wait (K_NO_WAIT TX context, zero-timeout
             * queue append -> -ENOBUFS). There is therefore no blocking step
             * between the last generation/CID check and the moment the PDU is
             * queued; a channel disconnect/reconnect cannot be interleaved.
             * The SDK side additionally re-resolves `cid` to this exact
             * channel object before queueing, which rejects a
             * retired/reassigned CID on its own. The buffer is owned by the
             * stack from here on, whatever the return value. */
            err = bt_l2cap_br_chan_send_handoff(&hid_ctx.intr_chan.chan,
                                                cid, buf, on_hid_output_sent,
                                                (void *)(uintptr_t)token);
            handoff = true;
        }
    }
    APP_TX_UNLOCK();

    if (oversize) {
        APP_TX_LOCK();
        if (app_tx_head == idx && app_tx_frame_id[idx] == fid)
            app_tx_head_advance_locked(idx);
        APP_TX_UNLOCK();
        net_buf_unref(buf);
        bt_conn_unref(conn);
        APP_TX_CNT_INC(g_bt_tx_oversize);
        APP_TX_CNT_INC(g_bt_tx_retry_drop);
        return false;
    }

    if (stale_frame || stale_session || stale_meta) {
        net_buf_unref(buf);
        bt_conn_unref(conn);
        /* One counter per reject class: a recycled slot, a changed session
         * and a changed channel are different faults. */
        if (stale_frame)
            APP_TX_CNT_INC(g_bt_tx_stale_frame);
        if (stale_session)
            APP_TX_CNT_INC(g_bt_tx_stale_conn);
        if (stale_meta)
            APP_TX_CNT_INC(g_bt_tx_stale_meta);
        return false;
    }

    if (!handoff) {
        /* Defensive: every accepted frame takes the hand-off branch above. */
        net_buf_unref(buf);
        bt_conn_unref(conn);
        return false;
    }

    /* From the hand-off return on, `buf` belongs to the stack: it was queued
     * on success and already unref'd on every failure. */
    bt_conn_unref(conn);            /* the stack holds its own ACL reference */

    if (err < 0) {
        if (err == -ENOTCONN) {
            /* The SDK refused the captured CID: the channel instance it
             * belongs to is gone (retired or rebound). Count it as the
             * channel-identity rejection it is; the retry budget still
             * bounds the frame. */
            APP_TX_CNT_INC(g_bt_tx_stale_meta);
        }
        app_tx_fail(token, fid, err == -ENOBUFS);
        return false;
    }

    APP_TX_CNT_INC(g_bt_tx_submitted);
    /* app_tx_inflight stays true until the completion for this token is
     * published (or the service task retires it on timeout). */
    return true;
}

/* One pump iteration: absorb an acknowledged submit, then try the next frame. */
static void app_tx_pump(void)
{
    for (unsigned i = 0; i <= APP_TX_DEPTH; i++) {
        if (!app_tx_consume_done())
            break;
        if (!app_tx_submit_head())
            break;
    }
}

/* Request one run of the TX service. This is the only thing a producer (or
 * the completion callback) is allowed to do with the send path: it wakes the
 * service task and returns immediately, so no producer can ever block on
 * net_buf/conn_tx allocation or on the L2CAP send. */
static void app_tx_kick(void)
{
    if (app_tx_wake && app_tx_task_handle) {
        /* Binary semaphore: coalesces repeated kicks, never blocks. */
        xSemaphoreGive(app_tx_wake);
        return;
    }

    /* Degraded mode only (task creation failed at init): submit inline rather
     * than leaving BT TX dead. Normal operation never reaches this. */
    app_tx_service();
}

/* Clear the ring + in-flight state. Caller holds APP_TX_LOCK(). */
static void app_tx_fifo_reset_locked(void)
{
    /* Invalidate everything in flight and bump the epoch. A submit that is
     * currently between its two checks will see the new epoch and abort
     * instead of sending an old-session frame on the new connection. */
    app_tx_head = 0;
    app_tx_tail = 0;
    app_tx_inflight = false;
    app_tx_inflight_slot = APP_TX_SLOT_NONE;
    app_tx_done = false;
    app_tx_token = 0;
    app_tx_retry = 0;
    app_tx_retry_id = 0;
    /* Drop every frame identity: nothing queued now can be mistaken for a
     * frame of the new session, even if the index range repeats. */
    for (unsigned i = 0; i < APP_TX_DEPTH; i++)
        app_tx_frame_id[i] = 0;
    app_tx_epoch++;
    intr_gen_bump();
}

static void app_tx_fifo_reset(void)
{
    APP_TX_LOCK();
    app_tx_fifo_reset_locked();
    APP_TX_UNLOCK();
}

/* Detach hid_ctx.conn from the TX session and clear the queue, atomically.
 *
 * The connection pointer is taken out of hid_ctx under the same lock the
 * service task uses to validate+ref it, so a submit can never ref a
 * connection this teardown is releasing. The unref happens after the lock is
 * dropped (it can reach into the stack). With `expect` non-NULL the teardown
 * only applies when that exact connection is still current (an old ACL
 * teardown must not clear the new session's queue).
 *
 * Returns the released reference, or NULL when nothing was detached. */
static struct bt_conn *app_tx_teardown(struct bt_conn *expect)
{
    struct bt_conn *conn;

    APP_TX_LOCK();
    conn = hid_ctx.conn;
    if (expect && conn != expect) {
        APP_TX_UNLOCK();
        return NULL;
    }
    hid_ctx.conn = NULL;
    hid_ctx.intr_connected = false;
    app_tx_fifo_reset_locked();
    APP_TX_UNLOCK();

    return conn;
}

/* Service task body: backstop for retries after a would-block, retirement of
 * a submit the controller never completed, and the actual drain. Without it,
 * recovery would depend on some unrelated report arriving later. */
static void app_tx_service(void)
{
    TickType_t now = xTaskGetTickCount();
    bool retired = false;

    APP_TX_LOCK();
    if (app_tx_inflight && !app_tx_done &&
        (TickType_t)(now - app_tx_inflight_since) >
            pdMS_TO_TICKS(APP_TX_INFLIGHT_TIMEOUT_MS)) {
        /* No completion arrived: free the slot so the queue can move on. If
         * the packet completes later its token no longer matches and it is
         * counted as stale instead of releasing a newer submit. */
        app_tx_head_advance_locked(app_tx_inflight_slot);
        app_tx_inflight = false;
        app_tx_inflight_slot = APP_TX_SLOT_NONE;
        app_tx_done = false;
        app_tx_token = 0;
        app_tx_retry = 0;
        app_tx_retry_id = 0;
        retired = true;
    }
    APP_TX_UNLOCK();

    if (retired)
        APP_TX_CNT_INC(g_bt_tx_inflight_timeout);

    app_tx_pump();
}

/* R04: the single BT TX service. It is deliberately the only context that
 * allocates a net_buf for output reports and submits them, and it runs below
 * bt/audio/USB priority, so an exhausted controller pool can never block the
 * 1 ms mic feed or the BT state machine. */
static void app_tx_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)xSemaphoreTake(app_tx_wake,
                             pdMS_TO_TICKS(APP_TX_SERVICE_TICK_MS));
        app_tx_service();
    }
}

static struct bt_br_discovery_result discovery_results[BT_MAX_DISCOVERED];

/* ---------- helpers ---------- */

static const bt_addr_t *conn_get_br_addr(const struct bt_conn *conn)
{
    struct bt_conn_info info;
    if (bt_conn_get_info(conn, &info) == 0)
        return info.br.dst;
    return NULL;
}

static const char *state_name(enum bt_hid_host_state s) {
    switch (s) {
    case BT_HID_STATE_IDLE:          return "IDLE";
    case BT_HID_STATE_SCANNING:      return "SCANNING";
    case BT_HID_STATE_CONNECTING:    return "CONNECTING";
    case BT_HID_STATE_SDP_QUERY:     return "SDP_QUERY";
    case BT_HID_STATE_L2CAP_CONTROL: return "L2CAP_CTRL";
    case BT_HID_STATE_L2CAP_INTERRUPT: return "L2CAP_INTR";
    case BT_HID_STATE_HANDSHAKE:     return "HANDSHAKE";
    case BT_HID_STATE_CONNECTED:     return "CONNECTED";
    case BT_HID_STATE_DISCONNECTING: return "DISCONNECTING";
    default:                         return "?";
    }
}

static void set_state(enum bt_hid_host_state new_state)
{
    /* Bluetooth callback context: enqueue an event, never format/print. */
    log_event_put(LOG_EVT_BT_FSM, (uint16_t)hid_ctx.state, (uint16_t)new_state);
    hid_ctx.state = new_state;

    if (new_state == BT_HID_STATE_CONNECTED && hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote) {
            bt_addr_copy(&hid_ctx.target_addr, remote);
            hid_ctx.has_target = true;
        }
    }

    if (hid_ctx.state_cb)
        hid_ctx.state_cb(new_state);
}

/* ---------- L2CAP callbacks ---------- */

static struct net_buf *l2cap_alloc_buf(struct bt_l2cap_chan *chan)
{
    return net_buf_alloc(&hid_rx_pool, K_NO_WAIT);
}

static void handshake_send_next(void)
{
    if (hid_ctx.state != BT_HID_STATE_HANDSHAKE)
        return;

    if (hid_ctx.handshake_next < hid_ctx.handshake_count) {
        uint8_t rid = hid_ctx.handshake_ids[hid_ctx.handshake_next];
        if (bt_hid_host_get_feature(rid) == 0) {
            hid_ctx.handshake_next++;
            hid_ctx.handshake_send_failed = false;
        } else {
            hid_ctx.handshake_send_failed = true;
            g_bt_handshake_err++;   /* BT RX context: counter only */
        }
    } else if (hid_ctx.state == BT_HID_STATE_HANDSHAKE) {
        set_state(BT_HID_STATE_CONNECTED);   /* emits a FSM event */
    }
}

static int l2cap_ctrl_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 1)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    switch (trans) {
    case HID_TRANS_HANDSHAKE:
        g_bt_ctrl_pkts++;   /* RX context: counters only */
        handshake_send_next();
        break;

    case HID_TRANS_DATA:
        if (param == HID_REPORT_FEATURE) {
            if (len > 2) {
                uint8_t rid = data[1];
                g_bt_feature_pkts++;
                log_event_put(LOG_EVT_HID_FEATURE, rid, (uint32_t)(len - 2));

                /* Cache payload + trailing CRC-32 (report id stripped).
                 * The USB GET handler strips the 4 CRC bytes itself. */
                feature_cache_store(rid, data + 2, len - 2);
            }
            handshake_send_next();
        }
        break;

    default:
        break;
    }

    return 0;
}

static int l2cap_intr_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
    uint8_t *data = buf->data;
    uint16_t len  = buf->len;

    if (len < 2)
        return 0;

    uint8_t header = data[0];
    uint8_t trans  = (header >> 4) & 0x0F;
    uint8_t param  = header & 0x0F;

    if (trans == HID_TRANS_DATA && param == HID_REPORT_INPUT) {
        g_bt_intr_pkts++;   /* RX context: counters only, no printf */
        if (hid_ctx.input_cb) {
            hid_ctx.input_cb(data + 1, len - 1);
        }
    } else {
        g_bt_intr_bad++;
    }

    return 0;
}

static void l2cap_ctrl_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_ctrl_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = true;

    /* Only create outgoing interrupt channel if WE initiated the connection
     * (via inquiry → connect). For controller-initiated reconnects, the
     * controller will open the interrupt channel via the L2CAP server. */
    if (hid_ctx.outgoing) {
        set_state(BT_HID_STATE_L2CAP_INTERRUPT);

        int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                        &hid_ctx.intr_chan.chan,
                                        HID_PSM_INTERRUPT);
        if (err) {
            LOG_ERR("[HID] Failed to connect interrupt channel: %d\n", err);
            bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
    }
}

static void l2cap_intr_connected(struct bt_l2cap_chan *chan)
{
    LOG_DBG("[HID-CB] l2cap_intr_connected ENTER chan=%p state=%d\n",
           chan, hid_ctx.state);
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = true;
    /* New interrupt-channel instance: any frame submitted with the previous
     * CID/MTU must not be committed against this one. */
    intr_gen_bump();


    /* Fire primer callback immediately — equivalent to DS4Dongle's
     * L2CAP_EVENT_CHANNEL_OPENED handler (gold lightbar + volume defaults).
     * This is the earliest possible moment to send an output report. */
    if (hid_ctx.primer_cb)
        hid_ctx.primer_cb();

    /* Successful L2CAP HID open removes this MAC from the blacklist,
     * treating it as an explicit user re-pair (PS+Share mode). */
    if (hid_ctx.conn) {
        const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
        if (remote && blacklist_contains(remote)) {
            blacklist_remove(remote);
            blacklist_dirty = true;
        }
    }

    /* Stop accepting further connections while connected (matches
     * DS5Dongle's gap_connectable_control(false) on HID open). */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);

    /* Shorten the Link Supervision Timeout so the BL618 controller
     * detects a dead link within ~5 s instead of the default ~20 s.
     * HCI Write_Link_Supervision_Timeout: OGF 0x03, OCF 0x0037.
     * Timeout unit = 0.625 ms; 5 s = 8000 slots = 0x1F40. */
    if (hid_ctx.conn) {
        uint16_t handle = hid_ctx.conn->handle;
        struct net_buf *buf = bt_hci_cmd_create(
            BT_OP(BT_OGF_BASEBAND, 0x0037), 4);
        if (buf) {
            net_buf_add_le16(buf, handle);
            net_buf_add_le16(buf, 0x1F40);
            int err = bt_hci_cmd_send_sync(
                BT_OP(BT_OGF_BASEBAND, 0x0037), buf, NULL);
            LOG_INF("[BT] Write Link Supervision Timeout: handle=0x%04x "
                   "timeout=5s err=%d\n", handle, err);
        }
    }

    set_state(BT_HID_STATE_HANDSHAKE);

    /* Request feature reports in DS4Dongle's order:
     * 0x05 (calibration — also switches the DS4 from minimal 0x01 input
     * reports to the full 0x11 reports), 0xA3 (firmware info), 0x06
     * (per-unit calibration / serial blob).
     * Paced: send one at a time, next sent in l2cap_ctrl_recv on response. */
    static const uint8_t handshake_ids[] = {
        DS4_FEATURE_REPORT_CALIBRATION_BT,
        DS4_FEATURE_REPORT_FIRMWARE,
        DS4_FEATURE_REPORT_PERUNIT_CAL
    };
    hid_ctx.handshake_ids   = handshake_ids;
    hid_ctx.handshake_count = sizeof(handshake_ids);
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_send_failed = false;

    handshake_send_next();
}

static void l2cap_ctrl_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Control channel disconnected (fallback_retry=%d conn=%p)\n",
           hid_ctx.fallback_retry, hid_ctx.conn);
    hid_ctx.ctrl_accepting = false;
    hid_ctx.ctrl_connected = false;
    if (!hid_ctx.intr_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static void l2cap_intr_disconnected(struct bt_l2cap_chan *chan)
{
    LOG_INF("[HID] Interrupt channel disconnected\n");
    hid_ctx.intr_accepting = false;
    hid_ctx.intr_connected = false;
    /* The ACL (and hid_ctx.conn, and app_tx_epoch) survives a channel
     * disconnect, so the generation is what invalidates a submit that
     * captured this channel's CID/MTU. */
    intr_gen_bump();
    if (!hid_ctx.ctrl_connected && hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

static struct bt_l2cap_chan_ops ctrl_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_ctrl_recv,
    .connected    = l2cap_ctrl_connected,
    .disconnected = l2cap_ctrl_disconnected,
};

static struct bt_l2cap_chan_ops intr_ops = {
    .alloc_buf    = l2cap_alloc_buf,
    .recv         = l2cap_intr_recv,
    .connected    = l2cap_intr_connected,
    .disconnected = l2cap_intr_disconnected,
};

/* ---------- SDP callback ---------- */

static uint8_t sdp_discover_cb(struct bt_conn *conn,
                                struct bt_sdp_client_result *result)
{
    if (conn != hid_ctx.conn)
        return BT_SDP_DISCOVER_UUID_STOP;

    if (!result) {
        LOG_INF("[SDP] Discovery complete, HID found=%d\n",
               hid_ctx.hid_service_found);

        if (!hid_ctx.hid_service_found) {
            LOG_INF("[SDP] No HID service, disconnecting\n");
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            return BT_SDP_DISCOVER_UUID_STOP;
        }

        set_state(BT_HID_STATE_L2CAP_CONTROL);

        memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
        memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

        hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
        hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
        hid_ctx.intr_chan.chan.ops = &intr_ops;
        hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

        int err = bt_l2cap_chan_connect(conn, &hid_ctx.ctrl_chan.chan,
                                           HID_PSM_CONTROL);
        if (err) {
            LOG_ERR("[SDP] Failed to connect control channel: %d\n", err);
            bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        }
        return BT_SDP_DISCOVER_UUID_STOP;
    }

    LOG_INF("[SDP] Found HID service record\n");
    hid_ctx.hid_service_found = true;
    return BT_SDP_DISCOVER_UUID_CONTINUE;
}

/* ---------- SSP Authentication ---------- */

static void auth_passkey_confirm(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP confirm passkey: %06u (auto-accepting)\n", passkey);
    bt_conn_auth_passkey_confirm(conn);
}

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
    LOG_INF("[BT] SSP display passkey: %06u\n", passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
    LOG_INF("[BT] SSP pairing cancelled\n");
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded)
{
    LOG_INF("[BT] Pairing complete, bonded=%d\n", bonded);

    if (bonded) {
        const bt_addr_le_t *dst = bt_conn_get_dst(conn);
        if (dst) {
            const bt_addr_t *addr = &dst->a;
            int idx = bonded_list_find_addr(addr);
            if (idx >= 0) {
                active_idx = (uint8_t)idx;
            } else {
                /* link_key_notify may not have fired yet, so
                 * bt_br_foreach_bond won't find the new key.
                 * Manually add this address to bonded_list. */
                if (bonded_count < MAX_BONDED_CONTROLLERS) {
                    bt_addr_copy(&bonded_list[bonded_count], addr);
                    active_idx = bonded_count;
                    bonded_count++;
                } else {
                    active_idx = 0;
                }
            }
            bonded_list_persist_idx();
            LOG_INF("[BT] Active controller updated: idx=%d, total=%d\n",
                   active_idx, bonded_count);
        }
    }
}

static void auth_pairing_failed(struct bt_conn *conn,
                                 enum bt_security_err reason)
{
    LOG_ERR("[BT] Pairing failed, reason=%d\n", reason);
}

static struct bt_conn_auth_cb auth_callbacks = {
    .passkey_display   = auth_passkey_display,
    .passkey_confirm   = auth_passkey_confirm,
    .cancel            = auth_cancel,
    .pairing_complete  = auth_pairing_complete,
    .pairing_failed    = auth_pairing_failed,
};

/* ---------- SDP ---------- */

static void start_sdp_discovery(struct bt_conn *conn)
{
    static struct bt_sdp_discover_params params;

    static struct bt_uuid_16 hid_uuid = BT_UUID_INIT_16(0x1124);

    params.uuid  = &hid_uuid.uuid;
    params.func  = sdp_discover_cb;
    params.pool  = &hid_tx_pool;

    set_state(BT_HID_STATE_SDP_QUERY);

    int err = bt_sdp_discover(conn, &params);
    if (err) {
        LOG_ERR("[SDP] Failed to start discovery: %d\n", err);
        bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---------- BR/EDR connection callbacks ---------- */

static void security_changed(struct bt_conn *conn, bt_security_t level,
                              enum bt_security_err err)
{
    LOG_DBG("[BT-CB] security_changed ENTER conn=%p level=%d err=%d state=%d\n",
           conn, level, err, hid_ctx.state);

    if (conn != hid_ctx.conn) {
        LOG_DBG("[BT-CB] security_changed: not our conn, ignoring\n");
        return;
    }

    if (err) {
        LOG_ERR("[BT] Security elevation failed: level=%d err=%d\n", level, err);
        /* Clear stale link key so next reconnect triggers fresh SSP pairing
         * (mirrors DS5Dongle's gap_drop_link_key on AUTHENTICATION_COMPLETE fail) */
        const bt_addr_t *remote = conn_get_br_addr(conn);
        if (remote) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(remote);
            LOG_WRN("[BT] Cleared stale link key for re-pairing\n");
        }
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    LOG_INF("[BT] Security level raised to %d (outgoing=%d state=%s)\n",
           level, hid_ctx.outgoing, state_name(hid_ctx.state));

    hid_ctx.security_done = true;
    hid_ctx.security_done_tick = xTaskGetTickCount();

    if (level >= BT_SECURITY_L2 && hid_ctx.outgoing
        && (hid_ctx.state == BT_HID_STATE_CONNECTING
            || hid_ctx.state == BT_HID_STATE_SDP_QUERY)) {
        /* Encryption complete for outgoing connection (new pairing).
         * Defer L2CAP channel creation to task context, matching
         * DS5Dongle's ENCRYPTION_CHANGE + new_pair path. */
        LOG_DBG("[BT] security_changed: outgoing — deferring L2CAP to task\n");
        hid_ctx.sdp_pending = true;
    } else if (level >= BT_SECURITY_L2 && !hid_ctx.outgoing) {
        /* Incoming reconnection: encryption OK.
         * Controller will create L2CAP channels via our servers. */
        LOG_INF("[BT] security OK (incoming), waiting for controller L2CAP\n");
    }
    LOG_DBG("[BT-CB] security_changed EXIT\n");
}

static void conn_connected(struct bt_conn *conn, uint8_t err)
{
    LOG_DBG("[BT-CB] conn_connected ENTER conn=%p err=%d state=%d\n",
           conn, err, hid_ctx.state);

    if (err) {
        LOG_ERR("[BT] Connection failed: 0x%02x\n", err);
        if (hid_ctx.conn != NULL && conn != hid_ctx.conn)
            return;
        hid_ctx.conn = NULL;
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        LOG_INF("[BT] Re-enabled page scan after failed connection\n");
        set_state(BT_HID_STATE_IDLE);
        return;
    }

    const bt_addr_t *remote = conn_get_br_addr(conn);
    LOG_INF("[BT] Connected\n");

    if (hid_ctx.conn != NULL && conn != hid_ctx.conn) {
        LOG_WRN("[BT] Already connected, rejecting second ACL\n");
        bt_conn_disconnect(conn, BT_HCI_ERR_CONN_LIMIT_EXCEEDED);
        return;
    }

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    if (!hid_ctx.outgoing && remote && blacklist_contains(remote)) {
        LOG_WRN("[BT] Rejecting blacklisted device "
               "(re-pair via PS+Share to unblock)\n");
        bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
        return;
    }

    if (!hid_ctx.conn) {
        LOG_DBG("[BT] conn_connected: taking ref on conn=%p\n", conn);
        hid_ctx.conn = bt_conn_ref(conn);
    }

    /* New ACL session boundary (R04): frames queued for a previous connection
     * must never be submitted on this one, and any submit still in flight is
     * invalidated (its eventual completion is then counted as stale). */
    app_tx_fifo_reset();

    hid_ctx.fallback_retry = false;
    hid_ctx.connect_pending = false;
    hid_ctx.connect_pending_tick = 0;
    LOG_INF("[BT] conn_connected: fallback_retry CLEARED (incoming conn)\n");

    set_state(BT_HID_STATE_CONNECTING);

    /* Log internal security state for diagnostics */
    LOG_DBG("[BT] conn handle=0x%04x sec_level=%d required_sec=%d encrypt=%d\n",
           conn->handle, conn->sec_level, conn->required_sec_level,
           conn->encrypt);

    /* Defer security establishment to task context.
     *
     * Root cause found in SDK: conn_new() does NOT clear the bt_conn struct,
     * so required_sec_level retains the stale value (L2) from the previous
     * connection.  bt_conn_set_security(L2) checks:
     *     if (sec_level >= sec || required_sec_level >= sec) return 0;
     * and returns immediately without doing anything.
     *
     * Fix: reset required_sec_level in poll_security() (task context) before
     * calling bt_conn_set_security.  We also can't call bt_hci_cmd_send_sync
     * from this callback (deadlock risk), so deferring is the safe approach. */
    hid_ctx.security_pending = true;
    hid_ctx.security_done = false;
    hid_ctx.acl_connect_tick = xTaskGetTickCount();

    LOG_DBG("[BT] %s connection — security deferred to task context\n",
           hid_ctx.outgoing ? "Outgoing" : "Incoming");
    LOG_DBG("[BT-CB] conn_connected EXIT\n");
}

static volatile int8_t cached_rssi = 1; /* 1 = unknown (positive → web shows "Unknown") */
static volatile bool switch_pending = false;

static void conn_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("[BT-CB] conn_disconnected: conn=%p hid_ctx.conn=%p reason=0x%02x "
           "state=%d fallback_retry=%d\n",
           conn, hid_ctx.conn, reason, hid_ctx.state, hid_ctx.fallback_retry);

    if (conn != hid_ctx.conn) {
        LOG_INF("[BT] Old ACL teardown complete (unowned conn, reason 0x%02x, "
               "state=%d)\n", reason, hid_ctx.state);
        /* If a fallback outgoing connect is waiting for ACL teardown, fire it now */
        if (hid_ctx.connect_pending && hid_ctx.connect_pending_tick != 0 &&
            hid_ctx.state == BT_HID_STATE_IDLE) {
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
            LOG_INF("[BT] ACL clear — fallback connect ready\n");
        }
        return;
    }

    LOG_INF("[BT] Disconnected reason=0x%02x\n", reason);

    /* Release the connection and clear the TX session under one lock, so a
     * submit that is between validation and send either completed with a
     * reference (and aborts on the epoch check) or sees NULL here. The
     * reference itself is dropped outside the lock. */
    {
        struct bt_conn *old = app_tx_teardown(conn);
        if (old)
            bt_conn_unref(old);
    }

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    first_output_logged = false;

    /* The TX session was already reset by app_tx_teardown() above. */

    feature_cache_clear();
    cached_rssi = 1;

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    LOG_INF("[BT] Re-enabled connectable+discoverable, ready for new connection\n");
    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: old ACL teardown complete, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
        return;
    }

    LOG_INF("[BT] conn_disconnected: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = xTaskGetTickCount();
        LOG_INF("[BT] Fallback: outgoing connect scheduled (ACL already down)\n");
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected        = conn_connected,
    .disconnected     = conn_disconnected,
    .security_changed = security_changed,
};

/* ---------- Inquiry / Discovery ---------- */

static void discovery_complete(struct bt_br_discovery_result *results,
                               size_t count)
{
    LOG_INF("[BT] Inquiry complete, found %zu device(s)\n", count);

    if (hid_ctx.conn) {
        LOG_INF("[BT] Inquiry ended but ACL already established, skipping\n");
        return;
    }

    for (size_t i = 0; i < count; i++) {
        uint32_t cod = results[i].cod[0]
                     | ((uint32_t)results[i].cod[1] << 8)
                     | ((uint32_t)results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;

        LOG_DBG("[BT] Device COD=0x%06x (major=%d minor=%d)\n",
               cod, major, minor);

        /* CoD Major Class 0x05 = Peripheral, Minor bit 1 = Gamepad */
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || hid_ctx.has_target) {
            if (hid_ctx.has_target &&
                bt_addr_cmp(&results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Gamepad found, deferring connect to task context\n");
            bt_addr_copy(&hid_ctx.pending_addr, &results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            /* Set CONNECTING instead of IDLE to prevent the auto-reconnect
             * logic from starting a new scan before poll_connect runs. */
            set_state(BT_HID_STATE_CONNECTING);
            return;
        }
    }

    LOG_INF("[BT] No gamepad found, will retry via bt_task\n");
    set_state(BT_HID_STATE_IDLE);
}

/* ---------- L2CAP server (for controller-initiated reconnection) ---------- */

static int l2cap_server_reject_blacklisted(struct bt_conn *conn)
{
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote && blacklist_contains(remote)) {
        LOG_WRN("[HID] Rejecting L2CAP from blacklisted device\n");
        return -EACCES;
    }
    return 0;
}

static void l2cap_server_ensure_conn(struct bt_conn *conn)
{
    if (!hid_ctx.conn) {
        hid_ctx.conn = bt_conn_ref(conn);
        set_state(BT_HID_STATE_CONNECTING);
    }
}

static int l2cap_ctrl_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    LOG_DBG("[HID] Incoming L2CAP control (PSM 0x%04x) "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_CONTROL, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection —
     * prevents a racing "rejected" second controller from stealing channels */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Control: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.ctrl_connected || hid_ctx.ctrl_accepting) {
        LOG_WRN("[HID] Control channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.ctrl_accepting = true;

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.ctrl_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Control server: accepted, chan=%p\n", *chan);
    return 0;
}

static int l2cap_intr_server_accept(struct bt_conn *conn,
                                     struct bt_l2cap_chan **chan)
{
    LOG_DBG("[HID] Incoming L2CAP interrupt (PSM 0x%04x) "
           "state=%s conn=%p our_conn=%p\n",
           HID_PSM_INTERRUPT, state_name(hid_ctx.state),
           conn, hid_ctx.conn);

    int err = l2cap_server_reject_blacklisted(conn);
    if (err) return err;

    /* Reject L2CAP from a different ACL than the primary connection */
    if (hid_ctx.conn && conn != hid_ctx.conn) {
        LOG_WRN("[HID] Interrupt: rejecting L2CAP from non-primary conn %p "
               "(primary=%p)\n", conn, hid_ctx.conn);
        return -EACCES;
    }

    if (hid_ctx.intr_connected || hid_ctx.intr_accepting) {
        LOG_WRN("[HID] Interrupt channel already connected/accepting\n");
        return -ENOMEM;
    }
    hid_ctx.intr_accepting = true;

    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
    *chan = &hid_ctx.intr_chan.chan;

    l2cap_server_ensure_conn(conn);
    LOG_INF("[HID] Interrupt server: accepted, chan=%p\n", *chan);
    return 0;
}

static struct bt_l2cap_server l2cap_hid_ctrl_server = {
    .psm    = HID_PSM_CONTROL,
    .accept = l2cap_ctrl_server_accept,
};

static struct bt_l2cap_server l2cap_hid_intr_server = {
    .psm    = HID_PSM_INTERRUPT,
    .accept = l2cap_intr_server_accept,
};

/* ---------- SDP Service Record for HID Host ---------- */

static struct bt_sdp_attribute hid_host_attrs[] = {
    BT_SDP_NEW_SERVICE,
    /* Service Class ID List: HID (0x1124) */
    {
        BT_SDP_ATTR_SVCLASS_ID_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
          ),
        }
    },
    /* Protocol Descriptor List: L2CAP(PSM=0x0011) + HIDP */
    {
        BT_SDP_ATTR_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_CONTROL) },
                ),
              },
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                ),
              },
          ),
        }
    },
    /* Additional Protocol Descriptor List: L2CAP(PSM=0x0013) + HIDP */
    {
        BT_SDP_ATTR_ADD_PROTO_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 15),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 13),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0100) },
                          { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(HID_PSM_INTERRUPT) },
                      ),
                    },
                    { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 3),
                      BT_SDP_DATA_ELEM_LIST(
                          { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x0011) },
                      ),
                    },
                ),
              },
          ),
        }
    },
    /* Profile Descriptor List: HID v1.11 */
    {
        BT_SDP_ATTR_PROFILE_DESC_LIST,
        { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 8),
          BT_SDP_DATA_ELEM_LIST(
              { BT_SDP_TYPE_SIZE_VAR(BT_SDP_SEQ8, 6),
                BT_SDP_DATA_ELEM_LIST(
                    { BT_SDP_TYPE_SIZE(BT_SDP_UUID16), BT_SDP_ARRAY_16(0x1124) },
                    { BT_SDP_TYPE_SIZE(BT_SDP_UINT16), BT_SDP_ARRAY_16(0x0111) },
                ),
              },
          ),
        }
    },
    BT_SDP_SERVICE_NAME("DS4Dongle HID"),
};

static struct bt_sdp_record hid_host_rec = {
    .attrs = hid_host_attrs,
    .attr_count = ARRAY_SIZE(hid_host_attrs),
};

/* ---------- bt_enable ready synchronization ---------- */

static volatile bool bt_ready_flag;
static SemaphoreHandle_t bt_ready_sem;
static StaticSemaphore_t bt_ready_sem_buf;

static void bt_ready_cb(int err)
{
    LOG_INF("[BT] bt_ready_cb: err=%d (stack init complete)\n", err);
    bt_ready_flag = (err == 0);
    xSemaphoreGive(bt_ready_sem);
}

/* ---------- public API ---------- */

int bt_hid_host_init(const struct bt_hid_host_config *config)
{
    memset(&hid_ctx, 0, sizeof(hid_ctx));
    feature_cache_clear();
    hid_ctx.input_cb   = config->input_cb;
    hid_ctx.state_cb   = config->state_cb;
    hid_ctx.primer_cb  = config->primer_cb;
    hid_ctx.filter_vid = config->target_vid;
    hid_ctx.filter_pid = config->target_pid;
    hid_ctx.state      = BT_HID_STATE_IDLE;

    net_buf_init(&hid_tx_pool, 20, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);
    net_buf_init(&hid_rx_pool, 4, BT_L2CAP_BUF_SIZE(L2CAP_BR_MTU), NULL);

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&auth_callbacks);

    bt_ready_sem = xSemaphoreCreateBinaryStatic(&bt_ready_sem_buf);

    int err = bt_enable(bt_ready_cb);
    if (err) {
        LOG_ERR("[BT] bt_enable failed: %d\n", err);
        return err;
    }

    LOG_INF("[BT] bt_enable returned, waiting for stack init...\n");
    if (xSemaphoreTake(bt_ready_sem, pdMS_TO_TICKS(10000)) != pdTRUE) {
        LOG_ERR("[BT] ERROR: stack init timeout!\n");
        return -1;
    }
    if (!bt_ready_flag) {
        LOG_ERR("[BT] ERROR: stack init failed\n");
        return -1;
    }
    LOG_INF("[BT] Bluetooth fully initialized\n");

    /* R04: start the single BT TX service task. Producers (USB, audio/mic
     * feed, BT callbacks) only enqueue and wake it, so nothing in a
     * latency-critical path ever allocates a net_buf or waits for a controller
     * TX context. Priority is below bt/audio/USB: a stalled BT link can never
     * delay the audio path. */
    if (!app_tx_wake)
        app_tx_wake = xSemaphoreCreateBinary();
    if (!app_tx_wake) {
        LOG_ERR("[BT] TX service semaphore alloc failed — inline fallback\n");
    } else if (!app_tx_task_handle) {
        if (xTaskCreate(app_tx_task, "bttx", APP_TX_TASK_STACK_WORDS, NULL,
                        APP_TX_TASK_PRIORITY, &app_tx_task_handle) != pdPASS) {
            app_tx_task_handle = NULL;
            LOG_ERR("[BT] TX service task create failed — inline fallback\n");
        }
    }

    /* Generate BD_ADDR from chip eFuse ID (boards often ship without
     * a pre-programmed BT MAC, resulting in 00:00:00:00:00:00).
     * Must be called AFTER bt_enable (HCI transport must be up). */
    {
        bt_addr_le_t cur;
        bt_get_local_public_address(&cur);
        bool all_zero = true;
        for (int i = 0; i < 6; i++)
            if (cur.a.val[i]) { all_zero = false; break; }

        if (all_zero) {
            uint8_t chipid[8];
            bflb_efuse_get_chipid(chipid);
            bt_addr_t bd;
            bd.val[0] = chipid[0];
            bd.val[1] = chipid[1];
            bd.val[2] = chipid[2];
            bd.val[3] = chipid[3];
            bd.val[4] = chipid[4];
            bd.val[5] = chipid[5] | 0xC0;
            int r1 = bt_set_bd_addr(&bd);
            int r2 = bt_set_local_public_address(bd.val);
            LOG_INF("[BT] Set BD_ADDR from chipid: %02X:%02X:%02X:%02X:%02X:%02X "
                   "(hci=%d host=%d)\n",
                   bd.val[5], bd.val[4], bd.val[3],
                   bd.val[2], bd.val[1], bd.val[0], r1, r2);
        }

        bt_get_local_public_address(&cur);
        LOG_INF("[BT] BD_ADDR: %02X:%02X:%02X:%02X:%02X:%02X\n",
               cur.a.val[5], cur.a.val[4], cur.a.val[3],
               cur.a.val[2], cur.a.val[1], cur.a.val[0]);
    }

    blacklist_load();
    bonded_list_load();

    /* Register L2CAP servers so the controller can initiate reconnection
     * (matching DS5Dongle's l2cap_register_service for both PSMs). */
    err = bt_l2cap_br_server_register(&l2cap_hid_ctrl_server);
    if (err)
        LOG_ERR("[BT] Warning: register control server failed: %d\n", err);

    err = bt_l2cap_br_server_register(&l2cap_hid_intr_server);
    if (err)
        LOG_ERR("[BT] Warning: register interrupt server failed: %d\n", err);

    err = bt_sdp_register_service(&hid_host_rec);
    if (err)
        LOG_ERR("[BT] Warning: register SDP HID record failed: %d\n", err);
    else
        LOG_INF("[BT] SDP HID service record registered\n");

    /* Match Pico's aggressive page scan: interval=window=0x0012 (11.25ms)
     * = 100% duty cycle. Ensures near-instant response to controller
     * reconnection attempts, critical for low-battery reconnect success.
     * HCI Write_Page_Scan_Activity: OGF 0x03, OCF 0x001C. */
    {
        struct net_buf *ps_buf = bt_hci_cmd_create(
            BT_OP(BT_OGF_BASEBAND, 0x001c), 4);
        if (ps_buf) {
            net_buf_add_le16(ps_buf, 0x0012);  /* interval: 11.25ms */
            net_buf_add_le16(ps_buf, 0x0012);  /* window:   11.25ms */
            int ps_err = bt_hci_cmd_send_sync(
                BT_OP(BT_OGF_BASEBAND, 0x001c), ps_buf, NULL);
            LOG_INF("[BT] Write Page Scan Activity: interval=0x0012 "
                   "window=0x0012 err=%d\n", ps_err);
        }
    }

    /* Make device discoverable so the controller can find us if needed */
    err = bt_br_set_connectable(true);
    LOG_INF("[BT] set_connectable: %d\n", err);

    err = bt_br_set_discoverable(true);
    LOG_INF("[BT] set_discoverable: %d\n", err);

    /* Diagnostic: check if BR/EDR write_scan_enable succeeded.
     * If both return 0, the controller accepted BR/EDR commands,
     * confirming that BR/EDR HCI path is functional. */
    LOG_INF("[BT] BR/EDR init complete — phone should see \"%s\"\n",
           CONFIG_BT_DEVICE_NAME);

    return 0;
}

int bt_hid_host_scan_start(uint8_t duration_sec)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE || hid_ctx.conn)
        return -1;

    /* Don't scan if an outgoing connect retry is pending —
     * HCI can't do inquiry and create_connection simultaneously */
    if (hid_ctx.connect_pending) {
        LOG_INF("[BT] scan_start suppressed: outgoing connect pending\n");
        return -2;
    }

    set_state(BT_HID_STATE_SCANNING);

    memset(discovery_results, 0, sizeof(discovery_results));

    struct bt_br_discovery_param param = {
        .length     = duration_sec ? duration_sec : BT_INQUIRY_LEN,
        .limited    = false,
    };

    int err = bt_br_discovery_start(&param, discovery_results,
                                     BT_MAX_DISCOVERED,
                                     discovery_complete);
    if (err) {
        LOG_ERR("[BT] Discovery start failed: %d\n", err);
        set_state(BT_HID_STATE_IDLE);
        return err;
    }

    LOG_INF("[BT] Scanning for devices...\n");
    return 0;
}

void bt_hid_host_scan_stop(void)
{
    bt_br_discovery_stop();
    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_poll_scan_early(void)
{
    if (hid_ctx.state != BT_HID_STATE_SCANNING)
        return false;

    for (int i = 0; i < BT_MAX_DISCOVERED; i++) {
        static const bt_addr_t zero = {{0}};
        if (bt_addr_cmp(&discovery_results[i].addr, &zero) == 0)
            break;

        uint32_t cod = discovery_results[i].cod[0]
                     | ((uint32_t)discovery_results[i].cod[1] << 8)
                     | ((uint32_t)discovery_results[i].cod[2] << 16);
        uint8_t major = (cod >> 8) & 0x1F;
        uint8_t minor = (cod >> 2) & 0x3F;
        bool is_gamepad = (major == 0x05) && (minor & 0x02);

        if (is_gamepad || (hid_ctx.has_target &&
            bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) == 0)) {

            if (hid_ctx.has_target &&
                bt_addr_cmp(&discovery_results[i].addr, &hid_ctx.target_addr) != 0)
                continue;

            LOG_INF("[BT] Early termination: gamepad found during inquiry\n");
            bt_br_discovery_stop();
            bt_addr_copy(&hid_ctx.pending_addr, &discovery_results[i].addr);
            hid_ctx.connect_pending = true;
            hid_ctx.connect_pending_tick = 0;
            set_state(BT_HID_STATE_CONNECTING);
            return true;
        }
    }
    return false;
}

bool bt_hid_host_poll_connect(void)
{
    if (!hid_ctx.connect_pending)
        return false;

    /* If controller already reconnected (incoming), cancel outgoing */
    if (hid_ctx.conn != NULL && hid_ctx.state != BT_HID_STATE_IDLE) {
        LOG_INF("[BT] poll_connect: cancelled (controller already connected, "
               "state=%d)\n", hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* connect_pending_tick == UINT32_MAX means "waiting for ACL teardown" */
    if (hid_ctx.connect_pending_tick == (TickType_t)UINT32_MAX)
        return false;

    /* After ACL teardown event fires, wait before connecting */
    if (hid_ctx.connect_pending_tick != 0) {
        TickType_t elapsed = xTaskGetTickCount() - hid_ctx.connect_pending_tick;
        if (elapsed < pdMS_TO_TICKS(500))
            return false;
    }

    /* Cancel if another connect path is active (not from our deferred flow).
     * States IDLE, SCANNING, and CONNECTING are valid — early termination and
     * discovery_complete set CONNECTING before deferring to poll_connect. */
    if (hid_ctx.state != BT_HID_STATE_IDLE &&
        hid_ctx.state != BT_HID_STATE_SCANNING &&
        hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_INF("[BT] poll_connect: cancelled (state=%d, not idle)\n",
               hid_ctx.state);
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
        return false;
    }

    /* If scanning is in progress, cancel it first (HCI can't do both) */
    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_INF("[BT] poll_connect: cancelling active scan first\n");
        bt_br_discovery_stop();
        set_state(BT_HID_STATE_IDLE);
    }

    LOG_INF("[BT] Fallback: attempting outgoing connect (state=%d conn=%p)\n",
           hid_ctx.state, hid_ctx.conn);
    int rc = bt_hid_host_connect(hid_ctx.pending_addr.val);
    if (rc != 0) {
        static uint8_t retry_count = 0;
        retry_count++;
        if (retry_count >= 3) {
            LOG_WRN("[BT] Fallback: gave up after %d attempts\n", retry_count);
            retry_count = 0;
            hid_ctx.connect_pending = false;
            hid_ctx.connect_pending_tick = 0;
        } else {
            LOG_WRN("[BT] Fallback: attempt %d failed (%d), retry in 500ms\n",
                   retry_count, rc);
            hid_ctx.connect_pending_tick = xTaskGetTickCount();
        }
    } else {
        hid_ctx.connect_pending = false;
        hid_ctx.connect_pending_tick = 0;
    }
    return true;
}

bool bt_hid_host_poll_security(void)
{
    if (!hid_ctx.security_pending || !hid_ctx.conn)
        return false;

    hid_ctx.security_pending = false;

    struct bt_conn *conn = hid_ctx.conn;

    LOG_DBG("[BT] poll_security: conn=%p handle=0x%04x state=%s "
           "sec_level=%d required_sec=%d encrypt=%d outgoing=%d\n",
           conn, conn->handle, state_name(hid_ctx.state),
           conn->sec_level, conn->required_sec_level, conn->encrypt,
           hid_ctx.outgoing);

    /* Root cause fix: conn_new() doesn't clear bt_conn, so
     * required_sec_level retains the stale value from a previous
     * connection.  Reset it so bt_conn_set_security() actually
     * initiates authentication. */
    if (conn->required_sec_level >= BT_SECURITY_L2) {
        LOG_DBG("[BT] poll_security: resetting stale required_sec_level "
               "%d -> L1\n", conn->required_sec_level);
        conn->required_sec_level = BT_SECURITY_L1;
    }

    int err = bt_conn_set_security(conn, BT_SECURITY_L2);
    LOG_DBG("[BT] poll_security: bt_conn_set_security(L2) = %d "
           "(now sec=%d req=%d)\n",
           err, conn->sec_level, conn->required_sec_level);

    if (err == -EBUSY) {
        LOG_DBG("[BT] poll_security: pairing already in progress, waiting\n");
    } else if (err && err != -EALREADY) {
        LOG_DBG("[BT] poll_security: set_security failed (%d), "
               "trying HCI Auth_Requested directly\n", err);
        struct net_buf *buf = bt_hci_cmd_create(
            BT_HCI_OP_AUTH_REQUESTED, 2);
        if (buf) {
            net_buf_add_le16(buf, conn->handle);
            int hci_err = bt_hci_cmd_send_sync(
                BT_HCI_OP_AUTH_REQUESTED, buf, NULL);
            LOG_DBG("[BT] poll_security: HCI Auth_Requested = %d\n", hci_err);
        }
    }

    return true;
}

bool bt_hid_host_poll_sdp(void)
{
    if (!hid_ctx.sdp_pending || !hid_ctx.conn)
        return false;

    hid_ctx.sdp_pending = false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING) {
        LOG_DBG("[BT] poll_sdp: state already advanced (%s), skipping\n",
               state_name(hid_ctx.state));
        return false;
    }

    /* Skip SDP for bonded devices — directly create L2CAP channels.
     * This matches DS5Dongle behavior where reconnection never does SDP. */
    LOG_DBG("[BT] poll_sdp: creating outgoing L2CAP channels\n");

    set_state(BT_HID_STATE_L2CAP_CONTROL);

    memset(&hid_ctx.ctrl_chan, 0, sizeof(hid_ctx.ctrl_chan));
    memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));

    hid_ctx.ctrl_chan.chan.ops = &ctrl_ops;
    hid_ctx.ctrl_chan.rx.mtu  = L2CAP_BR_MTU;
    hid_ctx.intr_chan.chan.ops = &intr_ops;
    hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;

    int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                    &hid_ctx.ctrl_chan.chan,
                                    HID_PSM_CONTROL);
    if (err) {
        LOG_DBG("[BT] poll_sdp: L2CAP control connect failed: %d\n", err);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
    return true;
}

#define SECURITY_TIMEOUT_TICKS  pdMS_TO_TICKS(8000)

bool bt_hid_host_poll_security_watchdog(void)
{
    if (hid_ctx.security_done || !hid_ctx.conn)
        return false;

    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.acl_connect_tick;
    if (elapsed < SECURITY_TIMEOUT_TICKS)
        return false;

    struct bt_conn *conn = hid_ctx.conn;
    LOG_WRN("[BT] WATCHDOG: security not established after %lu ms! "
           "sec_level=%d required_sec=%d encrypt=%d\n",
           (unsigned long)(elapsed * portTICK_PERIOD_MS),
           conn->sec_level, conn->required_sec_level, conn->encrypt);

    if (conn->encrypt && conn->sec_level >= BT_SECURITY_L2) {
        LOG_WRN("[BT] WATCHDOG: encryption active, forcing L2CAP path\n");
        hid_ctx.security_done = true;
        if (hid_ctx.outgoing) {
            hid_ctx.sdp_pending = true;
        }
        return true;
    }

    /* Auth failed: required_sec_level was reset by reset_pairing() in the
     * SDK, meaning the controller rejected our link key.  Delete the stale
     * key so the next connection attempt triggers fresh SSP pairing. */
    const bt_addr_t *remote = conn_get_br_addr(conn);
    if (remote) {
        LOG_WRN("[BT] WATCHDOG: clearing stale link key for "
               "%02X:%02X:%02X:%02X:%02X:%02X\n",
               remote->val[5], remote->val[4], remote->val[3],
               remote->val[2], remote->val[1], remote->val[0]);
        extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
        bt_keys_link_key_clear_addr(remote);
    }

    LOG_WRN("[BT] WATCHDOG: disconnecting to retry with fresh pairing\n");
    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return false;
}

#define INCOMING_L2CAP_FALLBACK_MS 3000

bool bt_hid_host_poll_incoming_l2cap_fallback(void)
{
    if (hid_ctx.state != BT_HID_STATE_CONNECTING)
        return false;
    if (!hid_ctx.security_done || hid_ctx.outgoing || !hid_ctx.conn)
        return false;

    TickType_t elapsed = xTaskGetTickCount() - hid_ctx.security_done_tick;
    if (elapsed < pdMS_TO_TICKS(INCOMING_L2CAP_FALLBACK_MS))
        return false;

    /* Control L2CAP handshake stuck (accept fired but connected never came) */
    if (hid_ctx.ctrl_accepting && !hid_ctx.ctrl_connected) {
        LOG_INF("[BT] L2CAP fallback: Control handshake stuck, disconnecting "
               "(ctrl_accept=%d ctrl_conn=%d intr_conn=%d)\n",
               hid_ctx.ctrl_accepting, hid_ctx.ctrl_connected, hid_ctx.intr_connected);
        /* Explicitly disconnect the stuck L2CAP channel to cancel RTX timer
         * before tearing down the ACL — speeds up ACL teardown significantly */
        bt_l2cap_chan_disconnect(&hid_ctx.ctrl_chan.chan);
        hid_ctx.fallback_retry = true;
        LOG_INF("[BT] fallback_retry SET=1 (stuck path)\n");
        bt_hid_host_disconnect();
        return true;
    }

    if (hid_ctx.ctrl_connected) {
        /* Control is up but Interrupt isn't — open Interrupt only */
        if (!hid_ctx.intr_connected && !hid_ctx.intr_accepting) {
            LOG_INF("[BT] L2CAP fallback: Control connected, opening Interrupt\n");
            hid_ctx.outgoing = true;
            set_state(BT_HID_STATE_L2CAP_INTERRUPT);
            memset(&hid_ctx.intr_chan, 0, sizeof(hid_ctx.intr_chan));
            hid_ctx.intr_chan.chan.ops = &intr_ops;
            hid_ctx.intr_chan.rx.mtu  = L2CAP_BR_MTU;
            int err = bt_l2cap_chan_connect(hid_ctx.conn,
                                            &hid_ctx.intr_chan.chan,
                                            HID_PSM_INTERRUPT);
            if (err)
                LOG_ERR("[BT] Interrupt connect failed: %d\n", err);
            return true;
        }
        return false;
    }

    /* Neither Control connecting nor connected — controller didn't open L2CAP.
     * Controller's SDP query may have closed before receiving our response.
     * Set fallback_retry to actively page controller and open L2CAP from host side
     * (standard HID Host behavior, same as Pico2W DS5Dongle). */
    LOG_WRN("[BT] L2CAP fallback: no activity in %dms, retry outgoing\n",
            (int)(elapsed * portTICK_PERIOD_MS));
    hid_ctx.fallback_retry = true;
    bt_hid_host_disconnect();
    return true;
}

int bt_hid_host_connect(const uint8_t *addr)
{
    bt_addr_t bt_addr;
    memcpy(bt_addr.val, addr, 6);

    LOG_DBG("[BT] connect ENTER addr=%02X:%02X:%02X:%02X:%02X:%02X state=%d conn=%p\n",
           addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
           hid_ctx.state, hid_ctx.conn);

    if (hid_ctx.state == BT_HID_STATE_SCANNING) {
        LOG_DBG("[BT] connect [1] stopping scan...\n");
        int stop_err = bt_br_discovery_stop();
        LOG_DBG("[BT] connect [2] scan stop rc=%d\n", stop_err);
    }

    if (hid_ctx.conn) {
        LOG_DBG("[BT] connect: already have conn=%p, skip create\n", hid_ctx.conn);
        hid_ctx.outgoing = true;
        set_state(BT_HID_STATE_CONNECTING);
        return 0;
    }

    hid_ctx.outgoing = true;
    set_state(BT_HID_STATE_CONNECTING);
    LOG_DBG("[BT] connect [3] state=CONNECTING, outgoing=true\n");

    /* Clear link key only for devices NOT in our bonded list (fresh discovery).
     * For bonded devices (fallback retry / reconnection), keep the key. */
    {
        bool is_bonded = false;
        for (uint8_t i = 0; i < bonded_count; i++) {
            if (bt_addr_cmp(&bonded_list[i], &bt_addr) == 0) {
                is_bonded = true;
                break;
            }
        }
        if (!is_bonded) {
            extern void bt_keys_link_key_clear_addr(const bt_addr_t *addr);
            bt_keys_link_key_clear_addr(&bt_addr);
            LOG_INF("[BT] Cleared stale link key for outgoing connection "
                   "(fresh SSP)\n");
        }
    }

    LOG_DBG("[BT] connect [4] bt_conn_create_br...\n");
    /* Disable page scan before paging — BL616 HCI controller can't do both */
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    struct bt_conn *conn = bt_conn_create_br(&bt_addr,
                                              BT_BR_CONN_PARAM_DEFAULT);
    LOG_DBG("[BT] connect [5] returned %p\n", conn);
    if (!conn) {
        LOG_WRN("[BT] bt_conn_create_br failed — re-enabling page scan\n");
        hid_ctx.outgoing = false;
        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);
        set_state(BT_HID_STATE_IDLE);
        return -1;
    }

    /* Bouffalo SDK's bt_conn_create_br does NOT return a caller
     * reference (unlike standard Zephyr). SDK examples never call
     * bt_conn_unref after bt_conn_create_br. */
    LOG_DBG("[BT] connect EXIT OK\n");
    return 0;
}

void bt_hid_host_disconnect(void)
{
    LOG_INF("[BT] bt_hid_host_disconnect called (state=%d fallback_retry=%d)\n",
           hid_ctx.state, hid_ctx.fallback_retry);
    if (hid_ctx.conn) {
        set_state(BT_HID_STATE_DISCONNECTING);
        bt_conn_disconnect(hid_ctx.conn,
                           BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

void bt_hid_host_force_disconnect(void)
{
    LOG_WRN("[BT] Force disconnect — HID cleanup, conn=%p\n", hid_ctx.conn);

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    first_output_logged = false;

    feature_cache_clear();

    if (hid_ctx.conn) {
        LOG_INF("[BT] Sending HCI Disconnect for ACL cleanup\n");
        bt_conn_disconnect(hid_ctx.conn, 0x13);
    }

    /* Detach the connection and clear the TX queue atomically, then release
     * the reference: a submit that already ref'd it aborts on the epoch
     * check instead of sending through a released/NULL channel. */
    {
        struct bt_conn *old = app_tx_teardown(NULL);
        if (old)
            bt_conn_unref(old);
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    set_state(BT_HID_STATE_IDLE);

    LOG_INF("[BT] force_disconnect: fallback_retry=%d bonded_count=%d\n",
           hid_ctx.fallback_retry, bonded_count);
    if (hid_ctx.fallback_retry && bonded_count > 0) {
        hid_ctx.fallback_retry = false;
        memcpy(hid_ctx.pending_addr.val, bonded_list[active_idx].val, 6);
        hid_ctx.connect_pending = true;
        hid_ctx.connect_pending_tick = (TickType_t)UINT32_MAX;
        LOG_INF("[BT] Fallback: outgoing connect pending (waiting for ACL teardown)\n");
    }
}

bool bt_hid_host_has_pending_conn(void)
{
    return hid_ctx.conn != NULL;
}

void bt_hid_host_drop_stale_conn(void)
{
    if (!hid_ctx.conn)
        return;

    LOG_WRN("[BT] conn_disconnected never arrived — force cleanup\n");

    /* Detach + clear the TX session atomically, then drop the reference. */
    {
        struct bt_conn *old = app_tx_teardown(NULL);
        if (old)
            bt_conn_unref(old);
    }

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.connect_pending = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;

    feature_cache_clear();

    set_state(BT_HID_STATE_IDLE);

    if (switch_pending) {
        switch_pending = false;
        LOG_INF("[BT] Switch: stale conn cleared, waiting for "
               "controller [%d/%d] to reconnect\n", active_idx, bonded_count);
    }

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Re-enabled connectable+discoverable after stale cleanup\n");
}

int bt_hid_host_read_rssi(int8_t *rssi)
{
    if (!hid_ctx.conn || hid_ctx.state != BT_HID_STATE_CONNECTED)
        return -1;
    int8_t val = 0;
    int ret = bt_le_read_rssi(hid_ctx.conn->handle, &val);
    if (ret == 0) {
        cached_rssi = val;
        if (rssi) *rssi = val;
    } else {
        LOG_WRN("[BT] Read RSSI failed: %d\n", ret);
    }
    return ret;
}

int8_t bt_hid_host_get_cached_rssi(void)
{
    return cached_rssi;
}

int bt_hid_host_send_output(const uint8_t *data, uint16_t len)
{
    if (!hid_ctx.intr_connected || !hid_ctx.conn)
        return -1;
    if (!data || len == 0)
        return -1;

    /* Every output report (0x11 game + 0x17 audio) goes through the FIFO
     * with completion flow control — mirrors DS4Dongle's bt_write, which
     * sends all reports through send_fifo + can_send_now. A report larger
     * than the L2CAP SDU cannot be carried at all; it is counted and
     * dropped here instead of taking a producer-side send path. */
    if (len + 1 > APP_TX_DATA_SZ) {
        APP_TX_CNT_INC(g_bt_tx_direct_drop);
        return -1;
    }

    uint8_t slot, next_tail;
    bool dropped = false;

    /* Producer side: publish one slot + tail under the lock and return. The
     * TX service task does the sending; this path never allocates, never
     * submits, never waits and never touches app_tx_inflight. */
    APP_TX_LOCK();
    next_tail = (uint8_t)((app_tx_tail + 1) % APP_TX_DEPTH);

    /* FIFO full: drop the oldest frame to make room (same as DS5Dongle).
     * The in-flight frame may be the oldest; its payload has already been
     * copied into the net_buf, so its slot can be recycled — but then the
     * completion must not advance head again, hence inflight_slot is
     * invalidated here. Retiring also clears that slot's frame identity, so a
     * submit that captured it can never mistake the new occupant for it. */
    if (next_tail == app_tx_head) {
        uint8_t victim = app_tx_head;
        app_tx_head_advance_locked(victim);
        if (app_tx_inflight && app_tx_inflight_slot == victim)
            app_tx_inflight_slot = APP_TX_SLOT_NONE;
        app_tx_retry = 0;                 /* budget belongs to the new head */
        app_tx_retry_id = 0;
        dropped = true;
    }

    slot = app_tx_tail;
    app_tx_buf[slot][0] = HID_HEADER(HID_TRANS_DATA, HID_REPORT_OUTPUT);
    memcpy(app_tx_buf[slot] + 1, data, len);
    app_tx_len[slot] = (uint16_t)(len + 1);
    /* Stamp the frame identity before publishing the tail, so the service
     * task never sees a queued slot without one. */
    {
        uint32_t fid = ++app_tx_frame_next;
        if (fid == 0)                     /* 0 = "no frame" */
            fid = ++app_tx_frame_next;
        app_tx_frame_id[slot] = fid;
    }
    app_tx_tail = next_tail;
    APP_TX_UNLOCK();

    if (dropped)
        APP_TX_CNT_INC(g_bt_tx_drop_full);

    if (!first_output_logged) {
        first_output_logged = true;
        LOG_INF("[HID] First output report enqueued, len=%d\n", (int)len);
    }

    /* Wake the TX service. Never submit from here: the caller may be the
     * 1 ms mic feed task, and the controller TX pool can be empty. */
    app_tx_kick();

    return 0;
}

/* Control-channel send with one explicit ownership rule (R04).
 *
 * The control channel is BR/EDR, so this goes to the BR hand-off directly
 * instead of bt_l2cap_chan_send(). It uses the non-blocking variant
 * (bt_l2cap_br_chan_send_handoff()) for the same reason as the data path:
 * bt_l2cap_br_chan_send() may wait up to 20 ms for a TX context, which is not
 * acceptable for a handshake issued from BT RX context or from the control
 * retry tick.
 *
 * R04 control hand-off — the same protocol as app_tx_submit_head():
 *   1. The final channel/session validation (conn identity, ctrl_connected,
 *      the channel's own conn pointer, live TX CID != 0), the CID/MTU capture
 *      and the L2CAP hand-off all happen inside ONE APP_TX_LOCK() window.
 *   2. APP_TX_LOCK() is an interrupt mask: on this single-core port no ISR, no
 *      other task, no BT RX/state callback and no BT workqueue item can run
 *      between the last check and the enqueue. A teardown of the control
 *      channel or a new ACL therefore cannot land in that gap, and the CID
 *      that was validated is the CID that is queued.
 *   3. The hand-off is itself non-blocking:
 *        bt_l2cap_br_chan_send_handoff()
 *          -> bt_l2cap_send_cb_no_wait()          (header push only)
 *          -> bt_conn_send_cb_no_wait()           (conn.c)
 *          -> conn_tx_alloc_no_wait()             k_fifo_get(..., K_NO_WAIT)
 *          -> conn_tx_queue_put_no_wait()         xQueueSend(..., 0)
 *      The queue append uses a ZERO tick timeout, not net_buf_put()'s
 *      xQueueSend(..., portMAX_DELAY). Queue space is counted first and the
 *      appends use zero timeout, so the "probe succeeds, another producer
 *      fills the last slot" case returns -ENOBUFS instead of blocking. The
 *      remaining operations in the window cannot wait either: net_buf_unref()
 *      returns the buffer to a LIFO that cannot overflow (it holds at most the
 *      pool's buffer count) and k_sem_give() is non-blocking.
 *   4. The hand-off re-resolves the captured CID to this exact channel object
 *      on the connection (-ENOTCONN otherwise), so even a retired/reassigned
 *      CID could never receive the PDU.
 *
 * Interleavings this removes (both were reachable when the CID read, the
 * caller's pre-check and the enqueue were outside the lock):
 *   (a) validation succeeds -> BT RX tears down the control channel -> the
 *       resumed call queues the captured CID. Now the teardown cannot run
 *       inside the window, and the SDK's CID->channel lookup is part of the
 *       same window, so the PDU is either queued on the validated channel or
 *       rejected.
 *   (b) the queue-space probe succeeds -> another producer takes the last
 *       slot -> net_buf_put() enters xQueueSend(..., portMAX_DELAY) and blocks
 *       the caller (possibly BT RX). The append is now zero-timeout, so that
 *       race is classified as -ENOBUFS and returned to the caller.
 *
 * Ownership: the hand-off consumes the buffer on EVERY return except a NULL
 * buffer — it unrefs on its own validation rejections (-ENOTCONN once the ACL
 * or the CID is gone, -EMSGSIZE when the report cannot fit) and
 * bt_conn_send_cb_no_wait() consumes it on success, on -ENOBUFS (no TX queue
 * room / no TX context) and on failure alike. Hence, from the hand-off return
 * on, the buffer is already gone: never unref it again, and allocate a new one
 * when retrying. This closes both defects of the old shape — a double unref
 * when the inner send had already freed the buffer, and a false success from
 * reading buf->len after the hand-off (which made the handshake advance on a
 * dropped GET_REPORT).
 *
 * Return value — -ENOBUFS is "retry later", not a hard failure. It is reported
 * to callers as the distinct, retryable BT_HID_SEND_WOULD_BLOCK so a caller
 * that owns the request (e.g. a deferred SET/GET_REPORT) can tell it apart
 * from a rejected PDU and re-issue it instead of dropping it. The buffer is
 * already released by then; the retry builds a fresh PDU. The internal
 * handshake relies on the existing retry tick: handshake_send_next() leaves
 * handshake_send_failed set when this returns an error, and
 * bt_hid_host_handshake_tick() re-issues the GET_REPORT on the next tick.
 * There is no blocking anywhere in the chain.
 *
 * The pre-check on ctrl_connected/conn in the callers is only a fast path;
 * correctness does not depend on it, because the final check below runs under
 * the lock and the backend re-checks the ACL state and the CID identity. */
static int hid_ctrl_send(struct net_buf *buf)
{
    struct bt_conn *conn;
    uint16_t cid, mtu, len;
    int err;

    if (!buf)
        return -EINVAL;         /* nothing to consume, nothing to send */

    len = buf->len;

    /* Serialised hand-off: final validation + CID/MTU capture + enqueue in one
     * non-preemptible window. Everything that could wait (the net_buf and its
     * headroom) was done by the caller before this point, so the window only
     * contains bookkeeping and the zero-timeout enqueue. */
    APP_TX_LOCK();
    conn = hid_ctx.conn;
    cid  = hid_ctx.ctrl_chan.tx.cid;
    mtu  = hid_ctx.ctrl_chan.tx.mtu;
    if (mtu == 0)                   /* not negotiated: the configured MTU */
        mtu = L2CAP_BR_MTU;

    if (!conn || !hid_ctx.ctrl_connected ||
        hid_ctx.ctrl_chan.chan.conn != conn || cid == 0) {
        APP_TX_UNLOCK();
        /* No backend call was made: the buffer is still ours. */
        net_buf_unref(buf);
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        return -ENOTCONN;
    }

    if (len > mtu) {
        APP_TX_UNLOCK();
        /* Cannot ever fit this channel; the backend was not called. */
        net_buf_unref(buf);
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        return -EMSGSIZE;
    }

    /* From here on `buf` belongs to the stack, whatever the result. */
    err = bt_l2cap_br_chan_send_handoff(&hid_ctx.ctrl_chan.chan, cid,
                                        buf, NULL, NULL);
    APP_TX_UNLOCK();

    if (err < 0) {
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        if (err == -ENOBUFS) {
            /* Transient back-pressure: the PDU was not accepted (and its
             * buffer is already released). Report a distinct retryable
             * status so the caller keeps the request instead of dropping it;
             * the handshake tick re-issues internally. */
            APP_TX_CNT_INC(g_bt_ctrl_would_block);
            return BT_HID_SEND_WOULD_BLOCK;
        }
    }

    return err;
}

int bt_hid_host_set_feature(uint8_t report_id, const uint8_t *data,
                             uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        /* HID TX pool exhausted: nothing was allocated, so there is no buffer
         * to release and no PDU was built. This is the retryable class, the
         * same as the hand-off's -ENOBUFS — report it as WOULD_BLOCK so a
         * caller that owns the request keeps it instead of dropping it. */
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        APP_TX_CNT_INC(g_bt_ctrl_would_block);
        return BT_HID_SEND_WOULD_BLOCK;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    /* Buffer ownership is resolved inside hid_ctrl_send(): never unref here.
     * BT_HID_SEND_WOULD_BLOCK (-EAGAIN) is the retryable class: the request
     * was not accepted, the caller keeps it and retries later. */
    int err = hid_ctrl_send(buf);
    if (err < 0)
        return err;

    return 0;
}

int bt_hid_host_get_feature(uint8_t report_id)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn)
        return -1;

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        /* Pool exhaustion is temporary back-pressure, not a send failure:
         * no buffer exists to release and no PDU reached the stack. Report
         * the retryable status so the deferred request is kept. */
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        APP_TX_CNT_INC(g_bt_ctrl_would_block);
        return BT_HID_SEND_WOULD_BLOCK;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_GET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);

    /* Ownership resolved inside the helper (the buffer is consumed on every
     * return), so nothing is unref'd here. The handshake advances only on 0:
     * any error — including the retryable BT_HID_SEND_WOULD_BLOCK — leaves
     * handshake_send_failed set, and handshake_tick() re-issues the request
     * with a fresh buffer. Never a false success from a released buffer's
     * length. */
    int err = hid_ctrl_send(buf);
    if (err < 0)
        return err;

    LOG_DBG("[HID] GET_REPORT(Feature, 0x%02x) sent\n", report_id);
    return 0;
}

enum bt_hid_host_state bt_hid_host_get_state(void)
{
    return hid_ctx.state;
}

bool bt_hid_host_get_connected_addr(uint8_t *addr)
{
    if (!hid_ctx.conn)
        return false;

    const bt_addr_t *remote = conn_get_br_addr(hid_ctx.conn);
    if (!remote)
        return false;

    memcpy(addr, remote->val, 6);
    return true;
}

bool bt_hid_host_get_local_addr(uint8_t *addr)
{
    bt_addr_le_t cur;
    bt_get_local_public_address(&cur);
    memcpy(addr, cur.a.val, 6);
    return true;
}

/* ---------- bonding persistence ---------- */

int bt_hid_host_try_reconnect(void)
{
    if (hid_ctx.state != BT_HID_STATE_IDLE)
        return -1;

    if (bonded_count == 0) {
        LOG_INF("[BT] No bonded BR/EDR device found\n");
        return -1;
    }

    if (active_idx >= bonded_count)
        active_idx = 0;

    bt_addr_t *addr = &bonded_list[active_idx];
    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    /* DS5Dongle approach: don't actively page the controller.
     * Stay connectable and let the controller reconnect to us.
     * The controller will create ACL + L2CAP channels. */
    LOG_INF("[BT] Bonded device [%d/%d] %s — waiting for controller to reconnect "
           "(page scan enabled)\n", active_idx, bonded_count, addr_str);
    return 0;
}

int bt_hid_host_switch_next(void)
{
    if (bonded_count <= 1) {
        LOG_INF("[BT] switch_next: only %d bonded, nothing to switch\n",
               bonded_count);
        return -1;
    }

    active_idx = (active_idx + 1) % bonded_count;
    bonded_list_persist_idx();

    bt_addr_t *addr = &bonded_list[active_idx];
    bt_addr_copy(&hid_ctx.target_addr, addr);
    hid_ctx.has_target = true;

    char addr_str[BT_ADDR_STR_LEN];
    bt_addr_to_str(addr, addr_str, sizeof(addr_str));

    if (hid_ctx.conn) {
        /* Block further BT sends immediately */
        hid_ctx.ctrl_connected = false;
        hid_ctx.intr_connected = false;
        switch_pending = true;

        LOG_INF("[BT] Switching: disconnect current → wait ACL teardown → "
               "reconnect [%d/%d] %s\n", active_idx, bonded_count, addr_str);
        bt_conn_disconnect(hid_ctx.conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
        /* conn_disconnected callback will handle full cleanup and
         * enable connectable only after old ACL is fully torn down. */
    } else {
        if (hid_ctx.state == BT_HID_STATE_SCANNING)
            bt_br_discovery_stop();

        hid_ctx.ctrl_accepting = false;
        hid_ctx.intr_accepting = false;
        hid_ctx.outgoing = false;
        hid_ctx.security_pending = false;
        hid_ctx.security_done = false;
        hid_ctx.sdp_pending = false;
        hid_ctx.connect_pending = false;
        hid_ctx.handshake_next  = 0;
        hid_ctx.handshake_count = 0;
        hid_ctx.handshake_send_failed = false;

        app_tx_fifo_reset();
        feature_cache_clear();
        cached_rssi = 1;

        bt_br_set_connectable(true);
        bt_br_set_discoverable(true);

        LOG_INF("[BT] Switched to controller [%d/%d] %s — waiting for reconnect\n",
               active_idx, bonded_count, addr_str);
        set_state(BT_HID_STATE_IDLE);
    }

    return (int)active_idx;
}

bool bt_hid_host_is_switching(void)
{
    return switch_pending;
}

uint8_t bt_hid_host_get_bonded_count(void)
{
    return bonded_count;
}

uint8_t bt_hid_host_get_active_idx(void)
{
    return active_idx;
}

void bt_hid_host_clear_bonds(void)
{
    LOG_INF("[BT] Clearing all bonding info\n");

    if (hid_ctx.state == BT_HID_STATE_SCANNING)
        bt_br_discovery_stop();

    /* Disconnect the current controller if connected.
     *
     * Detach first through the same atomic teardown helper the disconnect /
     * force-disconnect / drop-stale paths use. bt_conn_disconnect() can reach
     * conn_disconnected() -> app_tx_teardown() and clear hid_ctx.conn before
     * this function resumes; the old code then did
     * bt_conn_unref(hid_ctx.conn) and dereferenced NULL. Here the reference is
     * taken out of hid_ctx under the TX lock, used for the disconnect request
     * and dropped exactly once, whatever the callback did in between. */
    {
        struct bt_conn *old = app_tx_teardown(NULL);
        if (old) {
            bt_conn_disconnect(old, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
            bt_conn_unref(old);
        }
    }

    /* Clear the blacklist too — we want the controller to reconnect freely
     * after a manual bond clear. The security watchdog handles stale keys. */
    blacklist_count = 0;

    hid_ctx.ctrl_connected = false;
    hid_ctx.intr_connected = false;
    hid_ctx.ctrl_accepting = false;
    hid_ctx.intr_accepting = false;
    hid_ctx.outgoing = false;
    hid_ctx.handshake_next  = 0;
    hid_ctx.handshake_count = 0;
    hid_ctx.handshake_send_failed = false;
    hid_ctx.security_pending = false;
    hid_ctx.security_done = false;
    hid_ctx.sdp_pending = false;
    hid_ctx.connect_pending = false;

    bt_keys_link_key_clear_addr(NULL);

    blacklist_persist();
    blacklist_dirty = false;
    LOG_INF("[BT] All link keys and blacklist cleared\n");

    bonded_count = 0;
    active_idx = 0;
    bonded_list_persist_idx();

    hid_ctx.has_target = false;
    memset(&hid_ctx.target_addr, 0, sizeof(hid_ctx.target_addr));

    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);

    feature_cache_clear();

    set_state(BT_HID_STATE_IDLE);
}

bool bt_hid_host_feature_snapshot(uint8_t report_id, uint8_t *dst,
                                  uint16_t cap, uint16_t *out_len)
{
    bool found = false;
    uint16_t n = 0;

    if (out_len)
        *out_len = 0;
    if (!dst)
        return false;

    /* One bounded critical section: length and payload come from the same
     * published version, so the caller can never see a mixed block or a
     * length that does not describe the bytes it received. */
    uintptr_t key = bflb_irq_save();
    for (int i = 0; i < FEATURE_CACHE_SLOTS; i++) {
        if (feature_cache[i].report_id == report_id &&
            feature_cache[i].len > 0) {
            n = feature_cache[i].len;
            if (n > cap)
                n = cap;          /* truncated, never past the caller's cap */
            memcpy(dst, feature_cache[i].data, n);
            found = true;
            break;
        }
    }
    bflb_irq_restore(key);

    if (found && out_len)
        *out_len = n;
    return found;
}

int bt_hid_host_set_feature_crc(uint8_t report_id, const uint8_t *data,
                                uint16_t len)
{
    if (!hid_ctx.ctrl_connected || !hid_ctx.conn) {
        LOG_WRN("[HID] SET_FEATURE_CRC(0x%02x): not connected (ctrl=%d conn=%p)\n",
               report_id, hid_ctx.ctrl_connected, hid_ctx.conn);
        return -1;
    }

    struct net_buf *buf = net_buf_alloc(&hid_tx_pool, K_NO_WAIT);
    if (!buf) {
        /* Pool exhaustion: retryable, no buffer was taken. Kept distinct from
         * the not-connected -1 above so a caller can retry this request. */
        APP_TX_CNT_INC(g_bt_ctrl_send_err);
        APP_TX_CNT_INC(g_bt_ctrl_would_block);
        LOG_WRN("[HID] SET_FEATURE_CRC(0x%02x): no TX buffer, retry later\n",
                report_id);
        return BT_HID_SEND_WOULD_BLOCK;
    }

    net_buf_reserve(buf, BT_L2CAP_CHAN_SEND_RESERVE);
    net_buf_add_u8(buf, HID_HEADER(HID_TRANS_SET_REPORT, HID_REPORT_FEATURE));
    net_buf_add_u8(buf, report_id);
    if (data && len > 0)
        net_buf_add_mem(buf, data, len);

    /* Overwrite last 4 bytes of data with CRC (matching DS4Dongle's
     * fill_feature_report_checksum). The HID descriptor's Report Count
     * includes 4 bytes of CRC space that the USB host sends as zeros. */
    if (len >= 4) {
        uint8_t *crc_region = buf->data + 1;
        uint16_t crc_data_len = 1 + (len - 4);
        uint32_t crc = ds4_crc32(crc_region, crc_data_len,
                                 DS4_CRC_SEED_FEATURE);
        crc_region[crc_data_len]     = (crc >> 0) & 0xFF;
        crc_region[crc_data_len + 1] = (crc >> 8) & 0xFF;
        crc_region[crc_data_len + 2] = (crc >> 16) & 0xFF;
        crc_region[crc_data_len + 3] = (crc >> 24) & 0xFF;
    }

    /* Ownership is resolved inside the helper: the buffer is consumed whether
     * the send succeeded, failed after the hand-off, or was rejected before
     * it. Never unref it here. */
    int err = hid_ctrl_send(buf);
    if (err == BT_HID_SEND_WOULD_BLOCK) {
        /* Retryable back-pressure, not a failure: record it at warning
         * severity. g_bt_ctrl_would_block was already bumped inside
         * hid_ctrl_send() before it mapped -ENOBUFS, so no counter is
         * touched here. */
        LOG_WRN("[HID] SET_FEATURE_CRC(0x%02x): would block, retry later\n",
                report_id);
        return err;
    }

    if (err < 0) {
        LOG_ERR("[HID] SET_FEATURE_CRC(0x%02x): l2cap send failed err=%d\n",
               report_id, err);
        return err;
    }

    LOG_INF("[HID] SET_REPORT(Feature, 0x%02x) sent with CRC, len=%d\n",
           report_id, (int)(len + 2));
    return 0;
}

void bt_hid_host_persist_if_dirty(void)
{
    /* Task-context housekeeping tick (bt_task main loop, ~every 24 ms while
     * connected). Besides bond persistence it nudges the BT TX service: a
     * submit that failed because the pool was empty or L2CAP rejected it must
     * be retried even if no further report is produced, so the queue can
     * never depend on an unrelated packet to recover. The service task owns
     * the actual drain, so this call never blocks. */
    app_tx_kick();

    if (blacklist_dirty) {
        blacklist_dirty = false;
        blacklist_persist();
    }
}

void bt_hid_host_handshake_tick(void)
{
    if (hid_ctx.state == BT_HID_STATE_HANDSHAKE &&
        hid_ctx.handshake_send_failed) {
        handshake_send_next();
    }
}

void bt_hid_host_radio_idle(void)
{
    bt_br_set_connectable(false);
    bt_br_set_discoverable(false);
    LOG_INF("[BT] Radio idle — page/inquiry scan disabled\n");
}

void bt_hid_host_radio_wake(void)
{
    bt_br_set_connectable(true);
    bt_br_set_discoverable(true);
    LOG_INF("[BT] Radio wake — page/inquiry scan re-enabled\n");
}
