/*
 * DS4Dongle BL618 - P0 audio session protocol core.
 *
 * Hardware independent by construction: only <stdint.h>, <stdbool.h> and
 * <string.h> plus the private port header (lock + clock + EP addresses).
 * Every public function is atomic and takes the lock itself; callers never
 * hold it, and platform callbacks run inside that critical section.
 *
 * Section references (§4.x, R#, S#) point at p0-protocol-spec-v2.md.
 */

#include <string.h>

#include "ds4_audio_proto.h"
#include "ds4_audio_proto_port.h"

/* Jitter reserve before mic streaming starts (~16 ms), R10.1. */
#define MIC_JITTER_RESERVE 256u

/* ---- helpers ---------------------------------------------------------- */

/* Effective target (R3): link down / suspend does not clear host intent, it
 * only pushes the effective target to 0 and back once the bus recovers. */
static uint8_t intf_eff(const p0_state_t *s, uint8_t idx)
{
    return (uint8_t)(s->intf[idx].desired && s->link_desired &&
                     !s->suspend_desired);
}

/* R11.3: requesting a PCM reset is a level bit, never a semaphore op. */
static void pcm_request_reset(p0_state_t *s)
{
    s->pcm_reset_pending = 1;
}

/* R8.1/R8.2: the only place the PCM ring is thrown away.  The epoch advances
 * here, so a block taken afterwards can never be mistaken for an older one. */
static void pcm_apply_reset(p0_state_t *s)
{
    s->pcm_reset_pending = 0;
    s->pcm_wr = 0;
    s->pcm_rd = 0;
    s->pcm_pending = 0;
    s->pcm_write_pos = 0;
    s->pcm_epoch++;
    s->st.pcm_reset_cnt++;
}

static uint8_t intf_ep(uint8_t idx)
{
    return (idx == P0_INTF_SPK) ? (uint8_t)P0_EP_SPK_OUT
                                : (uint8_t)P0_EP_MIC_IN;
}

/* ---- lifecycle -------------------------------------------------------- */

void p0_init(p0_state_t *s)
{
    P0_LOCK();
    memset(s, 0, sizeof(*s));
    s->mic_state = P0_MIC_IDLE;
    s->mic_tx_idx = 0;
    s->link_desired = 0;
    s->link_applied = 0;
    s->suspend_desired = 0;
    s->suspend_applied = 0;
    s->bus_epoch = 0;                 /* S2.1: one of the two writers */
    s->pcm_epoch = 0;
    P0_UNLOCK(p0_irq_key);
}

uint32_t p0_session_gen(const p0_state_t *s)
{
    P0_LOCK();
    uint32_t gen = s->session_gen;
    P0_UNLOCK(p0_irq_key);
    return gen;
}

/* ---- EP0 / bus events: intent only ------------------------------------ */

void p0_host_alt(p0_state_t *s, p0_intf_t intf, bool open)
{
    P0_LOCK();
    p0_intf_state_t *i = &s->intf[intf];

    i->desired = open ? 1 : 0;
    i->desired_seq++;
    i->desired_epoch = s->bus_epoch;

    /* R1.1/R1.2: CLOSE is effective immediately, in this same critical
     * section.  The session is invalidated, producers are frozen and the
     * in-flight transaction is abandoned (the host closed the pipe, so a
     * completion may never come).  Endpoint I/O stays deferred to service.
     * OPEN only sets intent (R1.3); it must not clear mic_frozen, because
     * the ring has not been reset yet. */
    if (intf == P0_INTF_MIC && !open) {
        s->session_gen++;
        s->mic_frozen = 1;
        if (s->mic_state == P0_MIC_INFLIGHT)
            s->mic_state = P0_MIC_IDLE;
    }

    if (!open) {
        /* A host close does not clear the fail-closed latch by itself: the
         * endpoint must first be cancelled and witnessed again.  Marking it
         * dirty forces exactly that reconstruction on the next service pass,
         * so a timeout latch cannot be bypassed by "close then open". */
        i->ep_dirty = 1;
        i->ep_dirty_reason = P0_EP_DIRTY_INVALIDATE;
    } else if (intf == P0_INTF_SPK) {
        /* A new SET_INTERFACE is the only thing that may clear an F0 fault. */
        s->spk_fault_latch = 0;
    }
    P0_UNLOCK(p0_irq_key);
}

void p0_bus_link(p0_state_t *s, bool up)
{
    P0_LOCK();
    s->link_desired = up ? 1 : 0;
    s->global_seq++;
    if (!up) {
        /* §4.3 table: a link loss ends the session now.  The endpoints are
         * closed by service; nothing here fakes a host request.
         *
         * The applied view is invalidated synchronously: link_down and
         * link_up can be coalesced between two service passes, and the
         * consumer must still rebuild the endpoints instead of observing
         * desired==applied and doing nothing. */
        for (uint8_t idx = 0; idx < P0_INTF_COUNT; idx++) {
            p0_intf_state_t *i = &s->intf[idx];
            i->applied = 0;
            i->applied_seq = i->desired_seq;
            i->settle = 0;
            i->settle_passes = 0;
            i->ep_dirty = 1;      /* the EP is still open in hardware */
            i->ep_dirty_reason = P0_EP_DIRTY_INVALIDATE;
        }
        s->session_gen++;
        s->mic_frozen = 1;
        s->mic_state = P0_MIC_IDLE;
    }
    P0_UNLOCK(p0_irq_key);
}

void p0_bus_suspend(p0_state_t *s, bool suspended)
{
    P0_LOCK();
    s->suspend_desired = suspended ? 1 : 0;
    s->global_seq++;
    if (suspended) {
        /* Same coalescing rule as link_down: suspend/resume can both land
         * before the consumer runs, so the applied view must be invalidated
         * here rather than derived from the (already restored) level. */
        for (uint8_t idx = 0; idx < P0_INTF_COUNT; idx++) {
            p0_intf_state_t *i = &s->intf[idx];
            i->applied = 0;
            i->applied_seq = i->desired_seq;
            i->settle = 0;
            i->settle_passes = 0;
            i->ep_dirty = 1;
            i->ep_dirty_reason = P0_EP_DIRTY_INVALIDATE;
        }
        s->session_gen++;
        s->mic_frozen = 1;
        s->mic_state = P0_MIC_IDLE;
    }
    P0_UNLOCK(p0_irq_key);
}

void p0_bus_reset(p0_state_t *s)
{
    P0_LOCK();
    /* §4.2: the epoch is the invalidation barrier for any intent that might
     * still be observed after the reset. */
    s->bus_epoch++;
    /* R2.2: a USB reset does not fake a Bluetooth link loss, so
     * link_desired is deliberately left alone.  Only suspend is cleared. */
    s->suspend_desired = 0;

    for (uint8_t idx = 0; idx < P0_INTF_COUNT; idx++) {
        p0_intf_state_t *i = &s->intf[idx];
        i->desired = 0;
        i->applied = 0;
        i->settle = 0;
        i->settle_passes = 0;
        i->settle_failed = 0;         /* RESET is a reconstruction point */
        i->ep_dirty = 1;              /* the EP may still be open */
        i->ep_dirty_reason = P0_EP_DIRTY_INVALIDATE;
        i->desired_seq++;
        i->applied_seq = i->desired_seq;  /* intent cleared, nothing to apply */
        i->desired_epoch = s->bus_epoch;
    }

    s->session_gen++;
    s->mic_frozen = 1;
    s->mic_ring_reset_pending = 1;
    s->mic_state = P0_MIC_IDLE;
    s->mic_cancel_issued = 0;
    s->spk_fault_latch = 0;
    s->pcm_reset_pending = 1;
    P0_UNLOCK(p0_irq_key);
}

/* ---- ISO OUT: speaker PCM -------------------------------------------- */

bool p0_pcm_write(p0_state_t *s, const int16_t *src, uint32_t samples,
                  const p0_platform_t *p)
{
    P0_LOCK();

    /* R11.1: gate first, and drop (counted) instead of touching the ring
     * when the speaker interface is not effectively open. */
    if (!intf_eff(s, P0_INTF_SPK)) {
        s->st.spk_inactive_drop++;
        P0_UNLOCK(p0_irq_key);
        return false;
    }

    /* R8.1 (producer side): apply a pending reset here when the consumer is
     * not holding a slot.  This is what breaks the startup deadlock: the
     * first packet after an open must not be rejected just because the audio
     * task has not run yet — the packet is also the only thing that can wake
     * it.  A reset that arrives while a slot is reserved is left to
     * p0_pcm_take(), which applies it at commit time (R8.2). */
    if (s->pcm_reset_pending && !s->pcm_reserved)
        pcm_apply_reset(s);

    uint32_t w = 0;
    uint32_t committed = 0;
    while (w < samples) {
        /* R8.4/R11.1: check slot ownership at every block boundary.  A
         * partially filled slot cannot be owned by the reader (pending only
         * counts committed slots), so the check is only needed at pos 0. */
        if (s->pcm_write_pos == 0 && s->pcm_pending >= P0_PCM_SLOTS) {
            s->st.pcm_drop++;
            break;                    /* drop the rest of this packet */
        }

        uint32_t pos = s->pcm_write_pos;
        uint32_t space = (uint32_t)(P0_PCM_FRAMES * 2) - pos;
        uint32_t chunk = samples - w;
        if (chunk > space)
            chunk = space;

        memcpy(&s->pcm[s->pcm_wr][pos], &src[w], chunk * sizeof(int16_t));

        pos += chunk;
        w += chunk;
        if (pos >= (uint32_t)(P0_PCM_FRAMES * 2)) {
            s->pcm_write_pos = 0;
            s->pcm_wr = (s->pcm_wr + 1) % P0_PCM_SLOTS;
            s->pcm_pending++;
            committed++;
        } else {
            s->pcm_write_pos = pos;
        }
    }

    /* R11.2: arming is a property of the endpoint state, never of the PCM
     * ring contents.  Gating it on pcm_reset_pending deadlocked the stream:
     * a reset could only be cleared by a task that needed a completed block. */
    bool can_rearm = intf_eff(s, P0_INTF_SPK) &&
                     s->intf[P0_INTF_SPK].applied &&
                     !s->intf[P0_INTF_SPK].settle;
    if (committed > 0)
        p->sem_wake();                /* P2-1: a pure wake-up hint */
    P0_UNLOCK(p0_irq_key);
    return can_rearm;
}

bool p0_pcm_take(p0_state_t *s, int16_t *dst)
{
    P0_LOCK();

    /* R8.1: a pending reset is applied at entry; the take returns false. */
    if (s->pcm_reset_pending) {
        pcm_apply_reset(s);
        P0_UNLOCK(p0_irq_key);
        return false;
    }
    if (s->pcm_pending == 0) {
        P0_UNLOCK(p0_irq_key);
        return false;
    }

    uint32_t slot = s->pcm_rd;
    s->pcm_reserved = 1;
    P0_TEST_HOOK(1);

    memcpy(dst, s->pcm[slot], (size_t)P0_PCM_FRAMES * 2 * sizeof(int16_t));

    P0_TEST_HOOK(2);

    /* R8.2: a reset that landed while the block was being copied invalidates
     * it.  Never commit rd/pending onto a ring that was already cleared. */
    if (s->pcm_reset_pending) {
        pcm_apply_reset(s);
        s->pcm_reserved = 0;
        s->st.pcm_late_take_reject++;
        P0_UNLOCK(p0_irq_key);
        return false;
    }

    s->pcm_rd = (slot + 1) % P0_PCM_SLOTS;
    s->pcm_pending--;                 /* S8.1: commit point */
    s->pcm_reserved = 0;
    P0_UNLOCK(p0_irq_key);
    return true;
}

/* ---- ISO IN: microphone ---------------------------------------------- */

void p0_mic_complete(p0_state_t *s)
{
    P0_LOCK();
    switch (s->mic_state) {
    case P0_MIC_INFLIGHT:
        /* A completion for an older session must not release a newer
         * transaction (A3), but it still ends the transaction that is
         * currently recorded. */
        if (s->mic_submit_gen != s->session_gen)
            s->st.mic_stale_cplt++;
        s->mic_state = P0_MIC_IDLE;
        break;
    case P0_MIC_CANCELING:
    case P0_MIC_FAULT:
        s->st.mic_stale_cplt++;       /* never trusted during cancel/fault */
        break;
    case P0_MIC_IDLE:
    default:
        s->st.mic_unexpected_cplt++;  /* does not change state */
        break;
    }
    P0_UNLOCK(p0_irq_key);
}

uint32_t p0_mic_publish(p0_state_t *s, const int16_t *src, uint32_t samples,
                        uint32_t gen)
{
    P0_LOCK();

    /* R5.2: frozen wins over the generation check; both are decided in the
     * same critical section that commits wr. */
    if (s->mic_frozen) {
        s->st.mic_publish_frozen_drop += samples;
        P0_UNLOCK(p0_irq_key);
        return 0;
    }
    if (gen != s->session_gen) {
        s->st.mic_publish_gen_drop += samples;
        P0_UNLOCK(p0_irq_key);
        return 0;
    }

    /* From here to UNLOCK this is one indivisible write transaction:
     * wr is read once and committed once (R5.1/R5.4). */
    uint32_t wr = s->mic_wr;
    uint32_t rd = s->mic_rd;
    uint32_t w = 0;
    while (w < samples) {
        uint32_t next = (wr + 1) % P0_MIC_RING_SAMPLES;
        if (next == rd) {
            s->st.mic_ring_full_drop += (samples - w);
            break;
        }
        s->mic_ring[wr] = src[w++];
        wr = next;
    }
    s->mic_wr = wr;                   /* the only commit point */
    P0_UNLOCK(p0_irq_key);
    return w;
}

void p0_spk_fault(p0_state_t *s)
{
    P0_LOCK();
    /* §4.6: the endpoint is unusable, so nothing is applied any more.  The
     * latch is what makes "wait for the host to reopen" real: changing
     * applied alone would let the next service pass arm the faulted FIFO
     * again, without a host SET_INTERFACE and without a fresh witness. */
    s->spk_fault_latch = 1;
    s->intf[P0_INTF_SPK].applied = 0;
    s->intf[P0_INTF_SPK].settle = 0;
    s->intf[P0_INTF_SPK].ep_dirty = 0;
    s->intf[P0_INTF_SPK].ep_dirty_reason = P0_EP_DIRTY_NONE;
    P0_UNLOCK(p0_irq_key);
}

bool p0_spk_fault_clear(p0_state_t *s)
{
    P0_LOCK();
    p0_intf_state_t *i = &s->intf[P0_INTF_SPK];

    /* M3b (fail-closed): settle_failed means the last cancel was never
     * witnessed, so the endpoint has not been proven silent and no
     * reconstruction (least of all a PHY reconnect, which destroys the
     * evidence) can be authorized. Keep the F0 latch AND the dirty state,
     * report the refusal, and let the caller latch the episode instead of
     * reconnecting. Only RESET / link / suspend / host close may clear it. */
    if (i->settle_failed) {
        s->st.spk_fault_clear_refused++;
        i->applied = 0;
        i->settle = 0;
        P0_UNLOCK(p0_irq_key);
        return false;
    }

    /* M3: the application climbed out of a terminal F0 fault with a
     * controlled USB disconnect/reconnect. The latch may go, but the endpoint
     * must still be rebuilt from scratch: ep_dirty forces a cancel+close and
     * requires a fresh witness before p0_service() may arm again.
     *
     * The dirty mark is tagged FAULT_RECOVERY, not INVALIDATE: service may
     * build the barrier but must not use this path to clear a fail-closed
     * settle latch (finding: an unconditional clear in the ep_dirty branch
     * made preserving settle_failed here meaningless). */
    s->spk_fault_latch = 0;
    i->applied = 0;
    i->settle = 0;
    i->settle_passes = 0;
    i->ep_dirty = 1;
    i->ep_dirty_reason = P0_EP_DIRTY_FAULT_RECOVERY;
    P0_UNLOCK(p0_irq_key);
    return true;
}

bool p0_spk_rebuild_allowed(const p0_state_t *s)
{
    P0_LOCK();
    bool ok = !s->intf[P0_INTF_SPK].settle_failed;
    P0_UNLOCK(p0_irq_key);
    return ok;
}

/* ---- task context ---------------------------------------------------- */

/* B2: side effects of ending a stream.  They belong to the teardown, not to
 * the level transition, because suspend/link-down invalidate `applied`
 * directly and would otherwise skip the "close" branch entirely. */
static void intf_close_effects(p0_state_t *s, uint8_t idx,
                               const p0_platform_t *p)
{
    if (idx == P0_INTF_SPK)
        pcm_request_reset(s);      /* drop queued pre-stop PCM */
    else
        p->notify_mic(false);      /* stop BT mic / keepalive activity */
}

void p0_service(p0_state_t *s, const p0_platform_t *p)
{
    P0_LOCK();

    for (uint8_t idx = 0; idx < P0_INTF_COUNT; idx++) {
        p0_intf_state_t *i = &s->intf[idx];
        const uint8_t ep = intf_ep(idx);

        /* §4.2 defensive barrier: an intent stamped before the last bus
         * reset is stale.  The reset already clears desired, so this only
         * fires if someone ever reintroduces a request queue. */
        if (i->desired_epoch != s->bus_epoch) {
            s->st.epoch_drop++;
            i->desired = i->applied;
            i->desired_seq = i->applied_seq;
            i->desired_epoch = s->bus_epoch;   /* count it once */
        }

        const uint8_t eff = intf_eff(s, idx);

        if (i->ep_dirty) {
            /* RESET / suspend / link-down / host close: the endpoint may
             * still be open even though every applied bit is 0.  Force it
             * closed, run the stream-end side effects, and require a fresh
             * witness before anything is armed again (R2.1, B1, B2).
             *
             * M3b: only an authorized reconstruction may clear the
             * fail-closed latch. A dirty mark left by p0_spk_fault_clear()
             * installs the same barrier but keeps settle_failed, so the open
             * branch below stays refused and the fault latch is preserved. */
            const bool authorized =
                (i->ep_dirty_reason != P0_EP_DIRTY_FAULT_RECOVERY);
            int cret = p->ep_cancel(ep);
            (void)p->ep_close(ep);
            i->ep_dirty = 0;
            i->ep_dirty_reason = P0_EP_DIRTY_NONE;
            i->applied = 0;
            i->settle = (cret == 0) ? 1 : 0;   /* nothing to witness if the
                                                * EP was never mapped */
            i->settle_passes = 0;
            if (authorized) {
                i->settle_failed = 0;          /* a new, host-owned barrier */
            }
            intf_close_effects(s, idx, p);
        }

        if (i->applied == eff) {
            if (i->applied == 1 && i->applied_seq != i->desired_seq) {
                /* Level did not change but the sequence advanced: a
                 * close->open flap happened while the consumer was busy, so
                 * the endpoint has to be rebuilt exactly once (R3.1). */
                (void)p->ep_cancel(ep);
                (void)p->ep_close(ep);
                i->applied = 0;
                i->settle = 1;
                intf_close_effects(s, idx, p);
            } else {
                i->applied_seq = i->desired_seq;
            }
            continue;
        }

        if (eff == 0) {               /* close */
            (void)p->ep_cancel(ep);
            (void)p->ep_close(ep);
            i->applied = 0;
            i->settle = 1;
            i->applied_seq = i->desired_seq;
            intf_close_effects(s, idx, p);
        } else {                      /* open: eff == 1 && applied == 0 */
            /* S6: a witness deadline that expired must latch, not merely
             * count: arm is refused until the host closes/reopens or RESET. */
            if (i->settle_failed)
                continue;
            /* §4.6/R6.1: an F0 fault is cleared only by a new host request. */
            if (idx == P0_INTF_SPK && s->spk_fault_latch)
                continue;

            if (i->settle) {
                /* R1.7: never arm before the cancel is witnessed. */
                if (!p->ep_cancel_settled(ep)) {
                    if (++i->settle_passes >= P0_SETTLE_MAX_PASSES) {
                        s->st.settle_timeout++;
                        i->settle = 0;
                        i->settle_failed = 1;   /* fail closed */
                    }
                    continue;         /* keep applied_seq, retry next pass */
                }
                i->settle = 0;
                i->settle_passes = 0;
            }

            if (idx == P0_INTF_SPK) {
                pcm_request_reset(s);
                if (p->ep_arm_spk() < 0) {
                    s->st.spk_arm_err++;
                    continue;         /* R3.2: do not advance applied_seq */
                }
            } else {
                /* R3.4: ring reset, then the single mic_frozen clear.  The
                 * reset happens here, so the deferred flag must NOT be left
                 * set: a later feed would replay it and discard everything
                 * published in the meantime. */
                s->session_gen++;
                s->mic_ring_reset_pending = 0;
                s->mic_wr = 0;
                s->mic_rd = 0;
                s->mic_streaming = 0;
                s->mic_frozen = 0;
                s->mic_state = P0_MIC_IDLE;
                s->mic_cancel_issued = 0;
                p->notify_mic(true);
            }
            i->applied = 1;
            i->applied_seq = i->desired_seq;
        }
    }

    /* §4.4 cancel recovery.  It runs after the interface pass so a close in
     * this same service round is already reflected in applied. */
    if (s->mic_state == P0_MIC_CANCELING) {
        if (!s->mic_cancel_issued) {
            /* Issue the abort exactly once: a witnessed settle is about the
             * cancel that was started here, so re-issuing it every pass would
             * reset the witness and it could never settle. */
            s->mic_cancel_issued = 1;
            if (p->ep_cancel(P0_EP_MIC_IN) < 0) {
                s->mic_state = P0_MIC_FAULT;
                s->st.mic_cancel_fail++;
                p->notify_mic_fault();
            }
        } else if (p->ep_cancel_settled(P0_EP_MIC_IN)) {
            /* The endpoint is silent: rebuild the ring and let feed submit
             * again if the host still has the interface open. */
            s->mic_wr = 0;
            s->mic_rd = 0;
            s->mic_streaming = 0;
            s->mic_ring_reset_pending = 0;
            s->mic_frozen = 0;
            s->mic_state = P0_MIC_IDLE;
            s->mic_cancel_issued = 0;
            s->st.mic_cancel_ok++;
        } else if (P0_NOW_US() - s->mic_cancel_us > P0_CANCEL_TIMEOUT_US) {
            /* A2: never guess.  Fail closed until the session is rebuilt. */
            s->mic_state = P0_MIC_FAULT;
            p->notify_mic_fault();
        }
    }

    P0_UNLOCK(p0_irq_key);
}

void p0_mic_feed(p0_state_t *s, const p0_platform_t *p)
{
    P0_LOCK();

    const uint64_t now = P0_NOW_US();

    if (s->mic_state == P0_MIC_CANCELING || s->mic_state == P0_MIC_FAULT)
        goto out;                     /* service owns these states */

    if (s->mic_state == P0_MIC_INFLIGHT) {
        if (now - s->mic_submit_us < P0_MIC_BUSY_TIMEOUT_US)
            goto out;
        /* §4.4 timeout: no slot is released, rd/tx_idx do not move. */
        s->mic_state = P0_MIC_CANCELING;
        s->session_gen++;
        s->mic_frozen = 1;
        s->mic_ring_reset_pending = 1;
        s->mic_cancel_issued = 0;     /* new cancel episode */
        s->mic_cancel_us = now;
        goto out;
    }

    /* I1 submit gate: every term is a level, so a change is effective at the
     * event that caused it, not at this consumer. */
    if (!s->intf[P0_INTF_MIC].desired || !s->intf[P0_INTF_MIC].applied ||
        !s->link_desired || s->suspend_desired || s->mic_frozen)
        goto out;

    if (s->mic_ring_reset_pending) {
        s->mic_ring_reset_pending = 0;
        s->mic_wr = 0;
        s->mic_rd = 0;
        s->mic_streaming = 0;
    }

    uint32_t avail = (s->mic_wr + P0_MIC_RING_SAMPLES - s->mic_rd) %
                     P0_MIC_RING_SAMPLES;
    uint32_t take = 0;

    if (!s->mic_streaming) {
        if (avail >= MIC_JITTER_RESERVE)
            s->mic_streaming = 1;
    }
    if (s->mic_streaming) {
        take = (avail < P0_MIC_PKT_SAMPLES) ? avail : P0_MIC_PKT_SAMPLES;
        if (take == 0)
            s->mic_streaming = 0;     /* reserve ran dry, buffer again */
    }

    int16_t *buf = s->mic_tx[s->mic_tx_idx];
    for (uint32_t k = 0; k < take; k++)
        buf[k] = (int16_t)s->mic_ring[(s->mic_rd + k) % P0_MIC_RING_SAMPLES];
    for (uint32_t k = take; k < P0_MIC_PKT_SAMPLES; k++)
        buf[k] = 0;                   /* R10.2: silence padding */

    s->mic_state = P0_MIC_INFLIGHT;
    s->mic_submit_gen = s->session_gen;
    s->mic_submit_us = now;

    int ret = p->submit_mic(buf, P0_MIC_PKT_SAMPLES);
    if (ret < 0) {
        /* R9.1: the samples stay in the ring and the buffer is not swapped. */
        s->mic_state = P0_MIC_IDLE;
        s->st.mic_submit_fail++;
    } else {
        s->mic_rd = (s->mic_rd + take) % P0_MIC_RING_SAMPLES;
        s->mic_tx_idx ^= 1;           /* §4.4 physical isolation */
        s->st.mic_submit_ok++;
    }

out:
    P0_UNLOCK(p0_irq_key);
}
