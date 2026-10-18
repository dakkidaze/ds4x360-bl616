/*
 * DS4Dongle BL618 — UAC1 USB audio (CherryUSB) for the DS4 v2 persona.
 *
 * This file is a thin adapter around the hardware-independent session core
 * (src/ds4_audio_proto.c). It owns descriptors, endpoint registration, the
 * ISR entry points and the platform callbacks; every piece of session and
 * buffer-ownership state lives in the core (`p0`). Nothing here keeps a
 * shadow copy of it.
 *
 * Topology (byte-identical descriptors to a real DS4 v2):
 *   IT 1 (USB streaming, 2ch) -> FU 2 (mute+volume) -> OT 3 (Headset 0x0402)
 *   IT 4 (Headset mic, 1ch)   -> FU 5 (mute+volume) -> OT 6 (USB streaming)
 *
 * Speaker ISO OUT: completed packets go straight to p0_pcm_write(), which
 * accumulates 512-frame blocks and wakes the SBC encode task through a
 * binary semaphore. The semaphore is a pure wake-up hint — p0_pcm_take() is
 * the authoritative record of which blocks are ready.
 * Mic ISO IN: the core clocks out one 32-byte packet (16 samples) per ms from
 * its own rotating TX buffers; this file only performs the endpoint write.
 * The mic Feature Unit's mute/volume are applied here, on the decoded frame,
 * before it is handed to the core (see the R15 block below).
 */

#include "ds4_usb_audio.h"
#include "ds4_audio.h"
#include "ds4_audio_proto.h"
#include "usbd_core.h"
#include "usbd_audio.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>
#include "debug_log.h"
#include "log_event.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "ds4_usb_audio_desc.h"
#include "usb_gamepad.h"
#include "spk_recovery_policy.h"

/* ---- UAC1 entity IDs (from the DS4 descriptor) ---- */
#define AUDIO_IT_SPK_ID     0x01
#define AUDIO_FU_SPK_ID     0x02
#define AUDIO_OT_SPK_ID     0x03
#define AUDIO_IT_MIC_ID     0x04
#define AUDIO_FU_MIC_ID     0x05
#define AUDIO_OT_MIC_ID     0x06

/* ---- Session core singleton ----
 * One static instance; the core serialises itself, callers never hold a lock.
 * `p0_ready` keeps p0_init() from wiping live state when early_init() is
 * called twice (main() and usb_audio_register()). */
static p0_state_t p0;
static bool       p0_ready = false;

/* ISR -> portYIELD_FROM_ISR hand-off for the binary semaphore wake-up. */
static BaseType_t p0_isr_woken = pdFALSE;

static SemaphoreHandle_t audio_sem;
static StaticSemaphore_t audio_sem_buf;

static volatile uint64_t audio_last_active_us = 0;

static uint8_t USB_NOCACHE_RAM_SECTION iso_rx_buf[DS4_AUDIO_OUT_MPS];

/* The session core keeps its rotating mic TX buffers in ordinary RAM (it is
 * hardware-independent). The VDMA reads the buffer directly, so the packet is
 * staged here, in the non-cacheable section, before submission. Two slots:
 * a cancelled transfer must never share a buffer with the next submission
 * (same reasoning as P0_MIC_TX_BUFS in the core). */
static uint8_t USB_NOCACHE_RAM_SECTION
    mic_tx_dma[P0_MIC_TX_BUFS][P0_MIC_PKT_SAMPLES * 2];
static uint8_t mic_tx_dma_idx;

/* ---- R15: mic Feature Unit 5 state ----
 * The descriptor exposes mute + volume on the mic path (EP 0x82), so the
 * adapter must keep the host-accepted values. Both are written synchronously
 * from the EP0 class callbacks (plain aligned stores) and read back by
 * GET_CUR; nothing here sends on BT or prints.
 * Defaults match the descriptor: unmuted, 0 dB (GET_MAX). */
static volatile bool mic_muted = false;
static volatile int  mic_volume_db = 0;

/* R15: the accepted volume is a real capture gain, not just a readback.
 *
 * Range. UAC1 on this persona advertises -100..0 dB at 1 dB resolution for
 * the mic Feature Unit (GET_MIN/GET_MAX/GET_RES in the SDK's usbd_audio.c),
 * i.e. only attenuation. A SET_CUR outside that span is still stored raw so
 * GET_CUR echoes it (the kernel's sticky-volume probe), but the applied gain
 * is clamped to the advertised span.
 *
 * Conversion. dB -> linear amplitude
 *     g = 10^(dB/20),   dB <= 0  =>  0 < g <= 1
 * evaluated in Q15 (unity = 32768) by repeated multiplication with the 1 dB
 * step 10^(-1/20) held in Q30 (MIC_GAIN_STEP_Q30). The factor is >= 1 LSB
 * down to -96 dB and rounds to zero at -97 dB and below; that bottom band is
 * published as silence (counted in g_audio_mic_gain_zero) rather than as a
 * gain that cannot be represented, and it is >90 dB below full scale.
 * The factor is cached and only recomputed when the host SETs a new value.
 *
 * No clipping is possible: g <= 1.0, so |out| <= |in| for every sample; the
 * int16 clamp in mic_publish_gain() is defensive only.
 *
 * Applied per sample in usb_audio_mic_publish(), i.e. in the mic decode task.
 * That path is single-context (ds4_audio_mic_task) and the cache below is
 * only touched there, so no locking is needed. Mute is checked first and
 * always wins over volume.
 *
 * Cost: one 32-bit multiply + rounding shift + clamp per sample, ~128
 * multiplies per decoded 8 ms frame (order of 1 us at 320 MHz); a gain
 * recompute is ~100 64-bit multiplies and happens only on SET_CUR. */
#define MIC_GAIN_Q15_UNITY (1u << 15)   /* 1.0 */
#define MIC_GAIN_STEP_Q30  956973408u   /* round(2^30 * 10^(-1/20)) */
#define MIC_VOL_DB_MIN     (-100)       /* advertised GET_MIN */
#define MIC_VOL_DB_MAX     0            /* advertised GET_MAX */

static uint32_t mic_gain_q15 = MIC_GAIN_Q15_UNITY; /* cached factor */
static int      mic_gain_db  = 0;                  /* dB baked into it */

/* Source for muted (and zero-gain) publications. One SBC mic frame decodes to
 * 128 samples; larger publishes are chunked so no per-frame allocation is
 * needed. */
#define MIC_PUB_CHUNK_SAMPLES 128
static const int16_t mic_mute_silence[MIC_PUB_CHUNK_SAMPLES];
/* Staging buffer for the attenuated path (mic decode task only). */
static int16_t mic_gain_buf[MIC_PUB_CHUNK_SAMPLES];

/* ---- Entity table for CherryUSB audio class ---- */
static struct audio_entity_info entity_table[] = {
    { .bEntityId = AUDIO_FU_SPK_ID,
      .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
      .ep = DS4_AUDIO_EP_OUT },
    { .bEntityId = AUDIO_FU_MIC_ID,
      .bDescriptorSubtype = AUDIO_CONTROL_FEATURE_UNIT,
      .ep = DS4_AUDIO_EP_IN },
};

static struct usbd_interface audio_intf_ctrl;
static struct usbd_interface audio_intf_stream;
static struct usbd_interface audio_intf_mic;
static struct usbd_endpoint  audio_out_ep;
static struct usbd_endpoint  audio_mic_ep;

/* Debug counters (read by the lightbar diagnostic in ds4_audio.c).
 * ISR and EP0 paths only increment them — never format or print. */
volatile uint32_t g_mic_open_cnt = 0;   /* host opened the mic interface */
volatile uint32_t g_mic_ep_tx_cnt = 0;  /* mic EP writes submitted */
volatile uint32_t g_mic_ep_err_cnt = 0; /* mic EP write failures (counted, not logged) */
volatile uint32_t g_mic_ep_done_cnt = 0;/* mic EP IN completions */
volatile uint32_t g_audio_stream_open = 0;
volatile uint32_t g_audio_stream_close = 0;
volatile uint32_t g_audio_stop_cnt = 0;
volatile uint32_t g_audio_mic_stop_cnt = 0;
volatile uint32_t g_audio_vol_cnt = 0;
volatile uint32_t g_audio_mute_cnt = 0;
volatile uint32_t g_audio_mic_gain_zero = 0; /* publish forced to silence by
                                              * the bottom of the mic range */
volatile uint32_t g_audio_arm_err = 0;
volatile uint32_t g_audio_mic_fault = 0;  /* core reported a mic endpoint fault */
volatile uint32_t g_audio_spk_fault = 0;  /* core/driver reported a speaker fault */

/* ---- M3: speaker F0 fault auto-recovery (one task, request-driven) ------
 *
 * A terminal driver FAULT used to latch until the host happened to issue
 * SET_INTERFACE (only a device switch did). That leaves the speaker dead for
 * the rest of the session. The recovery is a bounded, non-blocking state
 * machine driven from usb_audio_process() (keepalive task, 4 ms tick):
 *
 *   IDLE ──FAULT──> [>=3 faults in 10 s?] ──yes──> LATCHED (fail-closed,
 *     │                                          only a host reopen / USB
 *     │                                          reset clears the *state*)
 *     └──no──> COOLDOWN (500 ms) ──bus ok──> DISCONNECTED (100 ms) ──┐
 *                       │                        ^                  │
 *                       └ suspended / BT down /  │ request connect  │
 *                         main owns the PHY /    │ (re-check gates) │
 *                         fail-closed: stay      │                  │
 *                                                 └──> request disconnect,
 *                                                      then IDLE/LATCHED
 *
 *   ... CONNECT published -> CONNECTING ──PHY confirmed up──> IDLE
 *                                 │
 *                                 └── 250 ms retry (refused / overwritten)
 *
 * Finding 2: leaving DISCONNECTED publishes CONNECT and enters CONNECTING,
 * never IDLE. The persistent spk_reconnect_pending flag is set at publication
 * and cleared only when a usb_phy_snapshot() confirms a live PHY, so a
 * CONNECT that usb_phy_process() discards as stale cannot leave USB hidden.
 *
 * Finding 2 (two-pass interleaving): the generation that defines the attempt
 * is captured ONCE, when IDLE starts the COOLDOWN. A rejected DISCONNECT
 * publication (an external CONNECT landed in the snapshot/publication gap)
 * keeps that baseline. The next pass therefore sees a newer generation with
 * the PHY up and abandons the attempt (IDLE, obligation cleared) instead of
 * adopting the external CONNECT as a fresh baseline and publishing a second
 * DISCONNECT against it.
 *
 * Recurrence ordering (review finding 1). The latch decision is taken at the
 * top of the pass, immediately after the fault is claimed and recorded, and
 * BEFORE the pending bounce or the host epoch is consumed. Dropping a bounce
 * because the host rebuilt the endpoint therefore cannot skip the latch:
 * fault -> host reopen repeated three times ends in LATCHED, not IDLE. The
 * pure ordering lives in spk_recovery_claim_decide() /
 * spk_recovery_next_state() (spk_recovery_policy.h) and is what the host
 * test drives.
 *
 * Fail-closed revalidation (review finding 2). p0_spk_fault_clear() marks the
 * speaker endpoint dirty; a later p0_service() pass can raise settle_failed
 * while CONNECT is still unapplied. Both the CONNECT retry here and the final
 * usb_audio_phy_confirm() re-check p0_spk_rebuild_allowed(). A refusal never
 * applies a RECOVERY edge and latches the state instead. USB visibility then
 * belongs to main again: on entering BT_HID_STATE_CONNECTED (and in the
 * deferred-disconnect race) main publishes CONNECT, including when the PHY is
 * already down. The recovery keeps spk_reconnect_pending set so a main-owned
 * CONNECT is reconciled, but it never issues the unauthorized edge itself.
 *
 * Visibility handoff on LATCHED (review finding 1). "Latched" is a verdict
 * about the speaker endpoint, not permission to keep the whole persona
 * hidden. Every transition into LATCHED (history predicate, rebuild-gate
 * refusal, fault-clear refusal) calls spk_recovery_handoff_visibility(): if
 * BT is connected and the PHY is down it publishes a CONNECT owned by MAIN
 * through the same request channel. That is a visibility handoff, not a
 * RECOVERY edge: it bypasses the p0 rebuild gate on purpose, sets no
 * spk_reconnect_pending obligation, and never clears the latch. The speaker
 * stays latched until the host reopens the stream. Main additionally carries
 * an idempotent progress guarantee in its loop (BT connected + PHY down +
 * nothing pending => publish MAIN CONNECT), so visibility is restored even
 * when no BT event ever fires.
 *
 * Ownership model (review finding 3). Three rules, no exceptions:
 *
 *  1. EP0 / USB-ISR / F0-ISR callbacks are RECORDERS only. They publish a
 *     monotonic host-reconstruction epoch and a fault timestamp in an
 *     IRQ-safe step. They never write spk_recov_state, never clear history
 *     and never call the PHY. That removes the old race where an ISR "host
 *     open" reset the state machine after the task had already passed its
 *     cooldown checks, yet the interrupted task still disconnected the
 *     freshly reconstructed device.
 *  2. spk_recovery_step() is the only writer of spk_recov_state, the only
 *     caller of p0_spk_fault_clear(), and the only publisher of recovery
 *     PHY requests. It runs from exactly one task.
 *  3. It never touches the PHY itself. Every recovery connect/disconnect is
 *     a usb_phy_request(..., USB_PHY_OWNER_RECOVERY) applied by usb_task, the
 *     single owner of the UNPLUG bit, which re-validates the gates through
 *     usb_audio_phy_confirm() immediately before the write. main's BT
 *     connect / deferred-loss disconnect publish requests (owner MAIN)
 *     through the same channel, so the two writers are ordered by usb_task
 *     instead of racing each other. The single exception is the LATCHED
 *     visibility handoff, which also publishes owner MAIN: it is not a
 *     recovery edge, it carries no p0 authorisation and it never clears the
 *     latch (finding 1).
 *
 * Latch policy (review finding 4) lives in spk_recovery_policy.h: the fault
 * history decays with time only. A host open / reset / re-enumeration may
 * end the *latched state* (the host rebuilt the stream) but can never erase
 * the count, so three faults in ten seconds latch regardless of how many
 * reconnects happened in between. The 2 s self-open heuristic below is a
 * diagnostic only and gates nothing that matters.
 */
#define SPK_RECOV_COOLDOWN_MS    500u    /* settle time before touching USB   */
#define SPK_RECOV_DISCONNECT_MS  100u    /* PHY low period the host can see   */
#define SPK_RECOV_REQUEST_RETRY_MS 250u  /* re-publish a lost PHY request     */
#define SPK_RECOV_SELF_OPEN_MS   2000u   /* how long our own reconnect may
                                          * take to reach SET_INTERFACE
                                          * (diagnostics only, finding 4)    */

/* ISR -> task fault event. `valid` is the sentinel, not the timestamp: a
 * fault stamped at 0 ms (legal right after the 32-bit millisecond wrap) is a
 * real event. Neither field is ever folded to 1. Both are written under the
 * same IRQ mask by the ISR and claimed together, so the pair is consistent. */
static volatile uint32_t spk_fault_ms;
static volatile uint8_t  spk_fault_valid;
static spk_recov_state_t spk_recov_state;   /* task context only            */
static uint32_t spk_recov_due_ms;           /* task context only             */
static uint32_t spk_low_since;              /* PHY confirmed down at (ms)    */
/* Applied-edge generation captured when the recovery published its DISCONNECT.
 * While DISCONNECTED, a PHY that is up with a different generation means
 * someone else applied a CONNECT after our request: takeover, not a lost
 * disconnect (review finding 2). Task context only. */
static uint32_t spk_disc_edge_gen;
/* Attempt baseline (review finding 2, two-pass interleaving): the generation
 * captured ONCE when the attempt starts (IDLE -> COOLDOWN after a fault) and
 * preserved across every COOLDOWN pass. Re-capturing it after a rejected
 * publication would silently adopt an external CONNECT's generation as the
 * new baseline and publish a DISCONNECT against it on the following pass -
 * undoing the takeover. It is only rebased when the PHY is already down (no
 * stream to tear down). Task context only. */
static uint32_t spk_attempt_gen;
static uint8_t  spk_attempt_gen_valid;
static spk_fault_history_t spk_fault_hist;  /* task context only             */
/* Finding 3: every claimed fault is recorded here before any decision. */
static spk_fault_ledger_t  spk_fault_ledger;/* task context only             */

/* Finding 2: "the bus must become visible again" is a persistent fact, not a
 * one-shot request. Set when the recovery publishes a CONNECT; cleared only
 * when a usb_phy_snapshot() confirms the PHY is actually up. A CONNECT
 * that usb_phy_process() refuses (host reopened, main took the edge) leaves
 * it set, so SPK_RECOV_CONNECTING retries every SPK_RECOV_REQUEST_RETRY_MS.
 * Task context only (written by spk_recovery_step(), read by nothing else). */
static volatile uint8_t spk_reconnect_pending;

/* Finding 2: usb_task's final validation (usb_audio_phy_confirm) refused a
 * recovery edge because p0_spk_rebuild_allowed() turned false after the
 * decision. Single-byte writer (usb_task) / reader (recovery task); the
 * recovery task consumes it on its next pass and latches fail-closed. It
 * cannot write spk_recov_state itself: finding 3 keeps that single-writer. */
static volatile uint8_t spk_rebuild_refused;

/* Host reconstruction epoch: +1 for every host SET_INTERFACE(alt > 0) on the
 * speaker interface and every USB reset we did not cause. Recorders bump it;
 * the recovery task compares it against the value its pending decision was
 * based on. 32-bit aligned, so the read is atomic on this core. */
static volatile uint32_t spk_host_epoch;
static uint32_t spk_seen_epoch;             /* last value consumed by step()  */
static volatile uint32_t spk_decision_epoch;/* epoch of the pending request   */

static volatile bool audio_bus_suspended;
/* 1 while the SET_INTERFACE that follows one of *our* reconnects is still
 * expected. Diagnostics only (finding 4): it no longer gates the latch or
 * the history, only whether a late USB reset is attributed to us. */
static uint8_t  spk_recov_self_open;
static uint32_t spk_recov_self_open_ms;

volatile uint32_t g_audio_spk_recovery_cnt = 0;     /* controlled reconnects */
volatile uint32_t g_audio_spk_fault_episodes = 0;   /* terminal F0 faults    */
volatile uint32_t g_audio_spk_recovery_latched = 0; /* 1 = fail-closed       */
volatile uint32_t g_audio_spk_recovery_host_wins = 0; /* host beat us to it  */
/* Finding 2 diagnostics: how many recovery edges/retries the rebuild gate
 * refused (the endpoint was never proven silent), and how many times the
 * state machine actually entered LATCHED. */
volatile uint32_t g_audio_spk_recovery_refused = 0;
volatile uint32_t g_audio_spk_recovery_latched_cnt = 0;
/* Finding 2 takeover reconciliation: the PHY came up in SPK_RECOV_DISCONNECTED
 * with an applied edge newer than the recovery's DISCONNECT request (someone
 * else applied CONNECT), or a CONNECTING obligation was confirmed by an
 * applied CONNECT. Counted so a silent double-disconnect/duplicate-CONNECT
 * regression is visible. */
volatile uint32_t g_audio_spk_recovery_takeover = 0;
/* Finding 3 accounting: every claimed fault is recorded (recorded), a claim
 * pass that finds no ISR event counts as skipped. No claimed fault can be
 * dropped without being recorded. */
volatile uint32_t g_audio_spk_fault_recorded = 0;
volatile uint32_t g_audio_spk_fault_skipped = 0;

static uint32_t spk_now_ms(void)
{
    return (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
}

/* ---- recorders (EP0 / ISR): publish, never transition ------------------ */

/* Claim the pending fault event atomically, so a fault raised between the
 * read and the clear is never swallowed. Returns {at, valid}: validity is a
 * separate flag so a fault at t=0 is not lost. Also the only clearer of
 * spk_fault_ms/spk_fault_valid besides the ISR. */
static spk_fault_event_t spk_fault_event_claim(void)
{
    spk_fault_event_t ev;
    uintptr_t key = bflb_irq_save();

    ev.at_ms = spk_fault_ms;
    ev.valid = spk_fault_valid != 0;
    spk_fault_valid = 0;
    bflb_irq_restore(key);
    return ev;
}

static void spk_host_epoch_bump(void)
{
    uintptr_t key = bflb_irq_save();

    spk_host_epoch++;
    bflb_irq_restore(key);
}

/* USB reset path. A reset that belongs to our own reconnect is not a host
 * reconstruction. The flag below is written and cleared by the task only;
 * this recorder just reads it. */
static void spk_recovery_on_bus_reset(void)
{
    const uint32_t now = spk_now_ms();

    if (spk_recov_self_open &&
        (uint32_t)(now - spk_recov_self_open_ms) < SPK_RECOV_SELF_OPEN_MS) {
        return;
    }
    spk_host_epoch_bump();
}

/* Host SET_INTERFACE(alt > 0) on the speaker interface. */
static void spk_recovery_on_host_open(void)
{
    spk_host_epoch_bump();
}

/* ---- gates / request publication (task context) ------------------------ */

/* Everything the recovery must still observe before it may touch the PHY,
 * evaluated with the epoch the decision was made under. Called by the task
 * before publishing a request AND again by usb_task (via
 * usb_audio_phy_confirm) immediately before applying it. */
static bool spk_phy_gates_ok(uint32_t epoch_at_decision, bool connecting)
{
    if (audio_bus_suspended) {
        return false;                 /* persona suspended: no PHY work */
    }
    if (!ds4_audio_is_connected()) {
        return false;                 /* BT down: the persona is hidden anyway */
    }
    if (usb_phy_request_pending(USB_PHY_OWNER_MAIN)) {
        return false;                 /* main's BT path owns the next edge */
    }

    if (epoch_at_decision != spk_host_epoch) {
        /* The host reconstructed the stream after our decision.
         *  - Before a disconnect: the bounce is unnecessary, abandon it.
         *  - Before a connect: if the PHY is still up we never went down
         *    (the disconnect was refused), so do not fight the host; if the
         *    PHY is down we are the only one who can bring it back. */
        if (!connecting) {
            return false;
        }
        usb_phy_snapshot_t phy_snap;
        usb_phy_snapshot(&phy_snap);
        if (phy_snap.connected) {
            return false;
        }
    }
    return true;
}

/* Finding 1: entering LATCHED abandons the recovery bounce, but it must not
 * leave the USB persona hidden. The latch is a speaker-endpoint verdict, not
 * a USB-visibility verdict: a host that still sees the device can use the
 * gamepad, and only a host reopen can end the speaker latch. Because the
 * recovery is not authorized to issue its own CONNECT after a fail-closed
 * refusal, this is published as a MAIN-owned visibility handoff. usb_task
 * applies MAIN requests without the recovery's p0 rebuild gate, and the
 * recovery never touches the latch here: the speaker stays latched until the
 * host reopens the stream (SET_INTERFACE / USB reset bumps the epoch).
 *
 * Deliberately does not set spk_reconnect_pending: that flag describes the
 * recovery's own CONNECTING obligation, and the p0_spk_fault_clear() refusal
 * latches before it could ever be set. Main's loop carries the persistent
 * progress guarantee (any BT-connected, PHY-down, no-request-pending pass
 * republishes CONNECT), so visibility no longer depends on a BT event. */
static void spk_recovery_handoff_visibility(void)
{
    usb_phy_snapshot_t snap;

    usb_phy_snapshot(&snap);
    if (!spk_visibility_handoff_due(ds4_audio_is_connected(),
                                    snap.connected,
                                    usb_phy_request_any_pending())) {
        return;
    }
    usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_MAIN);
}

/* Single entry point for the fail-closed transition, so "entered LATCHED"
 * is counted exactly once per transition and the due time is cleared. */
static void spk_recovery_latch(void)
{
    if (spk_recov_state != SPK_RECOV_LATCHED) {
        spk_recov_state = SPK_RECOV_LATCHED;
        g_audio_spk_recovery_latched_cnt++;
        /* Visibility handoff on the transition only, from every entry path
         * (history predicate, rebuild-gate refusal, fault-clear refusal). */
        spk_recovery_handoff_visibility();
    }
    g_audio_spk_recovery_latched = 1;
    spk_recov_due_ms = 0;
    /* The attempt is over; a later IDLE -> COOLDOWN captures a new baseline. */
    spk_attempt_gen_valid = 0;
}

/* Applied by usb_task (see usb_phy_process) right before the UNPLUG write.
 *
 * Finding 2: the gate above only covers the host/BT/ownership conditions.
 * The core's fail-closed witness (settle_failed) can be raised by a
 * p0_service() pass after the recovery decided to reconnect, so it must be
 * re-validated here as well - inside the same IRQ-masked region and before
 * the register write. If the endpoint was never proven silent, refuse the
 * edge (both directions: bouncing the bus would destroy the evidence) and
 * let spk_recovery_step() latch on its next pass. The pending reconnect
 * obligation stays set, so USB visibility remains main's responsibility
 * until the PHY is actually up again. */
bool usb_audio_phy_confirm(bool connect)
{
    if (!spk_phy_gates_ok(spk_decision_epoch, connect)) {
        return false;
    }
    if (!p0_spk_rebuild_allowed(&p0)) {
        spk_rebuild_refused = 1;
        g_audio_spk_recovery_refused++;
        return false;
    }
    return true;
}

static void spk_recovery_step(void)
{
    const uint32_t now = spk_now_ms();
    const uint32_t epoch = spk_host_epoch;
    const spk_recov_state_t state_before = spk_recov_state;
    bool latched_now;

    /* Structural expiry runs at the TOP of every pass, before any early exit.
     * A fault-free pass takes no claim/decision path, so leaving expiry there
     * meant an ancient slot survived a long quiet interval and could be
     * resurrected as "recent" once the signed age clamped around 2^31 ms. It
     * is now impossible for a slot older than the window to remain valid
     * across passes. */
    spk_fault_history_expire(&spk_fault_hist, now);

    /* Findings 1 + 3: claim the ISR event and evaluate the recurrence
     * decision FIRST, while the history still owns the fault and before the
     * pending bounce or the host epoch is consumed. A host reopen may make a
     * scheduled bounce pointless, but it must not be able to veto the latch:
     * three faults in the window latch even when every bounce dies with a
     * reopen. Dropping the bounce later never un-records the fault.
     *
     * Finding 2: `now` above was sampled BEFORE the claim, so an ISR that
     * fired in the gap carries a timestamp ahead of it. Feeding that older
     * value to the history predicate makes the newest fault look like it is
     * in the future, excluding it from the count; if it was the third fault
     * the latch is skipped and no later fault-free pass reconsiders it. Sample
     * the decision time after the claim and pass the later of the two samples
     * (spk_recovery_decision_ms), so fault_at <= decision_ms by construction.
     *
     * Wrap (review finding 1): the event carries an explicit valid flag, so a
     * fault stamped 0 ms is a real fault and the decision time 0 ms is a real
     * timestamp. Nothing folds 0 to 1, so the 32-bit wrap cannot make the
     * newest fault look future-dated and the third fault always latches. */
    {
        const spk_fault_event_t fault_ev = spk_fault_event_claim();
        const uint32_t decision_ms =
            spk_recovery_decision_ms(now, spk_now_ms());
        spk_recov_state_t next =
            spk_recovery_claim_decide(&spk_fault_hist, &spk_fault_ledger,
                                      fault_ev, decision_ms,
                                      spk_recov_state);
        latched_now = (next == SPK_RECOV_LATCHED &&
                       spk_recov_state != SPK_RECOV_LATCHED);
        if (latched_now) {
            spk_recovery_latch();
        }
    }
    g_audio_spk_fault_recorded = spk_fault_ledger.recorded;
    g_audio_spk_fault_skipped  = spk_fault_ledger.skipped;

    /* Finding 2: usb_task refused a recovery edge in usb_audio_phy_confirm()
     * because p0_spk_rebuild_allowed() turned false after the decision
     * (settle_failed raised while CONNECT was still unapplied). Latch
     * fail-closed instead of retrying an unauthorized CONNECT; bouncing the
     * bus would destroy the witness evidence. The reconnect obligation stays
     * set so main's CONNECT publication can still bring the persona up. */
    if (spk_rebuild_refused) {
        spk_rebuild_refused = 0;
        spk_recovery_latch();
    }
    latched_now = (state_before != SPK_RECOV_LATCHED &&
                   spk_recov_state == SPK_RECOV_LATCHED);
    if (latched_now) {
        /* Once latched the recovery will not bounce the bus: nothing is left
         * to schedule for a fault that is already in the history. */
        spk_fault_ledger_drop_bounce(&spk_fault_ledger);
    }

    /* Finding 2: a reconnect is complete only when the PHY is confirmed up.
     * This is also the reconciliation point for ownership: whoever applied
     * the edge (recovery or main), a live PHY clears the pending state and
     * returns the machine to IDLE. */
    if (spk_reconnect_pending) {
        usb_phy_snapshot_t phy_snap;

        usb_phy_snapshot(&phy_snap);
        if (phy_snap.connected) {
            spk_reconnect_pending = 0;
            spk_attempt_gen_valid = 0;  /* the attempt reached visibility */
            /* The stream was rebuilt by us (or by main): a fault that happened
             * while the persona was hidden belongs to the old endpoint. */
            spk_fault_ledger_drop_bounce(&spk_fault_ledger);
            if (spk_recov_state == SPK_RECOV_CONNECTING) {
                /* Explicit confirm: the obligation "persona visible" is met,
                 * whoever applied the CONNECT. Counted so a takeover that had
                 * to re-publish is distinguishable from our own edge landing. */
                spk_recov_state = SPK_RECOV_IDLE;
                spk_recov_due_ms = 0;
                g_audio_spk_recovery_takeover++;
            }
        }
    }

    /* Consume host reconstructions. They end the *episode* (the host owns
     * the endpoint again) and make a scheduled bounce pointless, but they
     * never touch the recurrence history: only time decays that. A latch
     * established by this pass's fault is exempt: the reopen happened before
     * the third fault and cannot erase it. */
    if (epoch != spk_seen_epoch) {
        spk_seen_epoch = epoch;
        /* Age out the "our own reconnect" diagnostic as soon as an epoch we
         * did not record at connect time is consumed. */
        spk_recov_self_open = 0;

        if (spk_recov_state == SPK_RECOV_LATCHED && !latched_now) {
            spk_recov_state = SPK_RECOV_IDLE;
            spk_recov_due_ms = 0;
            spk_attempt_gen_valid = 0;
            g_audio_spk_recovery_latched = 0;
        } else if (spk_recov_state == SPK_RECOV_COOLDOWN) {
            /* The host reopened the stream during the cooldown: bouncing the
             * bus now would tear down a working stream. The attempt is
             * abandoned with its baseline; nothing is re-published. */
            spk_recov_state = SPK_RECOV_IDLE;
            spk_recov_due_ms = 0;
            spk_attempt_gen_valid = 0;
            g_audio_spk_recovery_host_wins++;
        }
        if (spk_recov_state == SPK_RECOV_IDLE ||
            spk_recov_state == SPK_RECOV_COOLDOWN) {
            /* Any fault event still queued belongs to the endpoint the host
             * just rebuilt; do not schedule a bounce for it. A fault that
             * survives the rebuild is re-reported by the driver guard. The
             * fault itself stays in the history (recorded at claim time). */
            spk_fault_ledger_drop_bounce(&spk_fault_ledger);
        }
    }

    switch (spk_recov_state) {
    case SPK_RECOV_IDLE: {
        spk_fault_event_t ev = spk_fault_ledger_take(&spk_fault_ledger);

        if (!ev.valid) {
            break;
        }
        g_audio_spk_fault_episodes++;

        /* Finding 2 (two-pass interleaving): capture the attempt baseline
         * ONCE, at the start of the attempt. Every COOLDOWN pass validates
         * against this generation; it is never re-captured after a rejected
         * publication, so an external CONNECT applied after our DISCONNECT
         * was queued can never become the baseline for a second DISCONNECT. */
        {
            usb_phy_snapshot_t base;

            usb_phy_snapshot(&base);
            spk_attempt_gen = base.gen;
            spk_attempt_gen_valid = 1;
        }
        /* The latch was already decided at claim time (finding 1). A bounce
         * that survived to here is below the recurrence threshold. */
        spk_recov_state = SPK_RECOV_COOLDOWN;
        spk_recov_due_ms = ev.at_ms + SPK_RECOV_COOLDOWN_MS;
        break;
    }

    case SPK_RECOV_COOLDOWN:
        if ((int32_t)(now - spk_recov_due_ms) < 0) {
            break;                      /* still cooling down */
        }
        if (!p0_spk_rebuild_allowed(&p0)) {
            /* Fail-closed: the last cancel was never witnessed, so no
             * reconstruction is authorized - and bouncing the bus would hide
             * that. Latch instead of reconnecting. */
            g_audio_spk_recovery_refused++;
            spk_recovery_latch();
            break;
        }
        if (!spk_phy_gates_ok(epoch, false)) {
            break;                      /* wait, do not count as recovery */
        }
        spk_decision_epoch = epoch;
        /* Finding 2 (two-pass interleaving): ONE snapshot per pass, validated
         * against the baseline captured when the attempt started - never
         * against a generation re-captured here. If an external CONNECT was
         * applied after the attempt began, the PHY is up for a reason that is
         * not ours. Re-publishing DISCONNECT would undo that takeover, and
         * silently rebasing to the new generation would do exactly that on
         * the next pass (the original bug). Abandon the attempt instead:
         * IDLE, reconnect obligation cleared, no second DISCONNECT. */
        {
            usb_phy_snapshot_t disc_snap;

            usb_phy_snapshot(&disc_snap);
            if (!spk_attempt_gen_valid) {
                /* Defensive: the IDLE branch always seeds the baseline. */
                spk_attempt_gen = disc_snap.gen;
                spk_attempt_gen_valid = 1;
            }
            if (disc_snap.gen != spk_attempt_gen) {
                if (disc_snap.connected) {
                    /* External takeover (host or main brought the persona
                     * up). Our DISCONNECT was never applied to this
                     * generation and must not be. */
                    spk_recov_state = SPK_RECOV_IDLE;
                    spk_recov_due_ms = 0;
                    spk_low_since = 0;
                    spk_attempt_gen_valid = 0;
                    spk_reconnect_pending = 0;
                    spk_fault_ledger_drop_bounce(&spk_fault_ledger);
                    g_audio_spk_recovery_host_wins++;
                    g_audio_spk_recovery_takeover++;
                    break;
                }
                /* An external DISCONNECT moved the edge while the PHY is
                 * down: the direction we want is already there and there is
                 * no live stream to tear down, so adopting this generation
                 * is safe (the "no external connected edge" case). */
                spk_attempt_gen = disc_snap.gen;
            }
        }
        spk_disc_edge_gen = spk_attempt_gen;
        /* Generation-validate the publication against the SAME baseline: if
         * an edge was applied between the snapshot and now, do not overwrite
         * it with a stale DISCONNECT. Keep COOLDOWN and, crucially, keep
         * spk_attempt_gen: the next pass classifies that edge (takeover when
         * the PHY is up, re-baseline when it is down) instead of mistaking
         * it for a fresh baseline. */
        if (!usb_phy_request_if_gen(USB_PHY_REQ_DISCONNECT,
                                    USB_PHY_OWNER_RECOVERY,
                                    spk_attempt_gen)) {
            break;
        }
        spk_recov_state = SPK_RECOV_DISCONNECTED;
        spk_low_since = 0;
        spk_recov_due_ms = now + SPK_RECOV_REQUEST_RETRY_MS;
        break;

    case SPK_RECOV_DISCONNECTED: {
        /* Review finding 2: ONE snapshot for the whole decision. Reading
         * "still up?" and "which generation?" separately let our own applied
         * DISCONNECT (direction down, generation bumped) be mistaken for an
         * external CONNECT. Direction, generation and timestamp below always
         * describe the same instant. */
        usb_phy_snapshot_t snap;

        usb_phy_snapshot(&snap);

        /* Wait until the PHY is really low before starting the low period:
         * the request is applied by usb_task and may be a tick or two late
         * (or refused). */
        if (snap.connected) {
            if (spk_host_epoch != spk_decision_epoch) {
                /* The disconnect was refused and the host has reconstructed
                 * the stream: nothing left to do. */
                spk_recov_state = SPK_RECOV_IDLE;
                spk_recov_due_ms = 0;
                spk_low_since = 0;
                spk_attempt_gen_valid = 0;
                g_audio_spk_recovery_host_wins++;
            } else if (spk_recovery_edge_takeover(
                           snap.connected, snap.gen, spk_disc_edge_gen)) {
                /* Finding 2 takeover reconciliation: the SAME snapshot says
                 * connected with a generation newer than our DISCONNECT
                 * baseline, so someone else (main's handoff, or the host path)
                 * applied a CONNECT after our request. Our own DISCONNECT has
                 * direction down and is handled by the low path below, never
                 * here. Treat the persona as confirmed and resolve to IDLE.
                 * Never publish a second DISCONNECT: that was the double
                 * disconnect the finding describes. */
                spk_recov_state = SPK_RECOV_IDLE;
                spk_recov_due_ms = 0;
                spk_low_since = 0;
                spk_reconnect_pending = 0;
                spk_attempt_gen_valid = 0;
                spk_fault_ledger_drop_bounce(&spk_fault_ledger);
                g_audio_spk_recovery_takeover++;
            } else if ((int32_t)(now - spk_recov_due_ms) > 0 &&
                       spk_phy_gates_ok(spk_decision_epoch, false)) {
                /* Finding 2: revalidate the witness gate before retrying the
                 * DISCONNECT as well. An unauthorized bounce destroys the
                 * very evidence the latch exists to preserve. */
                if (!p0_spk_rebuild_allowed(&p0)) {
                    g_audio_spk_recovery_refused++;
                    spk_recovery_latch();
                    break;
                }
                /* The request never reached the PHY (dropped or overwritten):
                 * publish it again instead of waiting forever. Refresh the
                 * snapshot first - usb_task may have applied an edge since the
                 * pass started. */
                usb_phy_snapshot(&snap);
                if (!snap.connected) {
                    /* Our DISCONNECT landed in the gap: the low period owns
                     * the next pass, no retry needed. */
                    break;
                }
                if (snap.gen != spk_disc_edge_gen) {
                    /* An external CONNECT was applied after our baseline:
                     * abort the retry; the next pass takes over instead of
                     * undoing that CONNECT. */
                    break;
                }
                if (!usb_phy_request_if_gen(USB_PHY_REQ_DISCONNECT,
                                            USB_PHY_OWNER_RECOVERY,
                                            spk_disc_edge_gen)) {
                    /* An edge was applied (or a request is pending) between
                     * the snapshot and publication: never overwrite it. */
                    break;
                }
                spk_recov_due_ms = now + SPK_RECOV_REQUEST_RETRY_MS;
            }
            break;
        }
        if (spk_low_since == 0) {
            spk_low_since = now;
            spk_recov_due_ms = now + SPK_RECOV_DISCONNECT_MS;
        }
        if ((int32_t)(now - spk_recov_due_ms) < 0) {
            break;
        }
        if (!spk_phy_gates_ok(spk_decision_epoch, true)) {
            break;                      /* BT/suspend/main: keep waiting */
        }
        /* Drop the protocol latch *before* the device becomes visible again
         * so the host's fresh SET_INTERFACE can arm the endpoint. Refused
         * while settle_failed is set (fail-closed): never reconnect then. */
        if (!p0_spk_fault_clear(&p0)) {
            g_audio_spk_recovery_refused++;
            spk_recovery_latch();
            break;
        }
        spk_recov_self_open = 1;
        spk_recov_self_open_ms = now;
        spk_decision_epoch = spk_host_epoch;
        /* Finding 2: publish, then wait for the PHY. A refused CONNECT may
         * have been discarded by usb_phy_process(); staying in CONNECTING
         * with the pending flag set makes the request retryable instead of
         * silently leaving USB hidden. */
        spk_reconnect_pending = 1;
        usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_RECOVERY);
        g_audio_spk_recovery_cnt++;
        spk_recov_state = SPK_RECOV_CONNECTING;
        spk_recov_due_ms = now + SPK_RECOV_REQUEST_RETRY_MS;
        break;
    }

    case SPK_RECOV_CONNECTING:
        /* CONNECT published, PHY not confirmed. usb_phy_process() may have
         * refused it as stale, main may have overwritten it, or the PHY may
         * simply not have been switched yet. Re-publish on the 250 ms retry
         * path until a usb_phy_snapshot() reports the bus up (handled at
         * the top of this pass). Never fall back to IDLE here: IDLE would
         * forget that the persona must become visible.
         *
         * Finding 2: revalidate the reconstruction authorization on every
         * pass. p0_service() can raise settle_failed while CONNECT is still
         * unapplied (deadline four passes), so a retry published here could
         * otherwise reconnect a faulted endpoint and let enumeration erase
         * the witness evidence. If it is no longer allowed, latch instead;
         * the pending obligation stays set and main remains responsible for
         * making the persona visible. */
        if (!p0_spk_rebuild_allowed(&p0)) {
            g_audio_spk_recovery_refused++;
            spk_recovery_latch();
            break;
        }
        if ((int32_t)(now - spk_recov_due_ms) >= 0 &&
            spk_phy_gates_ok(spk_host_epoch, true)) {
            spk_decision_epoch = spk_host_epoch;
            usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_RECOVERY);
            spk_recov_due_ms = now + SPK_RECOV_REQUEST_RETRY_MS;
        }
        break;

    case SPK_RECOV_LATCHED:
    default:
        break;
    }
}


/* FIFO occupancy diagnostics (FIBC1 = mic FIFO, 0x1B4). Read just before each
 * new VDMA write: 0 means the host drained the previous packet; non-zero
 * means the DCIA never put it on the bus (it accumulates up to the FIFO
 * size). */
volatile uint32_t g_mic_fifo_bc = 0;
volatile uint32_t g_mic_fifo_bc_max = 0;

#define DS4_USB_FIBC1_OFF 0x1B4u
#ifndef USB_BASE
#define USB_BASE 0x20072000u
#endif

uint32_t usb_audio_mic_fifo_bc(void)
{
    return *(volatile uint32_t *)((0x20072000u) + 0x1B4u) & 0x7FFu;
}

/* ---- Platform callbacks for the session core ----
 * Contract: called inside the core's critical section. They must not call
 * back into the core and must not block. */
static int plat_ep_cancel(uint8_t ep_addr)
{
    return usbd_ep_cancel(0, ep_addr);
}

static bool plat_ep_cancel_settled(uint8_t ep_addr)
{
    return usbd_ep_cancel_settled(0, ep_addr);
}

static int plat_ep_close(uint8_t ep_addr)
{
    return usbd_ep_close(0, ep_addr);
}

static int plat_ep_arm_spk(void)
{
    int ret = usbd_ep_start_read(0, DS4_AUDIO_EP_OUT, iso_rx_buf,
                                 sizeof(iso_rx_buf));
    if (ret < 0)
        g_audio_arm_err++;
    return ret;
}

static int plat_submit_mic(const int16_t *buf, uint32_t samples)
{
    /* FIFO occupancy snapshot before re-arming: 0 = host drained the last
     * packet, non-zero = the DCIA never transmitted it (accumulating).
     * Diagnostic counters only. */
    uint32_t bc = *(volatile uint32_t *)(USB_BASE + DS4_USB_FIBC1_OFF) & 0x7FFu;
    g_mic_fifo_bc = bc;
    if (bc > g_mic_fifo_bc_max)
        g_mic_fifo_bc_max = bc;

    g_mic_ep_tx_cnt++;

    uint32_t bytes = samples * 2u;
    if (bytes > sizeof(mic_tx_dma[0]))
        bytes = sizeof(mic_tx_dma[0]);
    uint8_t *dma = mic_tx_dma[mic_tx_dma_idx];
    mic_tx_dma_idx ^= 1;
    memcpy(dma, buf, bytes);

    int ret = usbd_ep_start_write(0, DS4_AUDIO_EP_IN, dma, bytes);
    if (ret < 0)
        g_mic_ep_err_cnt++;   /* hot path: count, never print */
    return ret;
}

static void plat_sem_wake(void)
{
    /* Binary semaphore: a failed give only means a wake-up is already queued.
     * The caller (audio_ep_out_handler) performs portYIELD_FROM_ISR(). */
    if (audio_sem)
        xSemaphoreGiveFromISR(audio_sem, &p0_isr_woken);
}

static void plat_notify_mic(bool on)
{
    ds4_audio_set_mic_active(on);
}

static void plat_notify_mic_fault(void)
{
    g_audio_mic_fault++;
    log_event_put(LOG_EVT_AUDIO_STREAM, 5, 0);
}

/* Called by the core from inside its critical section: counters/event only.
 * The endpoint state transition is done by usbd_event_f0_fault_handler(),
 * which runs in the driver ISR outside any core call. */
static void plat_notify_spk_fault(void)
{
    g_audio_spk_fault++;
    log_event_put(LOG_EVT_AUDIO_STREAM, 4, 0);
}

static const p0_platform_t plat = {
    .ep_cancel          = plat_ep_cancel,
    .ep_cancel_settled  = plat_ep_cancel_settled,
    .ep_close           = plat_ep_close,
    .ep_arm_spk         = plat_ep_arm_spk,
    .submit_mic         = plat_submit_mic,
    .sem_wake           = plat_sem_wake,
    .notify_mic         = plat_notify_mic,
    .notify_mic_fault   = plat_notify_mic_fault,
    .notify_spk_fault   = plat_notify_spk_fault,
    .test_hook          = NULL,   /* host tests only */
};

/* ---- ISO OUT endpoint callback (ISR context) ----
 * The core decides whether this packet belongs to an open speaker stream and
 * whether the endpoint may be re-armed; a false return must leave the EP
 * unarmed (S1: a cancelled stream must never be revived here). */
static void audio_ep_out_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)ep;

    if (nbytes != 0)
        audio_last_active_us = bflb_mtimer_get_time_us();

    p0_isr_woken = pdFALSE;
    /* ISO OUT is stereo 16-bit: one sample per int16_t, nbytes / 2 samples. */
    bool rearm = p0_pcm_write(&p0, (const int16_t *)iso_rx_buf, nbytes / 2u,
                              &plat);
    if (rearm)
        usbd_ep_start_read(busid, DS4_AUDIO_EP_OUT, iso_rx_buf,
                           sizeof(iso_rx_buf));
    if (p0_isr_woken)
        portYIELD_FROM_ISR(p0_isr_woken);
}

/* ---- Mic EP IN completion (ISR context) ---- */
static void audio_mic_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    g_mic_ep_done_cnt++;   /* ISR context: counters only, no printf */
    p0_mic_complete(&p0);
}

/* 1 ms feed tick (task context): the core owns the submit gate. */
void usb_audio_mic_feed(void)
{
    p0_mic_feed(&p0, &plat);
}

/* ---- CherryUSB weak callback overrides ---- */

void usbd_audio_open(uint8_t busid, uint8_t intf)
{
    (void)busid;
    /* EP0 context: count + event only. The core records the intent now and
     * resolves it (endpoint open/arm) in p0_service(). */
    if (intf == DS4_AUDIO_INTF_STREAM) {
        g_audio_stream_open++;
        log_event_put(LOG_EVT_AUDIO_STREAM, 0, 0);
        /* EP0 recorder: publish the reconstruction epoch; the keepalive task
         * ends the latched episode on its next pass. The recurrence history
         * is NOT touched (finding 4) - only time decays it. The core's own
         * fail-closed latch is released by p0_host_alt() below, but that is
         * a host-owned reconstruction and still has to pass the witness
         * gate before anything can arm. */
        spk_recovery_on_host_open();
        p0_host_alt(&p0, P0_INTF_SPK, true);
    } else if (intf == DS4_AUDIO_INTF_MIC) {
        g_audio_stream_open++;
        log_event_put(LOG_EVT_AUDIO_STREAM, 2, 0);
        g_mic_open_cnt++;
        p0_host_alt(&p0, P0_INTF_MIC, true);
    }
}

void usbd_audio_close(uint8_t busid, uint8_t intf)
{
    (void)busid;
    /* S1.1: no endpoint call in this path, ever. p0_host_alt() invalidates the
     * session synchronously, so the feed cannot submit after this returns;
     * the actual EP close is deferred to p0_service(). */
    if (intf == DS4_AUDIO_INTF_STREAM) {
        g_audio_stream_close++;
        log_event_put(LOG_EVT_AUDIO_STREAM, 1, 0);
        p0_host_alt(&p0, P0_INTF_SPK, false);
    } else if (intf == DS4_AUDIO_INTF_MIC) {
        g_audio_stream_close++;
        log_event_put(LOG_EVT_AUDIO_STREAM, 3, 0);
        p0_host_alt(&p0, P0_INTF_MIC, false);
    }
}

/* Called from task context (keepalive task): applies every deferred action
 * (endpoint cancel/close/arm, mic notify) derived from the published intent. */
void usb_audio_process(void)
{
    p0_service(&p0, &plat);
    /* M3: advance the F0 fault recovery machine (task context, non-blocking).
     * Runs after service so a cleared latch is picked up by the next pass. */
    spk_recovery_step();
}

void usbd_audio_set_volume(uint8_t busid, uint8_t ep, uint8_t ch, int volume_db)
{
    (void)busid; (void)ch;
    if (ep == DS4_AUDIO_EP_IN) {
        /* R15: authoritative readback for the mic Feature Unit. The stored
         * value is the raw accepted one (GET_CUR must agree synchronously);
         * usb_audio_mic_publish() turns it into a per-sample gain, clamped to
         * the advertised -100..0 dB span. */
        mic_volume_db = volume_db;
        g_audio_vol_cnt++;   /* EP0 path: counter only */
        return;
    }
    /* Host dB (advertised -100..0) -> DS4 amp byte via the measured curve */
    ds4_audio_push_speaker_volume(volume_db);
}

void usbd_audio_set_mute(uint8_t busid, uint8_t ep, uint8_t ch, bool mute)
{
    (void)busid; (void)ch;
    if (ep == DS4_AUDIO_EP_IN) {
        /* R15: applied by usb_audio_mic_publish() when the decoded frame is
         * published to the USB mic ring. */
        mic_muted = mute;
        g_audio_mute_cnt++;   /* EP0 path: counter only */
        return;
    }
    ds4_audio_push_speaker_mute(mute);
    g_audio_mute_cnt++;
}

int usbd_audio_get_volume(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid; (void)ch;
    if (ep == DS4_AUDIO_EP_IN)
        return mic_volume_db;
    /* Echo the host-set value: the Linux kernel probes with a SET_CUR/GET_CUR
     * round-trip and flags the mixer as "sticky" when readback differs. */
    return ds4_audio_get_speaker_volume();
}

bool usbd_audio_get_mute(uint8_t busid, uint8_t ep, uint8_t ch)
{
    (void)busid; (void)ch;
    if (ep == DS4_AUDIO_EP_IN)
        return mic_muted;
    /* The class default is a weak "always false", which made GET disagree
     * with the speaker SET as well; answer from the accepted state. */
    return ds4_audio_get_speaker_mute();
}

uint32_t usbd_audio_get_sampling_freq(uint8_t busid, uint8_t ep)
{
    (void)busid;
    return (ep == DS4_AUDIO_EP_IN) ? 16000 : 32000;
}

/* ---- Public API ---- */

void usb_audio_early_init(void)
{
    if (!audio_sem)
        audio_sem = xSemaphoreCreateBinaryStatic(&audio_sem_buf);
    if (!p0_ready) {
        p0_init(&p0);
        /* Empty recurrence history. Runtime code has no way to empty it
         * again: only time decays an entry (spk_recovery_policy.h). */
        spk_fault_history_init(&spk_fault_hist);
        spk_fault_ledger_init(&spk_fault_ledger);
        p0_ready = true;
    }
}

/* Diagnostic: the DCIA re-maps all four FIFOs on every usbd_ep_open, but we
 * only use indexes 1/2/4 (audio OUT, mic IN, gamepad IN), leaving FIFO F2
 * (index 3) mapped but never configured. The DS5Dongle uses all four and its
 * ISO IN mic works, so configure index 3 with an EP the host never sees. */
static const struct usb_endpoint_descriptor dummy_ep3 = {
    .bLength = 7,
    .bDescriptorType = 0x05,          /* ENDPOINT */
    .bEndpointAddress = 0x83,         /* EP3 IN -> FIFO F2 */
    .bmAttributes = 0x03,             /* interrupt */
    .wMaxPacketSize = 8,
    .bInterval = 10,
};

void usb_audio_register(uint8_t busid)
{
    usb_audio_early_init();

    /* F0 safety net: only guard the ISO OUT endpoint this application
     * actually owns (the driver skips the block entirely at 0). A guard
     * failure surfaces through usbd_event_f0_fault_handler() below. */
    g_usb_f0_guard_ep = DS4_AUDIO_EP_OUT & 0x7F;

    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_ctrl, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));
    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_stream, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));
    usbd_add_interface(busid, usbd_audio_init_intf(
        busid, &audio_intf_mic, 0x0100, entity_table,
        sizeof(entity_table) / sizeof(entity_table[0])));

    audio_out_ep.ep_addr = DS4_AUDIO_EP_OUT;
    audio_out_ep.ep_cb = audio_ep_out_handler;
    usbd_add_endpoint(busid, &audio_out_ep);

    audio_mic_ep.ep_addr = DS4_AUDIO_EP_IN;
    audio_mic_ep.ep_cb = audio_mic_ep_in_handler;
    usbd_add_endpoint(busid, &audio_mic_ep);

    usbd_ep_open(busid, &dummy_ep3);   /* configure FIFO F2 (index 3) */

    LOG_INF("[AUDIO] UAC1 interfaces registered (DS4 spk+mic)\n");
}

const uint8_t *usb_audio_get_desc(uint16_t *len)
{
    if (len) *len = DS4_AUDIO_DESC_SIZE;
    return audio_desc;
}

bool usb_audio_read(int16_t *out)
{
    return p0_pcm_take(&p0, out);
}

void *usb_audio_get_semaphore(void)
{
    return (void *)audio_sem;
}

bool usb_audio_speaker_streaming(void)
{
    /* Host intent, kept across a Bluetooth drop (R03) just like the old
     * stream_active flag: the core owns it now. */
    return p0.intf[P0_INTF_SPK].desired != 0;
}

/* Q15 linear factor for an accepted dB value (see the R15 block above).
 * `db` is clamped to the advertised -100..0 range; the result is 0 only for
 * the bottom few dB, where Q15 cannot represent the factor. */
static uint32_t mic_gain_for_db(int db)
{
    if (db >= MIC_VOL_DB_MAX)
        return MIC_GAIN_Q15_UNITY;
    if (db < MIC_VOL_DB_MIN)
        db = MIC_VOL_DB_MIN;

    uint64_t g = (uint64_t)1 << 30;          /* 1.0 in Q30 */
    for (int i = 0; i < -db; i++)
        g = (g * MIC_GAIN_STEP_Q30) >> 30;

    return (uint32_t)((g + (1u << 14)) >> 15);   /* Q30 -> Q15, rounded */
}

/* Publish `count` samples through the core as MIC_PUB_CHUNK_SAMPLES pieces.
 * Shared by the muted and the zero-gain path so both keep the unmuted
 * chunking and the "return accepted count" semantics. */
static uint32_t mic_publish_silence(uint32_t count, uint32_t gen)
{
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > MIC_PUB_CHUNK_SAMPLES)
            chunk = MIC_PUB_CHUNK_SAMPLES;
        uint32_t n = p0_mic_publish(&p0, mic_mute_silence, chunk, gen);
        done += n;
        if (n != chunk)
            break;   /* ring full or session generation dropped it */
    }
    return done;
}

/* Publish `count` samples scaled by `gain` (Q15, <= MIC_GAIN_Q15_UNITY).
 * Same chunking and partial-accept behaviour as mic_publish_silence(). */
static uint32_t mic_publish_gain(const int16_t *samples, uint32_t count,
                                 uint32_t gen, uint32_t gain)
{
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > MIC_PUB_CHUNK_SAMPLES)
            chunk = MIC_PUB_CHUNK_SAMPLES;
        for (uint32_t i = 0; i < chunk; i++) {
            /* g <= 1.0, so the product fits int32 and cannot exceed |in|;
             * the clamp is defensive only. */
            int32_t v = ((int32_t)samples[done + i] * (int32_t)gain + 16384) >> 15;
            if (v > INT16_MAX)
                v = INT16_MAX;
            else if (v < INT16_MIN)
                v = INT16_MIN;
            mic_gain_buf[i] = (int16_t)v;
        }
        uint32_t n = p0_mic_publish(&p0, mic_gain_buf, chunk, gen);
        done += n;
        if (n != chunk)
            break;   /* ring full or session generation dropped it */
    }
    return done;
}

uint32_t usb_audio_mic_publish(const int16_t *samples, uint32_t count,
                               uint32_t gen)
{
    /* R15: take one Feature Unit snapshot per frame. A SET_CUR landing
     * mid-frame then applies from the next frame on, so a frame is never
     * half-scaled. */
    bool muted = mic_muted;
    int  db    = mic_volume_db;

    /* Mute wins over volume and publishes silence rather than dropping the
     * frame. The ring then keeps the same fill/drain behaviour as the
     * unmuted path (the core still gets whole 8 ms frames and still drains
     * 16 samples per 1 ms feed), so packet pacing and the underrun-padding
     * path are unchanged; only the sample values are zeroed. The session
     * generation check stays inside p0_mic_publish(), so stale frames are
     * still dropped. */
    if (muted)
        return mic_publish_silence(count, gen);

    /* Advertised maximum (and any out-of-range positive dB): unity, and the
     * original zero-copy publish path. */
    if (db >= MIC_VOL_DB_MAX)
        return p0_mic_publish(&p0, samples, count, gen);

    /* Only the publish path touches the cache, so it needs no locking. */
    if (db != mic_gain_db) {
        mic_gain_db  = db;
        mic_gain_q15 = mic_gain_for_db(db);
    }
    if (mic_gain_q15 == 0) {
        g_audio_mic_gain_zero++;   /* bottom of the range: true silence */
        return mic_publish_silence(count, gen);
    }
    return mic_publish_gain(samples, count, gen, mic_gain_q15);
}

bool usb_audio_mic_is_active(void)
{
    return p0.intf[P0_INTF_MIC].desired != 0;
}

uint32_t usb_audio_mic_session_gen(void)
{
    return p0_session_gen(&p0);
}

/* Explicit teardown: publish "host no longer wants this interface". The core
 * performs the endpoint cancel/close on its next p0_service() pass. */
void usb_audio_stop(void)
{
    g_audio_stop_cnt++;
    p0_host_alt(&p0, P0_INTF_SPK, false);
    ds4_audio_set_speaker_streaming(false);
}

void usb_audio_mic_stop(void)
{
    g_audio_mic_stop_cnt++;
    p0_host_alt(&p0, P0_INTF_MIC, false);
}

/* R08: SUSPEND stops all endpoint I/O but keeps the host's alt state, so a
 * RESUME can restore it without the host re-issuing SET_INTERFACE. */
void usb_audio_suspend(void)
{
    audio_bus_suspended = true;
    p0_bus_suspend(&p0, true);
}

void usb_audio_resume(void)
{
    audio_bus_suspended = false;
    p0_bus_suspend(&p0, false);
}

/* R03: Bluetooth link lost/restored. The host's alt state is intentionally
 * NOT touched here — only the link layer. */
void usb_audio_link_down(void)
{
    p0_bus_link(&p0, false);
}

void usb_audio_link_up(void)
{
    p0_bus_link(&p0, true);
}

/* USB RESET / re-enumeration: the host will re-open everything, so drop all
 * host intent as well as the endpoint and buffer state. */
void usb_audio_reset(void)
{
    audio_bus_suspended = false;
    /* ISR recorder: publish the host reconstruction. The M3 *state* is
     * consumed by the keepalive task (a host reset ends a latched episode);
     * the recurrence history survives - it only decays with time. */
    spk_recovery_on_bus_reset();
    p0_bus_reset(&p0);
}

bool usb_audio_is_active(void)
{
    if (p0.intf[P0_INTF_SPK].desired)
        return true;
    if (audio_last_active_us == 0)
        return false;
    uint64_t elapsed = bflb_mtimer_get_time_us() - audio_last_active_us;
    return elapsed < 5000000ULL;
}

/* ---- Driver F0 guard fault hook (ISR context) ----
 * The VDMA safety net gave up recovering the speaker OUT endpoint. The fault
 * is a visible event, not a silent disable: one SET_INTERFACE from the host
 * rebuilds the stream. Counters/event only — no printf here. */
void usbd_event_f0_fault_handler(uint8_t busid, uint8_t ep)
{
    (void)busid; (void)ep;
    g_audio_spk_fault++;
    log_event_put(LOG_EVT_AUDIO_STREAM, 4, 0);

    /* M3: hand the recovery to the keepalive task. Only a timestamp is
     * recorded here (ISR context: no USB calls, no delays, no 64-bit store
     * that could tear across the ISR/task boundary). "No event" is carried by
     * spk_fault_valid, NOT by the value 0: at the 32-bit millisecond wrap a
     * genuine fault can be stamped 0, and folding it to 1 made it look
     * future-dated to the history predicate (review finding 1). The task
     * claims the {timestamp, valid} pair under the same mask
     * (spk_recovery_step). */
    {
        uint32_t at = spk_now_ms();
        uintptr_t key = bflb_irq_save();
        spk_fault_ms = at;
        spk_fault_valid = 1;
        bflb_irq_restore(key);
    }
    p0_spk_fault(&p0);
}
