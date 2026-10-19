/*
 * bflb_usb_v2_guard.h - pure decision logic for the BL6xx USB v2 device driver.
 *
 * Header-only, static inline, zero SDK dependencies (only <stdint.h> and
 * <stdbool.h>) so the decision tables below can be compiled and unit tested on
 * a host PC:
 *
 *     gcc -std=c11 -Wall -Wextra -fsyntax-only -I drivers/lhal/src <tu.c>
 *
 * It contains two independent pieces of policy, both taken verbatim from
 * P0 protocol v2 spec sections 4.4 and 4.6:
 *
 *   1. bflb_f0_guard_*: the F0 (speaker ISO OUT) VDMA deadlock watchdog that
 *      replaces the bare "count >= 200 && 50 ms" heuristic in
 *      bflb_usb_v2.c. It decides, per G3 pass, between NONE / ABORT / SYNTH /
 *      FAULT. Since M1 an armed FIFO with no queued bytes is an *idle host*
 *      (pause without SET_INTERFACE alt 0), not a deadlock: the guard only
 *      aborts when the FIFO still holds data the VDMA is not draining.
 *      Since M4 the 50 ms / 200 pass evidence window starts at the *first
 *      observation of queued data with no progress*, not at arm(): time spent
 *      with an empty FIFO must never be charged against data that arrives
 *      later (an idle pause followed by one packet is not a stall).
 *   2. bflb_ep_cancel_witness_*: the cancel witness used to prove that an
 *      aborted FIFO really went silent (a latched completion bit in the pass
 *      after the abort means the transfer is still bleeding through).
 *
 * Nothing in this file touches a register; the driver supplies the observed
 * hardware state per pass and performs the returned action.
 */

#ifndef BFLB_USB_V2_GUARD_H
#define BFLB_USB_V2_GUARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* F0 deadlock watchdog (spec 4.6)                                          */
/* ------------------------------------------------------------------------ */

/* A transfer must have had queued data with no progress for at least this
 * long ... */
#define BFLB_F0_ARM_MIN_US      50000ULL
/* ... and for at least this many G3 passes *since that data was first seen*
 * (never since arm(): an idle paste must not shorten the evidence window). */
#define BFLB_F0_ARM_MIN_PASSES  200u
/* Aborts allowed in one bus session before the endpoint is declared faulty. */
#define BFLB_F0_MAX_ABORTS      3u

typedef enum {
    BFLB_F0_IDLE = 0,     /* no transfer outstanding; counters cleared        */
    BFLB_F0_ARMED,        /* transfer(s) running; watching for a deadlock     */
    BFLB_F0_RECOVERING,   /* aborted; waiting for one clean G3 pass           */
    BFLB_F0_FAULT,        /* terminal until reset()/a successful arm()        */
} bflb_f0_state_t;

typedef enum {
    BFLB_F0_ACT_NONE = 0, /* nothing to do this pass                          */
    BFLB_F0_ACT_ABORT,    /* stop the FIFO, enter RECOVERING                   */
    BFLB_F0_ACT_SYNTH,    /* emit one synthetic zero-length OUT completion     */
    BFLB_F0_ACT_FAULT,    /* give up: stop the FIFO, notify the class layer    */
} bflb_f0_action_t;

/*
 * Per-guard counters. The driver mirrors these into its global volatile
 * counters (g_usb_f0_safetynet_cnt, ...) after every pass; they only ever
 * increase while the driver lives.
 */
typedef struct {
    uint32_t safetynet;       /* g_usb_f0_safetynet_cnt : criterion hit        */
    uint32_t misfire;         /* g_usb_f0_misfire_cnt   : blocked by 3/4/6     */
    uint32_t stale_drop;      /* g_usb_f0_stale_drop_cnt: dropped old complete */
    uint32_t witness_timeout; /* g_usb_f0_witness_timeout                      */
    uint32_t synth;           /* g_usb_f0_synth_cnt     : synthetic completions*/
    uint32_t fault;           /* g_usb_f0_fault_cnt     : entries into FAULT   */
    uint32_t idle;            /* g_usb_f0_idle_cnt      : empty FIFO evaluated,
                               *                          deliberately not
                               *                          aborted (idle host)  */
} bflb_f0_counters_t;

/*
 * Guard instance. Only the USB ISR touches it (plus reset()/arm() callers,
 * which run in the ISR or in the endpoint-open path).
 *
 *   state      : current state
 *   guard_ep   : endpoint index this instance guards (0 = disabled)
 *   abort_cnt  : aborts since the last reset(), the last real completion or
 *                the last successful synthetic-completion recovery; this is
 *                the *consecutive* budget, it never accumulates across
 *                opens/pauses (M2)
 *   abort_streak : aborts since the last real completion of the guarded FIFO.
 *                A synthetic completion is not forward progress by itself, so
 *                this is what escalates to FAULT (a deadlock that survives
 *                three recoveries without ever completing).
 *   progress   : a completion for this FIFO was seen since the last arm()
 *   passes     : G3 passes since the last arm() / since RECOVERING was entered
 *   data_passes: G3 passes since queued data was first observed without any
 *                completion (0 = no candidate stall window open)
 *   arm_us     : timestamp of the last arm() of guard_ep
 *   data_since_us: timestamp of that first queued-data/no-progress pass; the
 *                50 ms evidence window starts here, never at arm_us
 */
typedef struct {
    bflb_f0_state_t     state;
    uint8_t             guard_ep;
    uint8_t             abort_cnt;
    uint8_t             abort_streak;
    uint8_t             progress;
    uint32_t            passes;
    uint32_t            data_passes;
    uint64_t            arm_us;
    uint64_t            data_since_us;
    bflb_f0_counters_t  cnt;
} bflb_f0_guard_t;

/*
 * Hardware observations for exactly one G3 pass.
 *
 *   guard_ep : value of g_usb_f0_guard_ep (endpoint index, 0 = disabled)
 *   fifo_ep  : endpoint index currently mapped to FIFO0 (may be 0x0f/unmapped)
 *   armed    : out_ep[guard_ep].ep_active (a transfer is outstanding)
 *   start    : START bit read back from the FIFO0 VDMA PS1 register
 *   cmplt    : FIFO0 completion bit, taken from the G3 status snapshot that
 *              was sampled BEFORE bflb_usb_source_group_int_clear(3, st)
 *   fifo_bytes : used byte count of FIFO0 (FIBC0 at USB_BASE + 0x1B0, masked
 *              with USB_BC_F0_MASK). 0 with no completion = idle host, not a
 *              deadlock (M1).
 *   now_us   : monotonic microseconds
 */
typedef struct {
    uint8_t  guard_ep;
    uint8_t  fifo_ep;
    bool     armed;
    bool     start;
    bool     cmplt;
    uint16_t fifo_bytes;
    uint64_t now_us;
} bflb_f0_input_t;

/* Bind the instance to an endpoint and start from a clean slate. */
static inline void bflb_f0_guard_init(bflb_f0_guard_t *g, uint8_t guard_ep)
{
    if (!g) {
        return;
    }
    g->state = BFLB_F0_IDLE;
    g->guard_ep = guard_ep;
    g->abort_cnt = 0;
    g->abort_streak = 0;
    g->progress = 0;
    g->passes = 0;
    g->data_passes = 0;
    g->arm_us = 0;
    g->data_since_us = 0;
    g->cnt.safetynet = 0;
    g->cnt.misfire = 0;
    g->cnt.stale_drop = 0;
    g->cnt.witness_timeout = 0;
    g->cnt.synth = 0;
    g->cnt.fault = 0;
    g->cnt.idle = 0;
}

/*
 * Reset the guard budget. Called on USBRST and on usbd_ep_open() of the
 * guarded endpoint (the transfer history of the previous bus/endpoint
 * instance must not leak into the next one). Counters are preserved.
 */
static inline void bflb_f0_guard_reset(bflb_f0_guard_t *g)
{
    if (!g) {
        return;
    }
    g->state = BFLB_F0_IDLE;
    g->abort_cnt = 0;
    g->abort_streak = 0;
    g->progress = 0;
    g->passes = 0;
    g->data_passes = 0;
    g->arm_us = 0;
    g->data_since_us = 0;
}

/*
 * A transfer was just started on ep_idx (usbd_ep_start_read/write). guard_ep
 * is the application-configured g_usb_f0_guard_ep; only arms that hit the
 * guarded endpoint matter.
 *
 * Rule R6.1: a successful arm revives FAULT (the application may only arm
 * after a cancel+settle, i.e. after proving the endpoint silent); arming does
 * not clear the abort budget, so a revoked budget cannot be laundered by
 * re-arming. The budget is only cleared by bflb_f0_guard_reset() (bus reset /
 * endpoint open), by a real completion of the guarded FIFO, or by a
 * successful synthetic-completion recovery (M2).
 */
static inline void bflb_f0_guard_arm(bflb_f0_guard_t *g, uint8_t guard_ep,
                                     uint8_t ep_idx, uint64_t now_us)
{
    if (!g || guard_ep == 0 || ep_idx != guard_ep) {
        return;
    }
    g->guard_ep = guard_ep;
    g->state = BFLB_F0_ARMED;
    g->progress = 0;
    g->passes = 0;
    g->data_passes = 0;
    g->arm_us = now_us;
    g->data_since_us = 0;
}

/*
 * One G3 pass. Returns exactly one action; all bookkeeping is done here.
 *
 * Transition table (spec 4.6 + M1 idle-host criterion, notes split out):
 *
 *   state       input                                  action    next
 *   ---------   ------------------------------------   -------   -----------
 *   any         guard_ep == 0                          NONE      IDLE (clear
 *                                                                  passes/
 *                                                                  progress/
 *                                                                  data window)
 *   IDLE/ARMED  !armed                                 NONE      IDLE (clear
 *                                                                  passes/
 *                                                                  progress/
 *                                                                  data window)
 *   IDLE        armed                                  NONE      ARMED
 *   ARMED       cmplt                                  NONE      ARMED
 *                                                                  progress=1,
 *                                                                  data window
 *                                                                  reset,
 *                                                                  abort_cnt=0,
 *                                                                  abort_streak=0
 *                                                                  (if fifo_ep
 *                                                                  == guard_ep)
 *   ARMED       fifo_ep != guard_ep                    NONE      ARMED
 *                                                                  data window
 *                                                                  reset
 *   ARMED       !cmplt, progress != 0                  NONE      ARMED
 *                                                                  misfire++
 *                                                                  (data window
 *                                                                  reset)
 *   ARMED       !cmplt, progress == 0,
 *               fifo_bytes == 0 (host paused: the FIFO
 *               is empty, the VDMA has nothing to drain) NONE     ARMED
 *                                                                  idle++ (M1:
 *                                                                  NEVER abort)
 *                                                                  data window
 *                                                                  reset (M4:
 *                                                                  idle never
 *                                                                  counts)
 *   ARMED       !cmplt, progress == 0,
 *               fifo_bytes > 0, data window opens /
 *               advances                               NONE      ARMED
 *                                                                  (data_passes
 *                                                                  < 200 or
 *                                                                  < 50 ms since
 *                                                                  the window
 *                                                                  opened:
 *                                                                  misfire++ in
 *                                                                  the 50 ms case)
 *   ARMED       !cmplt, progress == 0,
 *               fifo_bytes > 0, data_passes >= 200,
 *               elapsed >= 50 ms, START == 0 (VDMA
 *               gone with bytes still queued)         ABORT     RECOVERING
 *                                                                  (or FAULT)
 *                                                                  misfire++
 *   ARMED       !cmplt, progress == 0,
 *               fifo_bytes > 0, data_passes >= 200,
 *               elapsed >= 50 ms, START == 1 (VDMA
 *               running, bytes stuck)                 ABORT     RECOVERING
 *                                                                  (or FAULT)
 *                                                                  safetynet++
 *   RECOVERING  cmplt                                  NONE      RECOVERING
 *                                                                  stale_drop++
 *   RECOVERING  !cmplt (first clean pass)               SYNTH     ARMED
 *                                                                  synth++,
 *                                                                  abort_cnt=0,
 *                                                                  progress=0,
 *                                                                  passes=0,
 *                                                                  data window
 *                                                                  reset,
 *                                                                  arm_us=now
 *   FAULT       anything                               NONE      FAULT
 *
 * "or FAULT": if abort_streak reaches BFLB_F0_MAX_ABORTS the action becomes
 * BFLB_F0_ACT_FAULT instead of BFLB_F0_ACT_ABORT, fault++ happens once on
 * entry, and the state becomes terminal for this endpoint episode (the
 * application-level recovery clears it with p0_spk_fault_clear() after a
 * controlled USB reconnect; a host SET_INTERFACE bus reset also clears it).
 *
 * Criteria (all must hold for a real hit, spec 4.6 + M1 + M4):
 *   1. guard_ep != 0 and fifo_ep == guard_ep
 *   2. armed (ep_active == true)
 *   3. START readback == 1, or START == 0 with bytes still queued
 *   4. now - data_since_us >= 50 ms, where data_since_us is the first pass
 *      that saw queued data with no progress (NOT arm_us; M4)
 *   5. data_passes >= 200, counted from that same first observation
 *   6. progress == 0 (never completed since this arm)
 *   7. fifo_bytes > 0 (an empty FIFO is an idle host, never a deadlock)
 *
 * M4 in one line: the evidence window opens on the first queued-data pass and
 * is reset by any completion, by an empty FIFO and by every state where the
 * guard does not own FIFO0. Time spent idle is never charged against data
 * that arrives later, so "long pause, then one packet" cannot abort.
 */
static inline bflb_f0_action_t bflb_f0_guard_armed_step(bflb_f0_guard_t *g,
                                                        const bflb_f0_input_t *in)
{
    g->passes++;

    if (in->cmplt) {
        g->progress = 1;
        /* M2: a completion of the guarded FIFO is real forward progress.
         * The abort budget is consecutive, so it must not survive it. The
         * completion bit belongs to FIFO0; credit it only while the guard
         * actually owns that FIFO. */
        if (in->fifo_ep == in->guard_ep) {
            g->abort_cnt = 0;
            g->abort_streak = 0;
        }
        /* M4: forward progress closes the stall window. */
        g->data_passes = 0;
        g->data_since_us = 0;
        return BFLB_F0_ACT_NONE;
    }

    /* Criterion 1: the guard must actually own FIFO0. Nothing observed about
     * a FIFO the guard does not own may start (or extend) a stall window. */
    if (in->fifo_ep != in->guard_ep) {
        g->data_passes = 0;
        g->data_since_us = 0;
        return BFLB_F0_ACT_NONE;
    }

    /* Criterion 6: a transfer that completed is not a deadlock, no matter how
     * long the endpoint has been armed. This is what makes "50 ms" not a
     * deadlock criterion by itself. The stall window is attributed to the
     * *next* transfer, so it restarts here. */
    if (g->progress != 0) {
        g->data_passes = 0;
        g->data_since_us = 0;
        if (g->passes >= BFLB_F0_ARM_MIN_PASSES) {
            g->cnt.misfire++;
        }
        return BFLB_F0_ACT_NONE;
    }

    /* Criterion 7 (M1): the FIFO is empty and no completion is pending.
     * Nothing is queued, so the VDMA has nothing to drain: this is a host
     * that paused playback while leaving alt 1 selected, not a deadlock.
     * M4: the stall window is closed here unconditionally, so an arbitrarily
     * long idle period is never charged against data that arrives later.
     * Never abort. */
    if (in->fifo_bytes == 0) {
        g->data_passes = 0;
        g->data_since_us = 0;
        if (g->passes >= BFLB_F0_ARM_MIN_PASSES) {
            g->cnt.idle++;
        }
        return BFLB_F0_ACT_NONE;
    }

    /* M4: queued data without progress. Open or advance the evidence window;
     * its origin is this first pass, not arm_us. */
    if (g->data_passes == 0) {
        g->data_since_us = in->now_us;
    }
    g->data_passes++;

    /* Criterion 5: enough evidence since the data was first seen. */
    if (g->data_passes < BFLB_F0_ARM_MIN_PASSES) {
        return BFLB_F0_ACT_NONE;
    }

    /* Criterion 4: sustained, not just "armed for a while". */
    if ((in->now_us - g->data_since_us) < BFLB_F0_ARM_MIN_US) {
        g->cnt.misfire++;
        return BFLB_F0_ACT_NONE;
    }

    if (!in->start) {
        /* Criterion 3 fails: the VDMA is no longer running although bytes are
         * still queued. That is a real stall - do not synthesise a completion
         * for a transfer the hardware already dropped, just abort so a clean
         * pass can be witnessed. */
        g->cnt.misfire++;
    } else {
        /* Full hit. */
        g->cnt.safetynet++;
    }

    g->abort_cnt++;
    g->abort_streak++;
    g->passes = 0;
    g->data_passes = 0;
    g->data_since_us = 0;

    if (g->abort_streak >= BFLB_F0_MAX_ABORTS) {
        g->state = BFLB_F0_FAULT;
        g->cnt.fault++;
        return BFLB_F0_ACT_FAULT;
    }

    g->state = BFLB_F0_RECOVERING;
    return BFLB_F0_ACT_ABORT;
}

static inline bflb_f0_action_t bflb_f0_guard_step(bflb_f0_guard_t *g,
                                                  const bflb_f0_input_t *in)
{
    if (!g || !in) {
        return BFLB_F0_ACT_NONE;
    }

    /* R6.3: guard disabled -> the whole block is a no-op. */
    if (in->guard_ep == 0) {
        g->state = BFLB_F0_IDLE;
        g->passes = 0;
        g->progress = 0;
        g->data_passes = 0;
        g->data_since_us = 0;
        return BFLB_F0_ACT_NONE;
    }

    /* Keep the bound endpoint in sync with the application's global (it is
     * written by usb_audio_register(), after this instance was statically
     * initialised). */
    g->guard_ep = in->guard_ep;

    switch (g->state) {
        case BFLB_F0_IDLE:
        case BFLB_F0_ARMED:
            /* Endpoint idle: the previous stall history is irrelevant.
             * Note that this reset only applies to IDLE/ARMED - RECOVERING
             * runs with ep_active == false by construction and must keep
             * waiting for its clean pass. */
            if (!in->armed) {
                g->state = BFLB_F0_IDLE;
                g->passes = 0;
                g->progress = 0;
                g->data_passes = 0;
                g->data_since_us = 0;
                return BFLB_F0_ACT_NONE;
            }
            if (g->state == BFLB_F0_IDLE) {
                g->state = BFLB_F0_ARMED;
                g->passes = 0;
                g->progress = 0;
                g->data_passes = 0;
                g->data_since_us = 0;
            }
            return bflb_f0_guard_armed_step(g, in);

        case BFLB_F0_RECOVERING:
            /* Rule R6.2: a synthetic completion is only legal after a pass
             * with no completion bit for this FIFO. */
            if (in->cmplt) {
                g->cnt.stale_drop++;
                return BFLB_F0_ACT_NONE;
            }
            g->state = BFLB_F0_ARMED;
            g->progress = 0;
            g->passes = 0;
            g->data_passes = 0;
            g->data_since_us = 0;
            g->arm_us = in->now_us;
            g->abort_cnt = 0;   /* successful synthetic-completion recovery */
            g->cnt.synth++;
            return BFLB_F0_ACT_SYNTH;

        case BFLB_F0_FAULT:
        default:
            return BFLB_F0_ACT_NONE;
    }
}

/* ------------------------------------------------------------------------ */
/* Cancel witness (spec 4.4)                                                */
/* ------------------------------------------------------------------------ */

/* Max time to wait for a cancelled FIFO to prove itself silent. */
#define BFLB_EP_CANCEL_WITNESS_TIMEOUT_US 20000ULL

typedef enum {
    BFLB_EP_CANCEL_NONE = 0,  /* no cancel in progress                        */
    BFLB_EP_CANCEL_PENDING,   /* cancelled, completion not yet witnessed      */
    BFLB_EP_CANCEL_SETTLED,   /* proven silent: clean G3 pass or hardware poll */
} bflb_ep_cancel_state_t;

typedef enum {
    BFLB_WITNESS_IDLE = 0,    /* nothing cancelled for this endpoint          */
    BFLB_WITNESS_PENDING,     /* late completion this pass, stay pending      */
    BFLB_WITNESS_SETTLED,     /* clean pass, cancel is now witnessed          */
} bflb_witness_result_t;

typedef struct {
    uint8_t  state;           /* bflb_ep_cancel_state_t                      */
    uint8_t  timeout_counted; /* diagnostic storm guard, one per cancel      */
    uint64_t since_us;        /* timestamp of the cancel                     */
} bflb_ep_cancel_witness_t;

/* usbd_ep_cancel(): start (or restart) the witness for one endpoint. */
static inline void bflb_ep_cancel_begin(bflb_ep_cancel_witness_t *w,
                                        uint64_t now_us)
{
    if (!w) {
        return;
    }
    w->state = BFLB_EP_CANCEL_PENDING;
    w->timeout_counted = 0;
    w->since_us = now_us;
}

/* Drop a witness without counting anything (not used by the driver: the
 * protocol must keep a pending witness alive across close/open, see spec 4.3;
 * provided for callers/tests that tear an endpoint down completely). */
static inline void bflb_ep_cancel_clear(bflb_ep_cancel_witness_t *w)
{
    if (!w) {
        return;
    }
    w->state = BFLB_EP_CANCEL_NONE;
    w->timeout_counted = 0;
    w->since_us = 0;
}

/*
 * Advance the witness with the completion bit of THIS G3 pass (sampled before
 * the group status was cleared).
 *
 *   - completion bit set   -> a late completion leaked out of the aborted
 *                             transfer: drop it (never dispatch it), count it,
 *                             and stay PENDING.
 *   - completion bit clear -> clean pass: the cancel is witnessed, SETTLED.
 *
 * The counter block is shared with the F0 guard so the driver can mirror a
 * single set of globals (g_usb_f0_stale_drop_cnt / g_usb_f0_witness_timeout).
 */
static inline bflb_witness_result_t
bflb_ep_cancel_witness_step(bflb_ep_cancel_witness_t *w,
                            bflb_f0_counters_t *cnt,
                            bool cmplt_in_pass)
{
    if (!w || w->state != BFLB_EP_CANCEL_PENDING) {
        return BFLB_WITNESS_IDLE;
    }
    if (cmplt_in_pass) {
        if (cnt) {
            cnt->stale_drop++;
        }
        return BFLB_WITNESS_PENDING;
    }
    w->state = BFLB_EP_CANCEL_SETTLED;
    return BFLB_WITNESS_SETTLED;
}

/*
 * Fail-closed diagnostic: a PENDING cancel that is still unproven after 20 ms.
 * Counts at most once per cancel episode. The driver never fabricates a
 * completion on timeout; the class layer's settle logic decides what to do.
 */
static inline bool bflb_ep_cancel_witness_timeout(bflb_ep_cancel_witness_t *w,
                                                  bflb_f0_counters_t *cnt,
                                                  uint64_t now_us)
{
    if (!w || w->state != BFLB_EP_CANCEL_PENDING) {
        return false;
    }
    if ((now_us - w->since_us) < BFLB_EP_CANCEL_WITNESS_TIMEOUT_US) {
        return false;
    }
    if (!w->timeout_counted) {
        w->timeout_counted = 1;
        if (cnt) {
            cnt->witness_timeout++;
        }
    }
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* BFLB_USB_V2_GUARD_H */
