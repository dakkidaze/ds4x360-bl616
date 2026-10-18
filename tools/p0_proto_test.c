/*
 * P0 audio session protocol - host cross-timing tests (spec section 7).
 *
 * No SDK, no device: the whole point is that the shipping protocol core
 * (src/ds4_audio_proto.c) is compiled and executed here unchanged.  The
 * platform hooks are stubs that record what the core asked for, and the clock
 * is a test variable so a 5 ms mic timeout costs nothing.
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -std=c11 -DP0_HOST_TEST -Isrc \
 *       -o /tmp/opencode/p0_proto_test tools/p0_proto_test.c src/ds4_audio_proto.c
 *
 * Exit codes (same convention as tools/ep0_boundary_test.c):
 *   0 all pass, 1 any FAIL, 2 usage/build, 3 no FAIL but some SKIP.
 *
 * T13-T19 exercise the F0 synthetic-completion guard exactly as specified in
 * section 4.6 plus the M1 idle-host criterion and the M2 consecutive budget.
 * The guard lives in the SDK tree (Lane D); the include is
 * probed so this file still builds before that header is merged, in which
 * case those seven cases report SKIP.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "ds4_audio_proto.h"
/* M3 latch policy: header-only and SDK-free by design, so the recurrence
 * rule is compiled and exercised here exactly as the firmware uses it. */
#include "spk_recovery_policy.h"
/* usb_gamepad.h is SDK-free (stdint/stdbool only) on purpose; the applied-edge
 * identity main and the recovery share is declared there, so T27 drives the
 * exact type and the exact generation/timestamp contract. */
#include "usb_gamepad.h"

#if defined(__has_include)
#  if __has_include("../bouffalo_sdk/drivers/lhal/src/bflb_usb_v2_guard.h")
#    include "../bouffalo_sdk/drivers/lhal/src/bflb_usb_v2_guard.h"
#    define HAVE_F0_GUARD 1
#  elif __has_include("../../bouffalo_sdk/drivers/lhal/src/bflb_usb_v2_guard.h")
#    include "../../bouffalo_sdk/drivers/lhal/src/bflb_usb_v2_guard.h"
#    define HAVE_F0_GUARD 1
#  endif
#endif

/* ---- test platform ---------------------------------------------------- */

static p0_state_t      st;
static p0_platform_t   plat;

static uint32_t g_submit_calls, g_cancel_calls, g_close_calls, g_arm_calls;
static uint8_t  g_cancel_ep[32];
static uint32_t g_cancel_ep_n;
static uint32_t g_wake_calls;
static uint32_t g_notify_on, g_notify_off, g_notify_fault;
static int      g_submit_ret;
static int      g_cancel_ret;
static int      g_arm_ret;
static bool     g_settled;
static uint64_t g_now;
static uint32_t g_last_samples;
static int16_t  g_last_buf[P0_MIC_PKT_SAMPLES];

/* Lock contract: the port lock is a no-op here, but re-entry would mean a
 * public call was made from inside a platform callback.  Count it. */
static int g_lock_depth;
static int g_lock_violations;

uint32_t p0_host_lock_enter(void)
{
    if (g_lock_depth != 0)
        g_lock_violations++;
    return (uint32_t)++g_lock_depth;
}

void p0_host_lock_leave(uint32_t depth)
{
    (void)depth;
    if (g_lock_depth > 0)
        g_lock_depth--;
}

uint64_t p0_host_now_us(void)
{
    return g_now;
}

static void (*g_hook)(int point);

void p0_host_test_hook(int point)
{
    if (g_hook)
        g_hook(point);
}

static int cb_ep_cancel(uint8_t ep_addr)
{
    g_cancel_calls++;
    if (g_cancel_ep_n < sizeof(g_cancel_ep))
        g_cancel_ep[g_cancel_ep_n++] = ep_addr;
    /* Match the driver contract: a successful abort latches PENDING, so the
     * witness cannot already be satisfied — it arrives on a later clean G3
     * pass.  Tests that want it settled set g_settled explicitly. */
    if (g_cancel_ret == 0)
        g_settled = false;
    return g_cancel_ret;
}

static bool cb_ep_cancel_settled(uint8_t ep_addr)
{
    (void)ep_addr;
    return g_settled;
}

static int cb_ep_close(uint8_t ep_addr)
{
    (void)ep_addr;
    g_close_calls++;
    return 0;
}

static int cb_ep_arm_spk(void)
{
    g_arm_calls++;
    return g_arm_ret;
}

static int cb_submit_mic(const int16_t *buf, uint32_t samples)
{
    g_submit_calls++;
    g_last_samples = samples;
    for (uint32_t i = 0; i < samples && i < P0_MIC_PKT_SAMPLES; i++)
        g_last_buf[i] = buf[i];
    return g_submit_ret;
}

static void cb_sem_wake(void)
{
    g_wake_calls++;
}

static void cb_notify_mic(bool on)
{
    if (on)
        g_notify_on++;
    else
        g_notify_off++;
}

static void cb_notify_mic_fault(void)
{
    g_notify_fault++;
}

static void cb_notify_spk_fault(void)
{
}

/* ---- harness ---------------------------------------------------------- */

static int  g_fails;
static bool g_skip;
static char g_detail[192];

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            if (g_fails == 1)                                                \
                snprintf(g_detail, sizeof(g_detail),                         \
                         "line %d: %s", __LINE__, #cond);                    \
        }                                                                    \
    } while (0)

/* Only record the success summary when the test actually passed; on failure
 * the first CHECK line stays in g_detail. */
#define DETAIL(...)                                                          \
    do {                                                                     \
        if (g_fails == 0)                                                    \
            snprintf(g_detail, sizeof(g_detail), __VA_ARGS__);               \
    } while (0)

static void env_reset(void)
{
    memset(&st, 0, sizeof(st));
    memset(&plat, 0, sizeof(plat));

    plat.ep_cancel = cb_ep_cancel;
    plat.ep_cancel_settled = cb_ep_cancel_settled;
    plat.ep_close = cb_ep_close;
    plat.ep_arm_spk = cb_ep_arm_spk;
    plat.submit_mic = cb_submit_mic;
    plat.sem_wake = cb_sem_wake;
    plat.notify_mic = cb_notify_mic;
    plat.notify_mic_fault = cb_notify_mic_fault;
    plat.notify_spk_fault = cb_notify_spk_fault;
    plat.test_hook = NULL;

    g_submit_calls = g_cancel_calls = g_close_calls = g_arm_calls = 0;
    g_cancel_ep_n = 0;
    g_wake_calls = 0;
    g_notify_on = g_notify_off = g_notify_fault = 0;
    g_submit_ret = 0;
    g_cancel_ret = 0;
    g_arm_ret = 0;
    g_settled = true;
    g_now = 1000000ULL;
    g_last_samples = 0;
    g_hook = NULL;
    g_lock_depth = 0;

    p0_init(&st);
    p0_bus_link(&st, true);              /* a connected link is the normal case */
}

static void fill(int16_t *dst, uint32_t n, int16_t base)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = (int16_t)(base + (int16_t)i);
}

/* Open an interface and let the consumer apply it (no close preceded it, so
 * there is nothing to witness). */
static void establish(p0_intf_t intf)
{
    p0_host_alt(&st, intf, true);
    p0_service(&st, &plat);
}

/* The open path queues a ring reset that the next feed replays; burn it so a
 * test observes a stable ring.  (See the report: the spec sets the pending
 * flag and resets inline without clearing it.) */
static void prime_mic(void)
{
    p0_mic_feed(&st, &plat);
    p0_mic_complete(&st);
}

/* ---- T1 --------------------------------------------------------------- */

static void t01(void)
{
    env_reset();
    establish(P0_INTF_MIC);
    prime_mic();

    int16_t src[512];
    fill(src, 512, 100);
    uint32_t subs0 = g_submit_calls;
    p0_mic_publish(&st, src, 512, p0_session_gen(&st));
    p0_mic_feed(&st, &plat);
    CHECK(g_submit_calls == subs0 + 1);
    CHECK(st.mic_rd == 16);
    p0_mic_complete(&st);

    uint32_t wr = st.mic_wr, rd = st.mic_rd, subs = g_submit_calls;

    /* R1.1: the close must block the very next feed, in the same instant. */
    p0_host_alt(&st, P0_INTF_MIC, false);
    p0_mic_feed(&st, &plat);

    CHECK(g_submit_calls == subs);
    CHECK(st.mic_wr == wr);
    CHECK(st.mic_rd == rd);

    DETAIL(
             "no submit after close; wr=%u rd=%u", wr, rd);
}

/* ---- T2 --------------------------------------------------------------- */

static void t02(void)
{
    env_reset();
    establish(P0_INTF_MIC);
    prime_mic();

    int16_t src[512];
    fill(src, 512, 7);
    p0_mic_publish(&st, src, 512, p0_session_gen(&st));
    p0_mic_feed(&st, &plat);
    p0_mic_complete(&st);

    uint32_t subs = g_submit_calls;
    uint32_t closes = g_close_calls;

    p0_host_alt(&st, P0_INTF_MIC, false);
    for (int i = 0; i < 10; i++)
        p0_mic_feed(&st, &plat);

    CHECK(g_submit_calls == subs);
    CHECK(g_close_calls == closes);      /* endpoint work is deferred */
    CHECK(st.intf[P0_INTF_MIC].applied == 1);

    p0_service(&st, &plat);

    CHECK(g_close_calls == closes + 1);
    CHECK(st.intf[P0_INTF_MIC].applied == 0);
    CHECK(g_notify_off == 1);

    DETAIL(
             "10 feeds silent, close deferred to service");
}

/* ---- T3 --------------------------------------------------------------- */

static void t03(void)
{
    env_reset();
    establish(P0_INTF_MIC);              /* applied=1 before the flap */

    uint32_t gen0 = p0_session_gen(&st);
    uint32_t closes = g_close_calls;

    p0_host_alt(&st, P0_INTF_MIC, true);
    p0_host_alt(&st, P0_INTF_MIC, false);
    p0_host_alt(&st, P0_INTF_MIC, true);

    p0_service(&st, &plat);              /* flap: close the old endpoint */
    CHECK(g_close_calls == closes + 1);
    CHECK(st.intf[P0_INTF_MIC].applied == 0);
    CHECK(st.intf[P0_INTF_MIC].settle == 1);

    g_settled = true;
    p0_service(&st, &plat);              /* witnessed: open again */
    CHECK(st.intf[P0_INTF_MIC].applied == 1);
    CHECK(st.intf[P0_INTF_MIC].desired == 1);
    CHECK(p0_session_gen(&st) - gen0 == 2);

    uint32_t subs = g_submit_calls;
    p0_mic_feed(&st, &plat);             /* the mic "arm" is the first submit */
    CHECK(g_submit_calls == subs + 1);

    DETAIL(
             "close then re-arm; gen +%u", (unsigned)(p0_session_gen(&st) - gen0));
}

/* ---- T4 --------------------------------------------------------------- */

static void t04(void)
{
    env_reset();

    /* 64 host requests, last one is OPEN.  The old request ring was 8 deep. */
    for (uint32_t i = 0; i < 64; i++) {
        bool open = ((i & 1u) != 0);
        p0_host_alt(&st, P0_INTF_MIC, open);
        CHECK(st.intf[P0_INTF_MIC].desired == (open ? 1u : 0u));
        CHECK(st.intf[P0_INTF_MIC].desired_seq == i + 1);
    }
    CHECK(g_submit_calls == 0);
    CHECK(st.intf[P0_INTF_MIC].desired == 1);
    CHECK(st.intf[P0_INTF_MIC].applied == 0);

    /* A teardown always needs a fresh witness, so arming is one pass later:
     * the first service round cancels, the witnessed round rebuilds. */
    p0_service(&st, &plat);
    g_settled = true;
    p0_service(&st, &plat);
    CHECK(st.intf[P0_INTF_MIC].applied == 1);
    CHECK(st.intf[P0_INTF_MIC].desired == 1);
    CHECK(st.intf[P0_INTF_MIC].applied_seq == st.intf[P0_INTF_MIC].desired_seq);

    /* Idempotent: a second pass must not touch anything. */
    uint32_t acts = g_cancel_calls + g_close_calls + g_arm_calls + g_submit_calls;
    uint32_t seq = st.intf[P0_INTF_MIC].applied_seq;
    p0_service(&st, &plat);
    CHECK(g_cancel_calls + g_close_calls + g_arm_calls + g_submit_calls == acts);
    CHECK(st.intf[P0_INTF_MIC].applied_seq == seq);

    DETAIL( "64 flips, final desired/applied=1, idempotent");
}

/* ---- T5 --------------------------------------------------------------- */

static void t05(void)
{
    env_reset();
    p0_host_alt(&st, P0_INTF_SPK, true);

    uint32_t epoch = st.bus_epoch;
    p0_bus_reset(&st);

    CHECK(st.intf[P0_INTF_SPK].desired == 0);
    CHECK(st.intf[P0_INTF_SPK].applied == 0);
    CHECK(st.bus_epoch == epoch + 1);
    CHECK(st.intf[P0_INTF_SPK].ep_dirty == 1);

    uint32_t arms = g_arm_calls;
    p0_service(&st, &plat);

    CHECK(g_arm_calls == arms);
    CHECK(st.intf[P0_INTF_SPK].applied == 0);
    CHECK(st.intf[P0_INTF_SPK].ep_dirty == 0);
    /* Both interfaces were marked dirty by the reset, so both endpoints are
     * force-closed even though only the speaker ever had intent. */
    CHECK(g_cancel_calls == 2);
    CHECK(g_close_calls == 2);

    DETAIL(
             "reset cleared intent, epoch+1, no arm");
}

/* ---- T6 --------------------------------------------------------------- */

static void t06(void)
{
    env_reset();
    p0_bus_reset(&st);

    p0_host_alt(&st, P0_INTF_SPK, true);
    CHECK(st.intf[P0_INTF_SPK].desired_epoch == st.bus_epoch);

    g_settled = true;
    uint32_t arms = g_arm_calls;
    p0_service(&st, &plat);              /* RESET teardown + cancel */
    g_settled = true;
    p0_service(&st, &plat);              /* witnessed: arm */

    CHECK(g_arm_calls == arms + 1);
    CHECK(st.intf[P0_INTF_SPK].applied == 1);
    CHECK(st.intf[P0_INTF_SPK].desired == 1);
    CHECK(st.intf[P0_INTF_SPK].desired_epoch == st.bus_epoch);
    CHECK(st.st.epoch_drop == 0);

    DETAIL(
             "one ep_arm_spk, desired_epoch==bus_epoch");
}

/* ---- T7 --------------------------------------------------------------- */

static void t07(void)
{
    env_reset();

    int16_t src[64];
    fill(src, 64, 300);
    uint32_t old_gen = p0_session_gen(&st);

    p0_bus_reset(&st);
    uint32_t wr0 = st.mic_wr;

    /* Immediately after reset the producer is frozen: that check comes first
     * (section 4.5), so this is a frozen drop. */
    uint32_t frozen = st.st.mic_publish_frozen_drop;
    CHECK(p0_mic_publish(&st, src, 64, old_gen) == 0);
    CHECK(st.mic_wr == wr0);
    CHECK(st.st.mic_publish_frozen_drop == frozen + 64);

    /* After the consumer reopened the stream, frozen is clear and the stale
     * generation is what rejects the frame. */
    p0_host_alt(&st, P0_INTF_MIC, true);
    p0_service(&st, &plat);              /* teardown + cancel */
    g_settled = true;
    p0_service(&st, &plat);              /* witnessed: stream open, frozen clear */

    uint32_t gend = st.st.mic_publish_gen_drop;
    CHECK(p0_mic_publish(&st, src, 64, old_gen) == 0);
    CHECK(st.mic_wr == wr0);
    CHECK(st.st.mic_publish_gen_drop == gend + 64);

    DETAIL(
             "stale gen rejected, gen_drop+64, wr=%u", st.mic_wr);
}

/* ---- T8 --------------------------------------------------------------- */

static void t08(void)
{
    env_reset();
    /* (a) The round loop below is only evidence if the stream is actually
     * armed.  Without this setup the first generated seed resets the bus and
     * the reopen never gets its witness (g_settled stays false), so the
     * session stays frozen and *every* publish returns 0: the wr/data checks
     * then pass without ever exercising a successful publication.  Start from
     * an applied, witnessed stream instead. */
    establish(P0_INTF_MIC);
    prime_mic();

    int16_t src[64];
    uint32_t seed = 0x12345678u;
    uint32_t accepted = 0, rejected = 0, rej_barrier = 0, acc_wr = 0;
    uint32_t bad_wr = 0, bad_data = 0, partial = 0;

    for (uint32_t it = 0; it < 1000; it++) {
        seed = seed * 1103515245u + 12345u;

        if ((seed >> 20) & 1u) {
            p0_bus_reset(&st);

            /* (c) RESET invalidates the session in the same instant, so this
             * publish must be refused and wr must not move. */
            fill(src, 64, (int16_t)(it * 7));
            uint32_t pre = st.mic_wr;
            uint32_t r = p0_mic_publish(&st, src, 64, p0_session_gen(&st));
            if (st.mic_wr != (pre + r) % P0_MIC_RING_SAMPLES)
                bad_wr++;
            if (r != 0)
                bad_wr++;                 /* frozen: this must be 0 */
            else {
                rejected++;
                rej_barrier++;
            }

            /* (a) Rebuild the stream the way the adapter does on re-open:
             * cancel+close first, then the next clean G3 pass witnesses the
             * abort, then arm.  The host re-opens the alt setting after every
             * bus reset, so this always happens. */
            p0_host_alt(&st, P0_INTF_MIC, true);
            p0_service(&st, &plat);       /* cancel/close, settle pending */
            g_settled = true;             /* the witness: FIFO proven silent */
            p0_service(&st, &plat);       /* arm -> applied */
            continue;
        }

        fill(src, 64, (int16_t)(it * 7));
        uint32_t pre = st.mic_wr;
        uint32_t r = p0_mic_publish(&st, src, 64, p0_session_gen(&st));

        /* One transaction: wr moves by exactly the number of samples the
         * publish reports, never by a torn half.
         */
        if (st.mic_wr != (pre + r) % P0_MIC_RING_SAMPLES)
            bad_wr++;
        for (uint32_t k = 0; k < r; k++) {
            if (st.mic_ring[(pre + k) % P0_MIC_RING_SAMPLES] != src[k])
                bad_data++;
        }
        if (r != 0 && r != 64)
            partial++;

        if (r != 0) {
            accepted++;
            if (st.mic_wr != pre)
                acc_wr++;                 /* commit observed: wr advanced */
            /* Drain what was just published.  Otherwise the ring simply fills
             * and the rest of the 1000 rounds degenerates into "rejected
             * because full", which proves nothing about publication. */
            for (uint32_t d = 0; d < 64 && st.mic_rd != st.mic_wr; d++) {
                p0_mic_feed(&st, &plat);
                p0_mic_complete(&st);
            }
        } else {
            rejected++;
        }
    }

    /* (b) the test must have observed real, committed publications ... */
    CHECK(accepted > 0);
    CHECK(acc_wr == accepted);            /* every accepted publish moved wr */
    /* (c) ... and real refusals (at least one at the reset barrier). */
    CHECK(rejected > 0);
    CHECK(rej_barrier > 0);
    CHECK(bad_wr == 0);
    CHECK(bad_data == 0);
    CHECK(partial == 0);

    DETAIL(
             "1000 rounds, accepted=%u (wr moved %u) rejected=%u (barrier=%u) bad_wr=%u bad_data=%u partial=%u",
             accepted, acc_wr, rejected, rej_barrier, bad_wr, bad_data, partial);
}

/* ---- T9 --------------------------------------------------------------- */

static void t09(void)
{
    env_reset();
    establish(P0_INTF_MIC);
    prime_mic();

    int16_t src[512];
    fill(src, 512, 11);
    p0_mic_publish(&st, src, 512, p0_session_gen(&st));
    p0_mic_feed(&st, &plat);
    CHECK(st.mic_state == P0_MIC_INFLIGHT);
    CHECK(g_submit_calls == 2);

    /* Close invalidates the in-flight transaction on the spot (R1.1). */
    p0_host_alt(&st, P0_INTF_MIC, false);
    CHECK(st.mic_state == P0_MIC_IDLE);

    uint32_t unexp = st.st.mic_unexpected_cplt;
    p0_mic_complete(&st);
    CHECK(st.mic_state == P0_MIC_IDLE);
    CHECK(st.st.mic_unexpected_cplt == unexp + 1);

    /* Reopen: the level is unchanged (1) but the sequence advanced, so the
     * endpoint must be rebuilt and witnessed before feed may submit. */
    p0_host_alt(&st, P0_INTF_MIC, true);
    g_settled = false;
    p0_service(&st, &plat);
    CHECK(st.intf[P0_INTF_MIC].applied == 0);
    CHECK(st.intf[P0_INTF_MIC].settle == 1);

    uint32_t subs = g_submit_calls;
    p0_mic_feed(&st, &plat);
    CHECK(g_submit_calls == subs);

    g_settled = true;
    p0_service(&st, &plat);
    CHECK(st.intf[P0_INTF_MIC].applied == 1);

    p0_mic_feed(&st, &plat);
    CHECK(g_submit_calls == subs + 1);

    DETAIL(
             "late complete ignored, submit blocked until settle");
}

/* ---- T10 -------------------------------------------------------------- */

static void t10(void)
{
    env_reset();
    establish(P0_INTF_MIC);
    prime_mic();

    int16_t src[512];
    fill(src, 512, 21);
    p0_mic_publish(&st, src, 512, p0_session_gen(&st));
    p0_mic_feed(&st, &plat);
    CHECK(st.mic_state == P0_MIC_INFLIGHT);
    CHECK(st.mic_rd == 16);

    uint32_t rd0 = st.mic_rd;
    uint32_t subs = g_submit_calls;

    g_now += 6000;                       /* past P0_MIC_BUSY_TIMEOUT_US */
    p0_mic_feed(&st, &plat);

    CHECK(st.mic_state == P0_MIC_CANCELING);
    CHECK(g_submit_calls == subs);
    CHECK(st.mic_rd == rd0);
    CHECK(st.mic_frozen == 1);

    g_settled = false;
    uint32_t cancels = g_cancel_calls;
    p0_service(&st, &plat);

    CHECK(g_cancel_calls > cancels);
    CHECK(st.mic_state == P0_MIC_CANCELING);   /* not settled: no arm */
    CHECK(g_submit_calls == subs);

    g_settled = true;
    p0_service(&st, &plat);

    CHECK(st.mic_state == P0_MIC_IDLE);
    CHECK(st.mic_wr == 0);
    CHECK(st.mic_rd == 0);
    CHECK(st.mic_frozen == 0);
    CHECK(st.st.mic_cancel_ok == 1);

    p0_mic_feed(&st, &plat);
    CHECK(g_submit_calls == subs + 1);

    DETAIL(
             "timeout->CANCELING, cancel witnessed, ring cleared");
}

/* ---- T11 -------------------------------------------------------------- */

static int   g_hook_point;
static bool  g_hook_fired;

static void hook_inject_reset(int point)
{
    if (point == g_hook_point && !g_hook_fired) {
        g_hook_fired = true;
        st.pcm_reset_pending = 1;        /* what the ISR/service side would do */
    }
}

static void t11(void)
{
    env_reset();
    p0_host_alt(&st, P0_INTF_SPK, true);
    p0_service(&st, &plat);

    int16_t blk[P0_PCM_FRAMES * 2];
    memset(blk, 0, sizeof(blk));

    /* The open path queued a reset; the first take applies it and fails. */
    CHECK(p0_pcm_take(&st, blk) == false);
    CHECK(st.pcm_epoch == 1);
    CHECK(st.pcm_pending == 0);

    int16_t src[P0_PCM_FRAMES * 2];
    fill(src, P0_PCM_FRAMES * 2, 1000);

    CHECK(p0_pcm_write(&st, src, P0_PCM_FRAMES * 2, &plat) == true);
    CHECK(st.pcm_pending == 1);
    CHECK(g_wake_calls == 1);

    /* Late reset at the commit point (after memcpy, before commit). */
    g_hook_point = 2;
    g_hook_fired = false;
    g_hook = hook_inject_reset;
    CHECK(p0_pcm_take(&st, blk) == false);
    g_hook = NULL;

    CHECK(st.pcm_pending == 0);
    CHECK(st.st.pcm_late_take_reject == 1);
    CHECK(st.pcm_epoch == 2);
    CHECK(p0_pcm_take(&st, blk) == false);   /* still empty */

    /* Reset at the reserve point (before memcpy) must behave the same. */
    CHECK(p0_pcm_write(&st, src, P0_PCM_FRAMES * 2, &plat) == true);
    g_hook_point = 1;
    g_hook_fired = false;
    g_hook = hook_inject_reset;
    CHECK(p0_pcm_take(&st, blk) == false);
    g_hook = NULL;
    CHECK(st.pcm_pending == 0);
    CHECK(st.st.pcm_late_take_reject == 2);

    /* A fresh block is accepted and belongs to the current epoch. */
    CHECK(p0_pcm_write(&st, src, P0_PCM_FRAMES * 2, &plat) == true);
    uint32_t epoch = st.pcm_epoch;
    CHECK(p0_pcm_take(&st, blk) == true);
    CHECK(st.pcm_epoch == epoch);
    CHECK(memcmp(blk, src, sizeof(src)) == 0);
    CHECK(st.pcm_pending == 0);

    DETAIL(
             "late reset rejected at both points, epoch now %u",
             (unsigned)st.pcm_epoch);
}

/* ---- T12 -------------------------------------------------------------- */

static void t12(void)
{
    env_reset();

    int16_t src[1024];
    fill(src, 1024, 55);

    /* A full ring holds N-1 samples (one slot is the read/write separator). */
    CHECK(p0_mic_publish(&st, src, 1023, p0_session_gen(&st)) == 1023);
    CHECK(st.mic_wr == 1023);

    int16_t snapshot[16];
    memcpy(snapshot, st.mic_ring, sizeof(snapshot));

    int16_t extra[16];
    fill(extra, 16, (int16_t)0x5555);
    uint32_t drops = st.st.mic_ring_full_drop;

    CHECK(p0_mic_publish(&st, extra, 16, p0_session_gen(&st)) == 0);
    CHECK(st.mic_wr == 1023);
    CHECK(st.st.mic_ring_full_drop == drops + 16);
    CHECK(memcmp(snapshot, st.mic_ring, sizeof(snapshot)) == 0);

    DETAIL(
             "full ring rejected, unread samples intact");
}

/* ---- T13-T16: F0 synthetic-completion guard (§4.6) -------------------- */

#ifdef HAVE_F0_GUARD

#define F0_EP 0x01u

static bflb_f0_action_t guard_pass(bflb_f0_guard_t *g, bool armed, bool start,
                                   bool cmplt, uint16_t fifo_bytes,
                                   uint64_t now_us)
{
    bflb_f0_input_t in;
    memset(&in, 0, sizeof(in));
    in.guard_ep = F0_EP;
    in.fifo_ep = F0_EP;
    in.armed = armed;
    in.start = start;
    in.cmplt = cmplt;
    in.fifo_bytes = fifo_bytes;
    in.now_us = now_us;
    return bflb_f0_guard_step(g, &in);
}

/* 200 passes with no completion but bytes queued: the exact pass count that
 * arms criterion 5, plus the M1 occupancy condition (a stall needs data).
 *
 * M4: the guard now measures its evidence window from the first queued-data
 * pass, so the helper has to model elapsed time like real G3 passes do
 * (300 us apart, ~60 ms across the run) instead of freezing the clock. The
 * assertions of T13-T19 are unchanged. */
static bflb_f0_action_t guard_run_deadlock(bflb_f0_guard_t *g, uint64_t now_us,
                                           uint16_t fifo_bytes)
{
    bflb_f0_action_t act = BFLB_F0_ACT_NONE;
    for (uint32_t i = 0; i < BFLB_F0_ARM_MIN_PASSES; i++)
        act = guard_pass(g, true, true, false, fifo_bytes,
                         now_us + (uint64_t)i * 300u);
    return act;
}

/* T13: START=1, 200 passes but only 10 ms -> not a deadlock, misfire. */
static void t13(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 1000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    bflb_f0_action_t act = BFLB_F0_ACT_FAULT;
    for (uint32_t i = 0; i < BFLB_F0_ARM_MIN_PASSES; i++)
        act = guard_pass(&g, true, true, false, 64, t0 + 10000);

    CHECK(act == BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.misfire == 1);
    CHECK(g.cnt.safetynet == 0);

    DETAIL("10 ms/200 passes -> NONE, misfire=%u", g.cnt.misfire);
}

/* T14: a transfer that completed once is not a deadlock, however long it has
 * been armed.  Same 60 ms/200 passes as T15, different progress. */
static void t14(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 5000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    /* One completion since this arm. */
    CHECK(guard_pass(&g, true, true, true, 0, t0) == BFLB_F0_ACT_NONE);
    CHECK(g.progress == 1);

    bflb_f0_action_t act = BFLB_F0_ACT_FAULT;
    for (uint32_t i = 0; i < BFLB_F0_ARM_MIN_PASSES + 10; i++)
        act = guard_pass(&g, true, true, false, 64, t0 + 60000);

    CHECK(act == BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.misfire >= 1);
    CHECK(g.cnt.safetynet == 0);
    CHECK(g.cnt.synth == 0);

    DETAIL("progress=1 -> no abort after 60 ms, misfire=%u", g.cnt.misfire);
}

/* T15: real deadlock -> ABORT, then the first clean pass synthesises one
 * completion. */
static void t15(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 9000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    bflb_f0_action_t act = guard_run_deadlock(&g, t0 + 60000, 256);
    CHECK(act == BFLB_F0_ACT_ABORT);
    CHECK(g.state == BFLB_F0_RECOVERING);
    CHECK(g.cnt.safetynet == 1);
    CHECK(g.cnt.idle == 0);

    act = guard_pass(&g, false, false, false, 0, t0 + 61000);
    CHECK(act == BFLB_F0_ACT_SYNTH);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.synth == 1);
    CHECK(g.abort_cnt == 0);      /* synthetic recovery clears the budget */

    DETAIL("ABORT then SYNTH, synth=%u", g.cnt.synth);
}

/* T16: a completion bit during RECOVERING must be dropped (no synthesis);
 * three aborts that never see a real completion are terminal FAULT and report
 * it once. A synthetic completion is not forward progress, so it does not
 * clear the escalation streak (M2). */
static void t16(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    uint64_t now = 20000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, now);

    /* Abort 1, then a late completion in the recovery pass. */
    CHECK(guard_run_deadlock(&g, now + 60000, 64) == BFLB_F0_ACT_ABORT);
    CHECK(guard_pass(&g, false, false, true, 0, now + 61000) == BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_RECOVERING);
    CHECK(g.cnt.stale_drop == 1);
    CHECK(g.cnt.synth == 0);

    /* Clean pass: one synthesis, back to ARMED. */
    now = g.arm_us;
    CHECK(guard_pass(&g, false, false, false, 0, now + 61000) == BFLB_F0_ACT_SYNTH);
    CHECK(g.cnt.synth == 1);

    /* Abort 2. */
    now = g.arm_us;
    CHECK(guard_run_deadlock(&g, now + 60000, 64) == BFLB_F0_ACT_ABORT);

    /* Clean pass -> synth, then abort 3 is the terminal fault. */
    now = g.arm_us;
    CHECK(guard_pass(&g, false, false, false, 0, now + 61000) == BFLB_F0_ACT_SYNTH);

    now = g.arm_us;
    bflb_f0_action_t act = guard_run_deadlock(&g, now + 60000, 64);
    CHECK(act == BFLB_F0_ACT_FAULT);
    CHECK(g.state == BFLB_F0_FAULT);
    CHECK(g.cnt.fault == 1);
    CHECK(g.abort_streak == BFLB_F0_MAX_ABORTS);
    CHECK(g.abort_cnt == 1);      /* consecutive budget was reset by each SYNTH */

    /* Terminal: no further action, and the fault is reported only once. */
    act = guard_pass(&g, true, true, false, 64, now + 200000);
    CHECK(act == BFLB_F0_ACT_NONE);
    CHECK(g.cnt.fault == 1);
    CHECK(g.cnt.synth == 2);

    DETAIL("stale completion dropped, FAULT once after 3 unrecovered aborts");
}

/* T17 (M1): host pause. Armed, no completion, FIFO empty for a long time and
 * many passes: this is an idle host, not a deadlock. NONE, no abort, no
 * fault, and the idle counter makes it observable. */
static void t17(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 30000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    bflb_f0_action_t act = BFLB_F0_ACT_FAULT;
    for (uint32_t i = 0; i < 2000; i++)
        act = guard_pass(&g, true, true, false, 0, t0 + 5000000ULL);

    CHECK(act == BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.safetynet == 0);
    CHECK(g.cnt.fault == 0);
    CHECK(g.abort_cnt == 0);
    CHECK(g.abort_streak == 0);
    CHECK(g.cnt.idle == 2000 - (BFLB_F0_ARM_MIN_PASSES - 1));

    /* START clear + empty FIFO is still idle: the VDMA has nothing to drain,
     * so a stopped engine is not evidence of a stall. */
    CHECK(guard_pass(&g, true, false, false, 0, t0 + 5000001ULL) ==
          BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.idle == 2000 - (BFLB_F0_ARM_MIN_PASSES - 1) + 1);
    CHECK(g.cnt.fault == 0);

    DETAIL("idle host: 2000 passes/5 s, FIFO empty -> no abort, idle=%u",
           g.cnt.idle);
}

/* T18 (M1/M2): the same shape with bytes queued is a real stall: ABORT, then
 * the clean recovery pass synthesises exactly one completion. */
static void t18(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 35000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    /* Bytes in the FIFO, VDMA running, 60 ms, 200 passes, no completion. */
    bflb_f0_action_t act = guard_run_deadlock(&g, t0 + 60000, 192);
    CHECK(act == BFLB_F0_ACT_ABORT);
    CHECK(g.state == BFLB_F0_RECOVERING);
    CHECK(g.cnt.safetynet == 1);
    CHECK(g.cnt.idle == 0);
    CHECK(g.cnt.misfire == 0);

    act = guard_pass(&g, false, false, false, 0, t0 + 61000);
    CHECK(act == BFLB_F0_ACT_SYNTH);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.synth == 1);
    CHECK(g.cnt.fault == 0);

    DETAIL("FIFO data + no completion -> ABORT then SYNTH");
}

/* T19 (M2): the abort budget is consecutive. Two aborts, then a real
 * completion, then another abort must not fault. */
static void t19(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 40000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    /* Abort 1 + synthetic recovery. */
    CHECK(guard_run_deadlock(&g, t0 + 60000, 64) == BFLB_F0_ACT_ABORT);
    uint64_t now = g.arm_us;
    CHECK(guard_pass(&g, false, false, false, 0, now + 1000) == BFLB_F0_ACT_SYNTH);

    /* Abort 2 + synthetic recovery. */
    now = g.arm_us;
    CHECK(guard_run_deadlock(&g, now + 60000, 64) == BFLB_F0_ACT_ABORT);
    CHECK(g.abort_cnt == 1);
    CHECK(g.abort_streak == 2);
    now = g.arm_us;
    CHECK(guard_pass(&g, false, false, false, 0, now + 1000) == BFLB_F0_ACT_SYNTH);

    /* A real completion of the guarded FIFO is forward progress: both the
     * consecutive budget and the escalation streak restart. */
    CHECK(guard_pass(&g, true, true, true, 0, now + 2000) == BFLB_F0_ACT_NONE);
    CHECK(g.progress == 1);
    CHECK(g.abort_cnt == 0);
    CHECK(g.abort_streak == 0);

    /* The class layer re-arms after the completion (progress is per-arm). */
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, now + 3000);

    /* One more deadlock: it must be an abort, not a fault. */
    bflb_f0_action_t act = guard_run_deadlock(&g, now + 63000, 64);
    CHECK(act == BFLB_F0_ACT_ABORT);
    CHECK(g.state == BFLB_F0_RECOVERING);
    CHECK(g.cnt.fault == 0);
    CHECK(g.abort_streak == 1);

    DETAIL("completion reset the consecutive budget: abort 1, no FAULT");
}

/* T20 (M4): idle time must not pre-charge the stall window. Two thousand
 * empty passes over 5 s, then one packet arrives with bytes queued and no
 * completion: that pass may not abort, and the evidence window starts at the
 * first queued-data pass, not at arm(). */
static void t20(void)
{
    bflb_f0_guard_t g;
    bflb_f0_guard_init(&g, F0_EP);

    const uint64_t t0 = 60000;
    bflb_f0_guard_arm(&g, F0_EP, F0_EP, t0);

    /* Long idle host: armed, FIFO empty, no completion for 5 s. */
    for (uint32_t i = 0; i < 2000; i++)
        CHECK(guard_pass(&g, true, true, false, 0, t0 + 5000000ULL) ==
              BFLB_F0_ACT_NONE);
    CHECK(g.cnt.idle == 2000 - (BFLB_F0_ARM_MIN_PASSES - 1));
    CHECK(g.data_passes == 0);      /* no window is open while idle */

    /* One packet arrives after the pause. */
    const uint64_t t_data = t0 + 5000001ULL;
    CHECK(guard_pass(&g, true, true, false, 192, t_data) == BFLB_F0_ACT_NONE);
    CHECK(g.state == BFLB_F0_ARMED);
    CHECK(g.cnt.safetynet == 0);
    CHECK(g.cnt.fault == 0);
    CHECK(g.abort_cnt == 0);
    CHECK(g.data_passes == 1);      /* window opens here, not at arm() */
    CHECK(g.data_since_us == t_data);

    /* Idle again -> the window closes; the next packet starts it over. */
    for (uint32_t i = 0; i < 500; i++)
        CHECK(guard_pass(&g, true, true, false, 0, t_data + 1000 + i) ==
              BFLB_F0_ACT_NONE);
    CHECK(g.data_passes == 0);
    CHECK(guard_pass(&g, true, true, false, 192, t_data + 2000) ==
          BFLB_F0_ACT_NONE);
    CHECK(g.data_passes == 1);
    CHECK(g.cnt.safetynet == 0);

    /* Only sustained queued-data/no-progress evidence aborts. 200 data passes
     * over 60 ms, starting from the window the single packet above opened. */
    bflb_f0_action_t act = BFLB_F0_ACT_NONE;
    for (uint32_t i = 0; i < BFLB_F0_ARM_MIN_PASSES; i++) {
        act = guard_pass(&g, true, true, false, 192, t_data + 3000 + i * 300u);
        if (act != BFLB_F0_ACT_NONE)
            break;
    }
    CHECK(act == BFLB_F0_ACT_ABORT);
    CHECK(g.state == BFLB_F0_RECOVERING);
    CHECK(g.cnt.safetynet == 1);
    CHECK(g.cnt.fault == 0);

    DETAIL("5 s idle + 1 packet -> no abort; window restarts at data");
}

#else /* !HAVE_F0_GUARD */

static void t13(void)
{
    g_skip = true;
    DETAIL("needs bflb_usb_v2_guard.h (Lane D deliverable, not present)");
}
static void t14(void) { t13(); }
static void t15(void) { t13(); }
static void t16(void) { t13(); }
static void t17(void) { t13(); }
static void t18(void) { t13(); }
static void t19(void) { t13(); }
static void t20(void) { t13(); }

#endif /* HAVE_F0_GUARD */

/* ---- T21-T22: fail-closed recovery latch and recurrence policy -------- */

/* T21 (M3b): a fail-closed witness deadline must survive the automatic fault
 * recovery. settle_failed=1 -> p0_spk_fault_clear() refuses, the F0 latch
 * stays, and service does not arm. Only a host RESET (authorized
 * reconstruction) may clear it. */
static void t21(void)
{
    env_reset();

    /* Armed speaker stream. */
    establish(P0_INTF_SPK);
    CHECK(st.intf[P0_INTF_SPK].applied == 1);
    const uint32_t arms = g_arm_calls;

    /* Close with an unsettled witness, reopen, and let the settle deadline
     * expire: settle_failed is now the fail-closed latch. */
    g_settled = false;
    p0_host_alt(&st, P0_INTF_SPK, false);
    p0_service(&st, &plat);
    CHECK(st.intf[P0_INTF_SPK].settle == 1);
    CHECK(st.intf[P0_INTF_SPK].settle_failed == 0);

    p0_host_alt(&st, P0_INTF_SPK, true);
    for (int i = 0; i < P0_SETTLE_MAX_PASSES; i++)
        p0_service(&st, &plat);

    CHECK(st.intf[P0_INTF_SPK].settle_failed == 1);
    CHECK(st.intf[P0_INTF_SPK].applied == 0);
    CHECK(g_arm_calls == arms);

    /* The driver reports the terminal F0 fault; the app then tries its
     * controlled recovery. It must be refused, not laundered. */
    p0_spk_fault(&st);
    CHECK(st.spk_fault_latch == 1);
    CHECK(p0_spk_rebuild_allowed(&st) == false);

    CHECK(p0_spk_fault_clear(&st) == false);
    CHECK(st.spk_fault_latch == 1);            /* still latched     */
    CHECK(st.intf[P0_INTF_SPK].settle_failed == 1);
    CHECK(st.intf[P0_INTF_SPK].ep_dirty == 0); /* no barrier either */
    CHECK(st.st.spk_fault_clear_refused == 1);

    /* Whatever service does next, the endpoint stays unarmed. */
    for (int i = 0; i < 8; i++)
        p0_service(&st, &plat);
    CHECK(st.intf[P0_INTF_SPK].applied == 0);
    CHECK(st.intf[P0_INTF_SPK].settle_failed == 1);
    CHECK(st.spk_fault_latch == 1);
    CHECK(g_arm_calls == arms);

    /* A host RESET is an authorized reconstruction: it clears the latch. */
    p0_bus_reset(&st);
    CHECK(st.intf[P0_INTF_SPK].settle_failed == 0);
    g_settled = true;
    p0_host_alt(&st, P0_INTF_SPK, true);
    p0_service(&st, &plat);       /* dirty: cancel/close, needs a witness */
    g_settled = true;             /* the fresh cancel is witnessed now    */
    p0_service(&st, &plat);       /* arm                                  */
    CHECK(st.intf[P0_INTF_SPK].applied == 1);
    CHECK(g_arm_calls == arms + 1);

    DETAIL("settle_failed: recovery refused, reset is the only way out");
}

/* T22 (M3c/finding 4): the recurrence latch is a function of time only.
 * Three faults inside 10 s latch no matter how many host opens / reconnects
 * happen in between; a fault outside the window does not. */
static void t22(void)
{
    spk_fault_history_t h;

    /* Three faults inside the window: latch. */
    spk_fault_history_init(&h);
    spk_fault_history_record(&h, 1000);
    spk_fault_history_record(&h, 3000);
    CHECK(spk_fault_history_should_latch(&h, 5000) == false);
    spk_fault_history_record(&h, 5000);
    CHECK(spk_fault_history_should_latch(&h, 5000) == true);

    /* The very same three faults, each separated by a host reopen: there is
     * no API that an open could call to erase them, so the decision is
     * identical. (This is the regression: the old code cleared the history on
     * every host SET_INTERFACE / reset / self-open.) */
    spk_fault_history_t h2;
    spk_fault_history_init(&h2);
    spk_fault_history_record(&h2, 1000);
    spk_fault_history_record(&h2, 3000);
    spk_fault_history_record(&h2, 5000);
    CHECK(spk_fault_history_should_latch(&h2, 5001) == true);
    CHECK(spk_fault_history_count(&h2, 5001) == 3);

    /* Three faults spread over more than 10 s: no latch (time decay). */
    spk_fault_history_t h3;
    spk_fault_history_init(&h3);
    spk_fault_history_record(&h3, 1000);
    spk_fault_history_record(&h3, 6000);
    spk_fault_history_record(&h3, 11000);
    CHECK(spk_fault_history_should_latch(&h3, 11000) == false);
    CHECK(spk_fault_history_count(&h3, 11000) == 2);

    /* The window slides: the same ring does latch once three faults fall
     * inside it again, and goes empty when all entries aged out. */
    spk_fault_history_record(&h3, 20000);
    spk_fault_history_record(&h3, 21000);
    spk_fault_history_record(&h3, 22000);
    CHECK(spk_fault_history_should_latch(&h3, 22000) == true);
    CHECK(spk_fault_history_count(&h3, 40000) == 0);

    /* Timestamp 0 is a real timestamp, not an "empty slot" sentinel: the
     * validity bit carries emptiness. A fault at 0 ms is inside the window at
     * now = 0 and at now = 5000, and it is not folded to 1 (folding made it
     * look 1 ms in the future at now = 0). */
    spk_fault_history_t h4;
    spk_fault_history_init(&h4);
    spk_fault_history_record(&h4, 0);
    CHECK(spk_fault_history_count(&h4, 0) == 1);
    CHECK(spk_fault_history_count(&h4, 1) == 1);
    CHECK(spk_fault_history_count(&h4, 5000) == 1);
    CHECK(spk_fault_history_count(&h4, 10000) == 0);
    /* An empty slot is not a fault at 0. */
    spk_fault_history_init(&h4);
    CHECK(spk_fault_history_count(&h4, 0) == 0);

    DETAIL("history decays by time only; opens cannot erase it");
}

/* T23 (finding 3): the fault is recorded when the event is claimed, not when
 * the bounce survives. The old consumer claimed the ISR timestamp only after
 * deciding that the scheduled bounce was obsolete, so "fault -> host reopen
 * -> task pass" recorded zero faults and three genuine stalls could never
 * latch. The ledger below is the exact firmware path: claim records, dropping
 * a bounce only clears the pending schedule. */
static void t23(void)
{
    spk_fault_history_t h;
    spk_fault_ledger_t l;

    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);

    /* No event: nothing recorded; the claim pass counts as skipped. This is
     * carried by ev.valid, not by a 0 timestamp. */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event_none()) == false);
    CHECK(l.recorded == 0);
    CHECK(l.skipped == 1);
    CHECK(spk_fault_history_count(&h, 1000) == 0);

    /* A real event at t=0 is claimed and recorded (the old sentinel collapsed
     * it into "no event"). */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(0)) == true);
    CHECK(spk_fault_ledger_pending(&l).valid == true);
    CHECK(spk_fault_ledger_pending(&l).at_ms == 0);
    CHECK(l.recorded == 1);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(spk_fault_ledger_pending(&l).valid == false);
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);

    /* Three faults, each followed by a host reopen that makes the bounce
     * pointless before the task can act on it. All three still count. */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(1000)) == true);
    CHECK(spk_fault_ledger_pending(&l).valid == true);
    CHECK(spk_fault_ledger_pending(&l).at_ms == 1000);
    spk_fault_ledger_drop_bounce(&l);            /* host reopened the stream */
    CHECK(spk_fault_ledger_pending(&l).valid == false);

    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(2000)) == true);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(3000)) == true);
    spk_fault_ledger_drop_bounce(&l);

    CHECK(l.recorded == 3);
    CHECK(l.skipped == 0);            /* fresh ledger after the t=0 case */
    CHECK(spk_fault_history_count(&h, 3000) == 3);
    CHECK(spk_fault_history_should_latch(&h, 3000) == true);

    /* A bounce that is actually consumed is recorded exactly once. */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(4000)) == true);
    CHECK(spk_fault_ledger_take(&l).at_ms == 4000);
    CHECK(spk_fault_ledger_take(&l).valid == false);  /* not consumable twice */
    CHECK(l.recorded == 4);
    /* The ring keeps the newest SPK_RECOV_MAX_IN_WINDOW timestamps. */
    CHECK(spk_fault_history_count(&h, 4000) == SPK_RECOV_MAX_IN_WINDOW);

    /* Time decay still applies to the recorded timestamps. */
    CHECK(spk_fault_history_count(&h, 30001) == 0);

    DETAIL("faults are recorded at claim time, independent of the bounce");
}

/* T24 (finding 1): three faults separated by host reopens must end LATCHED.
 *
 * The old recovery step consumed the host epoch first, dropped the pending
 * bounce, and only then - inside IDLE, with no bounce left - evaluated the
 * latch. Every fault in this sequence therefore reached the history but no
 * fault ever reached the transition, leaving the machine in IDLE.
 *
 * t24_pass() mirrors spk_recovery_step(): claim + decide at the top, then
 * consume the host reopen, then the IDLE bounce. t24_pass_legacy() keeps the
 * old order on purpose, so the two results can be compared directly. */
static spk_recov_state_t t24_pass(spk_fault_history_t *h,
                                  spk_fault_ledger_t *l,
                                  spk_recov_state_t state,
                                  uint32_t fault_at,
                                  uint32_t now,
                                  bool host_reopen)
{
    bool latched_now;

    /* Claim + recurrence decision, before anything consumes the bounce or
     * the epoch (the exact helper the firmware calls). */
    {
        spk_recov_state_t next =
            spk_recovery_claim_decide(h, l, spk_fault_event(fault_at), now,
                                      state);
        latched_now = (next != state);
        state = next;
    }

    if (host_reopen) {
        if (state == SPK_RECOV_LATCHED && !latched_now) {
            /* A latch that predates this pass ends when the host rebuilds
             * the stream; one established by this pass's fault does not. */
            state = SPK_RECOV_IDLE;
        } else if (state == SPK_RECOV_IDLE || state == SPK_RECOV_COOLDOWN) {
            spk_fault_ledger_drop_bounce(l);
        }
    }

    if (state == SPK_RECOV_IDLE) {
        spk_fault_event_t ev = spk_fault_ledger_take(l);

        if (ev.valid) {
            /* The latch was already decided at claim time. */
            state = SPK_RECOV_COOLDOWN;
        }
    }
    return state;
}

/* Old ordering, kept only as the regression witness in the test. */
static spk_recov_state_t t24_pass_legacy(spk_fault_history_t *h,
                                         spk_fault_ledger_t *l,
                                         spk_recov_state_t state,
                                         uint32_t fault_at,
                                         uint32_t now,
                                         bool host_reopen)
{
    (void)spk_fault_ledger_claim(l, h, spk_fault_event(fault_at));

    if (host_reopen) {
        if (state == SPK_RECOV_LATCHED) {
            state = SPK_RECOV_IDLE;
        } else if (state == SPK_RECOV_IDLE || state == SPK_RECOV_COOLDOWN) {
            spk_fault_ledger_drop_bounce(l);
        }
    }

    if (state == SPK_RECOV_IDLE) {
        spk_fault_event_t ev = spk_fault_ledger_take(l);

        if (ev.valid) {
            if (spk_fault_history_should_latch(h, now)) {
                state = SPK_RECOV_LATCHED;
            } else {
                state = SPK_RECOV_COOLDOWN;
            }
        }
    }
    return state;
}

static void t24(void)
{
    spk_fault_history_t h;
    spk_fault_ledger_t l;
    spk_recov_state_t state;

    /* The recurrence decision is independent of the pending bounce: both
     * answers agree once the history holds three faults. */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(1000)) == true);
    CHECK(spk_recovery_next_state(&h, 1000, true, SPK_RECOV_IDLE) ==
          SPK_RECOV_IDLE);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(2000)) == true);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(3000)) == true);
    CHECK(spk_recovery_next_state(&h, 3000, true, SPK_RECOV_IDLE) ==
          SPK_RECOV_LATCHED);
    CHECK(spk_recovery_next_state(&h, 3000, false, SPK_RECOV_IDLE) ==
          SPK_RECOV_LATCHED);

    /* Exact firmware sequence: fault -> host reopen, three times. */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    state = SPK_RECOV_IDLE;
    state = t24_pass(&h, &l, state, 1000, 1000, true);
    CHECK(state == SPK_RECOV_IDLE);
    state = t24_pass(&h, &l, state, 2000, 2000, true);
    CHECK(state == SPK_RECOV_IDLE);
    state = t24_pass(&h, &l, state, 3000, 3000, true);
    CHECK(state == SPK_RECOV_LATCHED);
    CHECK(spk_fault_history_count(&h, 3000) == SPK_RECOV_MAX_IN_WINDOW);
    CHECK(l.recorded == 3);

    /* Same sequence through the old ordering still ends IDLE. If the
     * decision is ever moved back below the bounce consumption, the first
     * CHECK above is the one that fails. */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    state = SPK_RECOV_IDLE;
    state = t24_pass_legacy(&h, &l, state, 1000, 1000, true);
    state = t24_pass_legacy(&h, &l, state, 2000, 2000, true);
    state = t24_pass_legacy(&h, &l, state, 3000, 3000, true);
    CHECK(state == SPK_RECOV_IDLE);
    CHECK(l.recorded == 3);
    CHECK(spk_fault_history_should_latch(&h, 3000) == true);

    /* A fourth fault after the host rebuilds still latches, and the latch
     * survives the reopen that accompanies it. */
    state = t24_pass(&h, &l, SPK_RECOV_IDLE, 4000, 4000, true);
    CHECK(state == SPK_RECOV_LATCHED);

    /* Finding 2 interleaving: the ISR stamps a fault with a time ahead of a
     * "now" sample taken BEFORE the claim. The old ordering fed that stale
     * sample to the history predicate; unsigned subtraction wrapped and the
     * newest fault vanished from the count, so fault three did not latch.
     *
     * The firmware now claims first and takes the decision time afterwards
     * (spk_recovery_decision_ms(pre, post)), so fault_at <= decision_ms by
     * construction. */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(9000)) == true);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(9500)) == true);
    spk_fault_ledger_drop_bounce(&l);

    const uint32_t pre_now   = 10000;   /* sampled before the claim        */
    const uint32_t fault_at  = 10005;   /* ISR fires in the gap            */
    const uint32_t post_now  = 10010;   /* sampled after the claim         */

    /* Old ordering witness: the forward stamp wraps out of the window. */
    CHECK(spk_fault_history_count(&h, pre_now) == 2);
    CHECK(spk_recovery_next_state(&h, pre_now, true, SPK_RECOV_IDLE) !=
          SPK_RECOV_LATCHED);

    /* Fixed ordering: claim, then use the later of the two samples. */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(fault_at)) == true);
    CHECK(spk_recovery_decision_ms(pre_now, post_now) == post_now);
    CHECK(spk_recovery_decision_ms(post_now, pre_now) == post_now);
    /* Wrap-safe: 5 ms is "later" than 4e9 ms in 32-bit monotonic time. */
    CHECK(spk_recovery_decision_ms(4000000000u, 5u) == 5u);
    state = spk_recovery_next_state(&h,
                                    spk_recovery_decision_ms(pre_now, post_now),
                                    true, SPK_RECOV_IDLE);
    CHECK(state == SPK_RECOV_LATCHED);
    CHECK(spk_fault_history_count(&h, fault_at) == SPK_RECOV_MAX_IN_WINDOW);

    DETAIL("three faults with reopens latch before the bounce is consumed; "
           "a forward-stamped fault latches once the decision time is sampled "
           "after the claim");
}

/* T25 (finding 1): a latched recovery with a steady BT link must not keep the
 * USB persona hidden, and the handoff must not clear the speaker latch.
 *
 * Path modelled: the bounce reached the PHY (PHY down), p0_spk_fault_clear()
 * refused the rebuild, spk_recovery_latch() runs, and no further BT event
 * ever arrives to publish CONNECT. Both the recovery's LATCHED handoff and
 * main's loop use spk_visibility_handoff_due() - the same pure predicate the
 * firmware calls - so this sequences the refusal-with-BT-connected case
 * exactly. */
static void t25(void)
{
    spk_fault_history_t h;
    spk_fault_ledger_t l;
    spk_recov_state_t state = SPK_RECOV_IDLE;
    bool bt_connected = true;
    bool phy_up = false;
    bool req_pending = false;

    /* Three faults in the window: the recurrence predicate latches. */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(1000)) == true);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(2000)) == true);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(3000)) == true);
    state = spk_recovery_next_state(&h, 3000,
                                    spk_fault_ledger_pending(&l).valid, state);
    CHECK(state == SPK_RECOV_LATCHED);

    /* The fault-clear refusal happens after the recovery's own DISCONNECT
     * was applied: PHY down, BT steadily connected, nothing in flight. The
     * latch performs no recovery reconnect, so visibility is owned by the
     * handoff predicate. */
    CHECK(spk_visibility_handoff_due(bt_connected, phy_up, req_pending) == true);

    /* Publication is a MAIN-owned CONNECT: it becomes a pending request, so
     * a second pass is a no-op (idempotent) and cannot queue a duplicate. */
    req_pending = true;
    CHECK(spk_visibility_handoff_due(bt_connected, phy_up, req_pending) == false);

    /* usb_task applies it: the request is consumed and the PHY is up. */
    req_pending = false;
    phy_up = true;
    CHECK(spk_visibility_handoff_due(bt_connected, phy_up, req_pending) == false);

    /* The visibility handoff never touches the speaker latch: still latched
     * and still authorised to keep the endpoint down until the host reopens. */
    CHECK(state == SPK_RECOV_LATCHED);
    CHECK(spk_recovery_next_state(&h, 3000, false, state) == SPK_RECOV_LATCHED);

    /* A pending DISCONNECT from any owner must not be overwritten, whether the
     * PHY has already dropped or the edge is still queued. */
    CHECK(spk_visibility_handoff_due(bt_connected, true, true) == false);
    CHECK(spk_visibility_handoff_due(bt_connected, false, true) == false);

    /* BT down: nothing to expose yet (the deferred-disconnect path owns it). */
    CHECK(spk_visibility_handoff_due(false, false, false) == false);

    /* Main's loop guarantee re-evaluates this every pass with no BT event. */
    CHECK(spk_visibility_handoff_due(true, false, false) == true);

    /* Host reopen: the driver's epoch branch drops LATCHED to IDLE, but the
     * fault history is untouched - the latch policy records, it never
     * forgives. */
    state = SPK_RECOV_IDLE;
    CHECK(spk_fault_history_should_latch(&h, 3000) == true);

    DETAIL("latched refusal with steady BT: MAIN CONNECT restores visibility, "
           "speaker stays latched");
}

/* T26 (finding 1, complete 32-bit millisecond wrap sequence): the
 * zero-sentinel / time-normalization mismatch.
 *
 * The ISR used to fold a fault timestamp of 0 to 1, because 0 meant "no event"
 * in the history ring. At the 32-bit ms wrap (~49.7 days) a claim and the
 * post-claim decision sample can both fall in the first millisecond after
 * UINT32_MAX, so the decision time is 0. The folded timestamp 1 then looked
 * 1 ms in the FUTURE to the unsigned (now - at) predicate, was excluded from
 * the count, and the third fault did not latch. No later fault-free pass could
 * reconsider it (claim_decide returns early with no event).
 *
 * This drives the whole boundary sequence through the shipping helpers:
 * faults on both sides of the wrap, the third exactly at 0, decision time 0,
 * LATCHED. On the folding implementation it fails (count 2, state unchanged). */
static void t26(void)
{
    const uint32_t before_wrap1 = 0xFFFFFFFFu - 400u;  /* ~MAX-400 ms */
    const uint32_t before_wrap2 = 0xFFFFFFFFu - 200u;  /* ~MAX-200 ms */
    spk_fault_history_t h;
    spk_fault_ledger_t l;
    spk_recov_state_t state;

    /* --- A: third fault stamped just before the wrap, decision after it --- */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    state = SPK_RECOV_IDLE;

    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(before_wrap1),
                                      before_wrap1, state);
    CHECK(state == SPK_RECOV_IDLE);
    spk_fault_ledger_drop_bounce(&l);        /* host reopened in between */
    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(before_wrap2),
                                      before_wrap2, state);
    CHECK(state == SPK_RECOV_IDLE);
    spk_fault_ledger_drop_bounce(&l);

    /* The claim is stamped UINT32_MAX; the post-claim sample has wrapped. */
    const uint32_t pre_wrap_now  = 0xFFFFFFFFu;
    const uint32_t fault3_at     = 0xFFFFFFFFu;
    const uint32_t post_wrap_now = 0u;

    CHECK(spk_recovery_decision_ms(pre_wrap_now, post_wrap_now) == 0u);
    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(fault3_at),
                                      spk_recovery_decision_ms(pre_wrap_now,
                                                               post_wrap_now),
                                      state);
    CHECK(l.recorded == 3);
    CHECK(l.skipped == 0);
    CHECK(spk_fault_history_count(&h, 0u) == SPK_RECOV_MAX_IN_WINDOW);
    CHECK(state == SPK_RECOV_LATCHED);

    /* --- B: the third fault is stamped exactly 0 (the folded case) ------- */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    state = SPK_RECOV_IDLE;

    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(before_wrap1),
                                      before_wrap1, state);
    spk_fault_ledger_drop_bounce(&l);
    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(0xFFFFFFFFu),
                                      0xFFFFFFFFu, state);
    spk_fault_ledger_drop_bounce(&l);
    CHECK(state == SPK_RECOV_IDLE);

    /* ISR and decision sample both land in millisecond 0. This is a REAL
     * event: claim must not skip it, and record must store 0 verbatim. */
    state = spk_recovery_claim_decide(&h, &l, spk_fault_event(0u), 0u, state);
    CHECK(l.recorded == 3);
    CHECK(l.skipped == 0);
    CHECK(h.at_ms[(h.idx + SPK_RECOV_MAX_IN_WINDOW - 1u) %
                  SPK_RECOV_MAX_IN_WINDOW] == 0u);
    CHECK(spk_fault_history_count(&h, 0u) == SPK_RECOV_MAX_IN_WINDOW);
    CHECK(spk_fault_history_should_latch(&h, 0u) == true);
    CHECK(state == SPK_RECOV_LATCHED);

    /* --- C: the exact failure mode of the folding code -------------------- */
    /* The old predicate was `(uint32_t)(now - at) < WINDOW`: a folded
     * timestamp 1 against decision time 0 wraps to ~2^32 and is excluded.
     * The shipping signed-age form clamps a future stamp to 0 and counts it,
     * so even a bad fold could no longer hide a fault. */
    CHECK((uint32_t)(0u - 1u) >= (uint32_t)SPK_RECOV_WINDOW_MS);
    CHECK(spk_fault_slot_recent(1u, 0u) == true);
    /* Unfolded timestamp 0 against decision time 0 is age 0: recent. */
    CHECK(spk_fault_slot_recent(0u, 0u) == true);
    /* Wrap edges both ways: MAX -> 0 is +1 ms, 0 -> MAX is -1 ms (clamped). */
    CHECK(spk_fault_slot_recent(0xFFFFFFFFu, 0u) == true);
    CHECK(spk_fault_slot_recent(0u, 0xFFFFFFFFu) == true);
    /* A window's worth of separation is still distance, not a wrap. */
    CHECK(spk_fault_slot_recent(0u,
                                (uint32_t)SPK_RECOV_WINDOW_MS) == false);

    /* --- D: no event at all is not a fault at 0 --------------------------- */
    spk_fault_history_init(&h);
    spk_fault_ledger_init(&l);
    state = SPK_RECOV_IDLE;
    state = spk_recovery_claim_decide(&h, &l, spk_fault_event_none(), 0u,
                                      state);
    CHECK(state == SPK_RECOV_IDLE);
    CHECK(l.recorded == 0);
    CHECK(l.skipped == 1);
    CHECK(spk_fault_history_count(&h, 0u) == 0);

    /* A pending bounce stamped 0 is still a pending bounce (it used to be
     * indistinguishable from "no bounce"). */
    CHECK(spk_fault_ledger_claim(&l, &h, spk_fault_event(0u)) == true);
    CHECK(spk_fault_ledger_pending(&l).valid == true);
    CHECK(spk_fault_ledger_pending(&l).at_ms == 0u);

    DETAIL("third fault at the 32-bit ms wrap latches: validity flag, no "
           "0->1 fold, signed age");
}

/* T27 (finding 2): applied-edge timing and recovery takeover.
 *
 * Main's grace used to start on the first OBSERVATION of "PHY down" and only
 * reset when the observation stopped. A CONNECT/DISCONNECT pair applied
 * between two loop passes left the earlier down timestamp in place, so main
 * could publish CONNECT < 250 ms after the LATEST disconnect. The rule is now
 * owned by the applied-edge generation/timestamp.
 *
 * The second half drives the recovery's DISCONNECTED wait: when main's
 * handoff applies a CONNECT while recovery is DISCONNECTED, the PHY comes up
 * with a newer applied generation. That is a takeover, not a lost disconnect -
 * the recovery must resolve to IDLE and must NOT publish a second DISCONNECT.
 *
 * t27_main_pass() mirrors main.c's loop block exactly, and
 * t27_disconnected_pass() mirrors the DISCONNECTED branch of
 * spk_recovery_step(), both using the shipping policy helpers. */
static bool t27_main_pass(bool bt_up, bool phy_up, bool req_pending,
                          usb_phy_snapshot_t edge,
                          uint32_t *grace_gen, uint32_t *grace_base_ms,
                          uint32_t now_ms)
{
    bool handoff_due = spk_visibility_handoff_due(bt_up, phy_up, req_pending);

    if (!handoff_due) {
        *grace_gen = 0;
        *grace_base_ms = 0;
        return false;
    }
    if (*grace_gen != edge.gen) {
        *grace_gen = edge.gen;
        *grace_base_ms = edge.last_ms;
    }
    if (spk_visibility_grace_elapsed(!edge.connected, *grace_base_ms,
                                     now_ms)) {
        *grace_base_ms = now_ms;      /* bounded retry, as in main.c */
        return true;
    }
    return false;
}

/* Mirrors SPK_RECOV_DISCONNECTED: returns true if it re-published DISCONNECT.
 * `connected` is the direction from the SAME snapshot as `applied_gen`. */
static bool t27_disconnected_pass(spk_recov_state_t *state, bool connected,
                                  uint32_t applied_gen,
                                  uint32_t gen_at_disc_request,
                                  bool epoch_changed, uint32_t *takeovers)
{
    if (*state != SPK_RECOV_DISCONNECTED) {
        return false;                 /* the switch only runs this branch */
    }
    if (!connected) {
        return false;                 /* PHY low: the low period runs */
    }
    if (epoch_changed) {
        *state = SPK_RECOV_IDLE;      /* host rebuilt the stream */
        return false;
    }
    if (spk_recovery_edge_takeover(connected, applied_gen,
                                   gen_at_disc_request)) {
        *state = SPK_RECOV_IDLE;      /* confirmed, do not disconnect again */
        (*takeovers)++;
        return false;
    }
    return true;                      /* our DISCONNECT still unapplied */
}

/* Attempt baseline: mirrors spk_attempt_gen / spk_attempt_gen_valid in
 * ds4_usb_audio.c. Captured once when IDLE starts the attempt. */
typedef struct {
    uint32_t gen;
    bool     valid;
} t27_attempt_t;

/* Mirrors the SPK_RECOV_COOLDOWN publication gate after the two-pass fix.
 * `snap_connected`/`snap_gen` come from ONE usb_phy_snapshot(); `pub_gen` is
 * the applied generation that usb_phy_request_if_gen() validates against (a
 * caller moves it to model an edge landing in the snapshot/publication gap).
 * Returns true only when a DISCONNECT was actually published. */
static bool t27_cooldown_pass(spk_recov_state_t *state, t27_attempt_t *attempt,
                              bool snap_connected, uint32_t snap_gen,
                              uint32_t pub_gen, bool *reconnect_pending,
                              uint32_t *takeovers)
{
    if (*state != SPK_RECOV_COOLDOWN) {
        return false;                 /* the switch only runs this branch */
    }
    if (!attempt->valid) {
        attempt->gen = snap_gen;      /* defensive: IDLE always seeds it */
        attempt->valid = true;
    }
    if (snap_gen != attempt->gen) {
        if (snap_connected) {
            /* External CONNECT: abandon the attempt, clear the reconnect
             * obligation and never rebase the baseline to this generation. */
            *state = SPK_RECOV_IDLE;
            attempt->valid = false;
            *reconnect_pending = false;
            (*takeovers)++;
            return false;
        }
        /* External DISCONNECT with the PHY down: no live stream to undo,
         * so the new generation is a safe baseline. */
        attempt->gen = snap_gen;
    }
    if (pub_gen != attempt->gen) {
        return false;                 /* rejected; the baseline is preserved */
    }
    *state = SPK_RECOV_DISCONNECTED;  /* shipping order after publication */
    return true;                      /* DISCONNECT published */
}

static void t27(void)
{
    usb_phy_snapshot_t edge = { 0, 0, true };
    uint32_t grace_gen = 0;
    uint32_t grace_base_ms = 0;
    uint32_t takeovers = 0;

    /* ---- main: grace tied to the latest applied edge ------------------- */
    /* Recovery applied its DISCONNECT at 1000 ms; the PHY is down. */
    edge.gen = 7;
    edge.last_ms = 1000;
    edge.connected = false;

    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1100) == false);
    CHECK(grace_base_ms == 1000);
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1249) == false);          /* 249 ms: still waiting */
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1250) == true);           /* 250 ms: publish */

    /* Reset to the pre-edge state for the intervening-edge case. */
    grace_gen = 0;
    grace_base_ms = 0;
    edge.gen = 7;
    edge.last_ms = 1000;
    edge.connected = false;
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1100) == false);
    CHECK(grace_base_ms == 1000);

    /* Between two passes: main (or the recovery) applies CONNECT at 1210 and
     * DISCONNECT at 1230. Only the LAST edge is observable now, and its
     * generation is new. */
    edge.gen = 9;
    edge.last_ms = 1230;
    edge.connected = false;

    /* The next pass must measure from 1230, not from 1000. */
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1240) == false);
    CHECK(grace_base_ms == 1230);
    /* 20 ms after the latest disconnect the old observation baseline (1000)
     * would already have published; the applied-edge rule does not. */
    CHECK(spk_visibility_grace_elapsed(true, 1000, 1260) == true);
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1260) == false);
    /* ...and only publishes 250 ms after 1230. */
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1479) == false);
    CHECK(t27_main_pass(true, false, false, edge, &grace_gen, &grace_base_ms,
                        1480) == true);

    /* Any new applied edge restarts the window again - a CONNECT means the
     * PHY is up, so nothing is due at all. */
    edge.gen = 10;
    edge.last_ms = 1490;
    edge.connected = true;
    CHECK(t27_main_pass(true, true, false, edge, &grace_gen, &grace_base_ms,
                        1800) == false);
    CHECK(spk_visibility_grace_elapsed(false, 1490, 1800) == false);

    /* BT down or an in-flight request drops the baseline (unchanged rule). */
    CHECK(t27_main_pass(false, false, false, edge, &grace_gen, &grace_base_ms,
                        2000) == false);

    /* ---- recovery takeover: DISCONNECTED + external CONNECT ------------ */
    const uint32_t gen_at_disc = 41;
    spk_recov_state_t state = SPK_RECOV_DISCONNECTED;

    /* Our OWN DISCONNECT applied: the generation moved, but the same
     * snapshot says the PHY is down. It must never be classified as a
     * takeover (that was the read-up-then-read-generation race). */
    CHECK(t27_disconnected_pass(&state, false, gen_at_disc + 1, gen_at_disc,
                                false, &takeovers) == false);
    CHECK(state == SPK_RECOV_DISCONNECTED);
    CHECK(takeovers == 0);

    /* Our DISCONNECT is still unapplied (same generation): keep retrying. */
    CHECK(t27_disconnected_pass(&state, true, 41, gen_at_disc, false,
                               &takeovers) == true);
    CHECK(state == SPK_RECOV_DISCONNECTED);
    CHECK(takeovers == 0);

    /* Main's handoff applied CONNECT: the generation moved. Takeover. */
    CHECK(t27_disconnected_pass(&state, true, 42, gen_at_disc, false,
                               &takeovers) == false);
    CHECK(state == SPK_RECOV_IDLE);
    CHECK(takeovers == 1);

    /* Subsequent passes are quiet: no second DISCONNECT is ever published. */
    CHECK(t27_disconnected_pass(&state, true, 42, gen_at_disc, false,
                               &takeovers) == false);
    CHECK(state == SPK_RECOV_IDLE);
    CHECK(takeovers == 1);

    /* A PHY that is down is never a takeover (the low period owns it). */
    CHECK(spk_recovery_edge_takeover(false, 42, gen_at_disc) == false);
    /* A host reconstruction is still handled by the epoch branch first. */
    state = SPK_RECOV_DISCONNECTED;
    CHECK(t27_disconnected_pass(&state, true, 42, gen_at_disc, true,
                               &takeovers) == false);
    CHECK(state == SPK_RECOV_IDLE);

    /* CONNECTING + PHY up is always a confirm (whoever applied the edge). */
    state = SPK_RECOV_CONNECTING;
    if (state == SPK_RECOV_CONNECTING) {          /* the firmware's top block */
        state = SPK_RECOV_IDLE;
        takeovers++;
    }
    CHECK(state == SPK_RECOV_IDLE);
    CHECK(takeovers == 2);

    /* ---- COOLDOWN: two-pass interleaving (finding 2) -------------------- */
    {
        t27_attempt_t attempt = { 41, true };
        bool reconnect_pending = false;
        uint32_t pubs = 0;

        state = SPK_RECOV_COOLDOWN;

        /* Pass 1: our snapshot still sees baseline 41. An external CONNECT
         * lands between that snapshot and usb_phy_request_if_gen(), which
         * validates against 42 and rejects the DISCONNECT. The attempt
         * baseline must survive the rejection. */
        if (t27_cooldown_pass(&state, &attempt, true, 41, 42,
                              &reconnect_pending, &takeovers)) {
            pubs++;
        }
        CHECK(pubs == 0);
        CHECK(state == SPK_RECOV_COOLDOWN);
        CHECK(attempt.valid == true);
        CHECK(attempt.gen == 41);

        /* Pass 2: the external CONNECT is now the applied edge (gen 42, PHY
         * up). Re-capturing the baseline here is exactly the old bug: it
         * would publish DISCONNECT against 42 and undo the takeover. */
        if (t27_cooldown_pass(&state, &attempt, true, 42, 42,
                              &reconnect_pending, &takeovers)) {
            pubs++;
        }
        CHECK(pubs == 0);                   /* no second DISCONNECT */
        CHECK(state == SPK_RECOV_IDLE);     /* abandoned: host wins */
        CHECK(attempt.valid == false);
        CHECK(reconnect_pending == false);  /* reconnect obligation cleared */

        /* A later pass cannot resurrect the abandoned attempt. */
        CHECK(t27_cooldown_pass(&state, &attempt, true, 43, 43,
                                &reconnect_pending, &takeovers) == false);
        CHECK(pubs == 0);
        CHECK(state == SPK_RECOV_IDLE);

        /* Sanity: the same pass with no external edge DOES publish. */
        state = SPK_RECOV_COOLDOWN;
        attempt.gen = 50;
        attempt.valid = true;
        if (t27_cooldown_pass(&state, &attempt, true, 50, 50,
                              &reconnect_pending, &takeovers)) {
            pubs++;
        }
        CHECK(pubs == 1);
        CHECK(state == SPK_RECOV_DISCONNECTED);

        /* An external DISCONNECT with the PHY already down may be adopted:
         * there is no connected edge to undo. */
        state = SPK_RECOV_COOLDOWN;
        attempt.gen = 60;
        attempt.valid = true;
        if (t27_cooldown_pass(&state, &attempt, false, 61, 61,
                              &reconnect_pending, &takeovers)) {
            pubs++;
        }
        CHECK(pubs == 2);
        CHECK(attempt.gen == 61);
        CHECK(state == SPK_RECOV_DISCONNECTED);
    }

    DETAIL("grace follows the applied edge; a rejected DISCONNECT keeps the "
           "attempt baseline and never republishes against an external "
           "CONNECT");
}

/* T28 (finding 1): redundant PHY publications are no-ops.
 *
 * usb_phy_process() used to write the register, bump the generation and
 * restamp the timestamp unconditionally. A DISCONNECT->DISCONNECT therefore
 * restarted main's grace window, and a CONNECT->CONNECT could falsely satisfy
 * the recovery's takeover check. The pure identity rule below is the one the
 * firmware applies under the IRQ mask. */
static void t28(void)
{
    usb_phy_identity_t id;
    usb_phy_identity_t next;

    /* Applied CONNECT, gen 7 at 1000 ms. */
    id.connected = true;
    id.gen = 7;
    id.at_ms = 1000;

    /* CONNECT again at 1100: no write, same generation, same timestamp. */
    CHECK(usb_phy_identity_changes(id.connected, true) == false);
    next = usb_phy_identity_apply(id, true, 1100);
    CHECK(next.connected == true);
    CHECK(next.gen == 7);
    CHECK(next.at_ms == 1000);

    /* Applied DISCONNECT, gen 8 at 1200 ms. */
    id.connected = false;
    id.gen = 8;
    id.at_ms = 1200;

    /* DISCONNECT again at 1300: also a no-op. In particular it must not
     * restamp the disconnect time main's grace measures from. */
    CHECK(usb_phy_identity_changes(id.connected, false) == false);
    next = usb_phy_identity_apply(id, false, 1300);
    CHECK(next.connected == false);
    CHECK(next.gen == 8);
    CHECK(next.at_ms == 1200);
    CHECK(spk_visibility_grace_elapsed(!next.connected, next.at_ms, 1500) ==
          true);                        /* measured from 1200, not 1300 */

    /* A real direction change bumps the generation exactly once and stamps
     * the time of that change. */
    next = usb_phy_identity_apply(id, true, 1400);
    CHECK(next.connected == true);
    CHECK(next.gen == 9);
    CHECK(next.at_ms == 1400);

    /* CONNECT -> DISCONNECT -> CONNECT: two real edges, two generations. */
    id = next;                          /* CONNECT, gen 9, 1400 */
    id = usb_phy_identity_apply(id, false, 1500);
    CHECK(id.gen == 10 && id.at_ms == 1500 && !id.connected);
    id = usb_phy_identity_apply(id, true, 1600);
    CHECK(id.gen == 11 && id.at_ms == 1600 && id.connected);

    /* A no-op cannot satisfy a takeover check: it leaves the generation at
     * the baseline, so the recovery still sees "our DISCONNECT unapplied". */
    CHECK(spk_recovery_edge_takeover(true, 11, 11) == false);
    CHECK(spk_recovery_edge_takeover(true, 12, 11) == true);

    DETAIL("same-direction publications are no-ops: no write, no generation "
           "bump, no restamp");
}

/* T29 (non-blocking cleanup): the fault ring has a structural expiry.
 *
 * The ring had no way to forget a slot, so after the signed age wrapped at
 * 2^31 ms an ancient timestamp could be counted as "recent" again. Slots
 * outside the 10 s window are now invalidated on record and on decision, and
 * spk_recovery_step() expires at the TOP of every pass - including fault-free
 * passes that take no claim/decision path. */
static void t29(void)
{
    spk_fault_history_t h;
    const uint32_t half_wrap = 0x80000000u;

    /* A stale slot is retired by the first decision outside the window. */
    spk_fault_history_init(&h);
    spk_fault_history_record(&h, 1000u);
    CHECK(spk_fault_history_count(&h, 10999u) == 1);
    CHECK((h.valid_mask & 1u) != 0);   /* still inside the window */
    CHECK(spk_fault_history_count(&h, 11000u) == 0);
    CHECK((h.valid_mask & 1u) == 0);   /* structurally invalidated */

    /* Without the structural clear that ancient timestamp would be
     * resurrected as "recent" once the signed age clamps around 2^31. It
     * cannot be counted again. */
    CHECK(spk_fault_history_count(&h, 1000u + half_wrap) == 0);
    CHECK(spk_fault_history_should_latch(&h, 1000u + half_wrap) == false);

    /* Recording also retires stale slots before it overwrites one. */
    spk_fault_history_init(&h);
    spk_fault_history_record(&h, 1000u);
    spk_fault_history_record(&h, 12000u);
    CHECK((h.valid_mask & 1u) == 0);   /* 1000 aged out at 12000 */
    CHECK(h.valid_mask == (1u << 1));
    CHECK(spk_fault_history_count(&h, 12000u) == 1);

    /* Shipping order: spk_recovery_step() expires at the TOP of every pass,
     * before any early exit, including a fault-free pass that never reaches a
     * claim or a decision. A long quiet interval must therefore retire the
     * slot on its own - not wait for the next record()/count(). */
    spk_fault_history_init(&h);
    spk_fault_history_record(&h, 1000u);
    spk_fault_history_expire(&h, 5000u);       /* quiet pass, still inside */
    CHECK((h.valid_mask & 1u) != 0);
    spk_fault_history_expire(&h, 11000u);      /* quiet pass, just outside */
    CHECK((h.valid_mask & 1u) == 0);
    /* No resurrection even if the next pass only observes the ring around the
     * signed-age clamp: the slot is gone before that can happen. */
    spk_fault_history_expire(&h, 11000u + half_wrap);
    CHECK(spk_fault_history_count(&h, 11000u + half_wrap) == 0);
    CHECK(spk_fault_history_should_latch(&h, 11000u + half_wrap) == false);

    /* Expiry does not break the wrap sequence: faults on both sides of the
     * 32-bit wrap are all inside the window. */
    spk_fault_history_init(&h);
    spk_fault_history_record(&h, 0xFFFFFFFFu - 400u);
    spk_fault_history_record(&h, 0xFFFFFFFFu - 200u);
    spk_fault_history_record(&h, 0xFFFFFFFFu);
    CHECK(spk_fault_history_count(&h, 0u) == 3);
    CHECK(spk_fault_history_should_latch(&h, 0u) == true);

    DETAIL("fault-history slots outside the 10 s window are structurally "
           "invalidated and cannot be resurrected");
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    static const struct {
        int id;
        void (*fn)(void);
    } tests[] = {
        { 1, t01 }, { 2, t02 }, { 3, t03 }, { 4, t04 },
        { 5, t05 }, { 6, t06 }, { 7, t07 }, { 8, t08 },
        { 9, t09 }, { 10, t10 }, { 11, t11 }, { 12, t12 },
        { 13, t13 }, { 14, t14 }, { 15, t15 }, { 16, t16 },
        { 17, t17 }, { 18, t18 }, { 19, t19 }, { 20, t20 },
        { 21, t21 }, { 22, t22 }, { 23, t23 }, { 24, t24 },
        { 25, t25 }, { 26, t26 }, { 27, t27 },
        { 28, t28 }, { 29, t29 },
    };

    int pass = 0, fail = 0, skip = 0;

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        g_fails = 0;
        g_skip = false;
        g_detail[0] = '\0';

        tests[i].fn();

        if (g_fails == 0 && g_lock_depth != 0)
            g_fails++;                    /* leaked critical section */

        const char *verdict;
        if (g_fails > 0) {
            verdict = "FAIL";
            fail++;
        } else if (g_skip) {
            verdict = "SKIP";
            skip++;
        } else {
            verdict = "PASS";
            pass++;
        }
        printf("T%d %s %s\n", tests[i].id, verdict, g_detail);
    }

    if (g_lock_violations != 0) {
        printf("---- re-entrant public call detected (%d) ----\n",
               g_lock_violations);
        fail++;
    }

    printf("---- %s (pass=%d fail=%d skip=%d) ----\n",
           fail ? "FAILED" : (skip ? "INCOMPLETE (cases unverified)" : "ALL PASS"),
           pass, fail, skip);

    if (fail > 0)
        return 1;
    if (skip > 0)
        return 3;
    return 0;
}
