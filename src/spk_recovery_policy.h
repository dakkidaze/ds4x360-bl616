/*
 * spk_recovery_policy.h - pure recurrence/latch policy for the speaker F0
 * auto-recovery (M3), host-testable.
 *
 * Header-only, static inline, zero SDK dependencies (only <stdint.h> and
 * <stdbool.h>), same style as the driver's bflb_usb_v2_guard.h:
 *
 *     gcc -std=c11 -Wall -Wextra -fsyntax-only -Isrc <tu.c>
 *
 * Why this lives outside ds4_usb_audio.c: the original latch counted faults
 * in a three-slot ring but erased the ring whenever a host SET_INTERFACE, USB
 * reset or "self open" was classified as a genuine reconstruction. That made
 * the latch defeatable: a host that reopened the stream between faults (or a
 * reconnect whose SET_INTERFACE arrived more than two seconds late, so the
 * short enumeration heuristic did not recognise it as ours) could keep the
 * count below three forever.
 *
 * The policy below has exactly one decay function: TIME. There is
 * deliberately no reset/clear entry point, so host opens, reconnects and
 * re-enumerations cannot influence the count. Three faults inside the window
 * latch, full stop.
 *
 * Timestamps are monotonic 32-bit milliseconds. The clock wraps every
 * ~49.7 days, so the value 0 is a legal timestamp: it is NOT a sentinel.
 * "No event" is carried by an explicit validity flag (spk_fault_event_t.valid
 * and spk_fault_history_t.valid_mask). Nothing folds 0 to 1 anywhere: folding
 * made a real fault stamped at 0 indistinguishable from "empty", and at the
 * wrap boundary the folded 1 looked future-dated relative to a decision time
 * of 0, silently dropping the third fault from the count.
 *
 * All age comparisons use a signed 32-bit difference:
 *     age = (int32_t)(now_ms - at_ms)
 * The invariant is fault_at_ms <= decision_ms for every event a claim can
 * observe, so age is normally >= 0. A negative age means the event was stamped
 * after the decision sample (a claim that raced the ISR); it is clamped to 0
 * and still counted, which is both wrap-safe and fail-closed: a fault can
 * never escape the window by looking "future-dated". Spans of interest are
 * milliseconds against a 10 s window, far below 2^31 ms, so the signed
 * representation is unambiguous.
 *
 * Structural expiry: a slot that leaves the window has its valid bit cleared
 * on the next record, decision, or recovery-step pass (the step expires at
 * the top, before any early exit). The ring is therefore empty long before the
 * signed difference becomes ambiguous at 2^31 ms, and an ancient timestamp
 * can never be resurrected as "recent".
 */
#ifndef SPK_RECOVERY_POLICY_H
#define SPK_RECOVERY_POLICY_H

#include <stdint.h>
#include <stdbool.h>

/* Faults inside this window latch the episode (fail-closed). */
#define SPK_RECOV_WINDOW_MS     10000u
/* >= this many faults inside the window: latch. */
#define SPK_RECOV_MAX_IN_WINDOW 3u

typedef struct {
    uint32_t at_ms[SPK_RECOV_MAX_IN_WINDOW];
    uint8_t  valid_mask;          /* bit i: at_ms[i] holds a recorded fault */
    uint8_t  idx;                 /* next slot to overwrite */
} spk_fault_history_t;

/* A fault event carries its timestamp and an explicit validity flag, so a
 * fault at t=0 is distinguishable from "the ISR published nothing". */
typedef struct {
    uint32_t at_ms;
    bool     valid;
} spk_fault_event_t;

static inline spk_fault_event_t spk_fault_event(uint32_t at_ms)
{
    spk_fault_event_t e;

    e.at_ms = at_ms;
    e.valid = true;
    return e;
}

static inline spk_fault_event_t spk_fault_event_none(void)
{
    spk_fault_event_t e;

    e.at_ms = 0;
    e.valid = false;
    return e;
}

/* Only ever called once, at init: an empty history. Runtime code has no way
 * to empty it again on purpose. */
static inline void spk_fault_history_init(spk_fault_history_t *h)
{
    if (!h) {
        return;
    }
    for (uint8_t i = 0; i < SPK_RECOV_MAX_IN_WINDOW; i++) {
        h->at_ms[i] = 0;
    }
    h->valid_mask = 0;
    h->idx = 0;
}

/* Is a recorded fault still inside the window at now_ms?
 *
 * Signed difference: a fault is recent when 0 <= age < window, where a
 * negative age (event stamped after the decision sample) is clamped to 0 and
 * therefore counts. This is the wrap-safe form: at the 32-bit ms wrap a claim
 * at UINT32_MAX with decision time 0 has age +1 and a claim at 0 with decision
 * time 0 has age 0 - neither is excluded, which the unsigned form did for the
 * folded 0 -> 1 timestamp. */
static inline bool spk_fault_slot_recent(uint32_t at_ms, uint32_t now_ms)
{
    int32_t age = (int32_t)(now_ms - at_ms);

    if (age < 0) {
        age = 0;
    }
    return (uint32_t)age < SPK_RECOV_WINDOW_MS;
}

/* Structural expiry (non-blocking cleanup): the ring has no other way to
 * forget a slot, so without this an ancient timestamp could be resurrected
 * as "recent" once the 32-bit ms clock wraps far enough for the signed
 * difference to clamp to 0. Any slot outside the window at now_ms has its
 * valid bit cleared, permanently, so it can never be counted again. Slots
 * stamped "in the future" (negative age) are recent by the predicate and are
 * left alone. Called on record, on every decision, and at the TOP of every
 * spk_recovery_step() pass - including fault-free passes, which take no
 * record/decision path. That last caller is what guarantees a long quiet
 * interval cannot leave a stale slot behind until the wrap. */
static inline void spk_fault_history_expire(spk_fault_history_t *h,
                                            uint32_t now_ms)
{
    if (!h) {
        return;
    }
    for (uint8_t i = 0; i < SPK_RECOV_MAX_IN_WINDOW; i++) {
        if ((h->valid_mask & (1u << i)) &&
            !spk_fault_slot_recent(h->at_ms[i], now_ms)) {
            h->valid_mask = (uint8_t)(h->valid_mask & ~(1u << i));
        }
    }
}

/* Record one fault at at_ms (monotonic ms; 0 is a valid timestamp and is
 * stored verbatim). Slots that already fell out of the window are retired
 * first, so recording cannot resurrect them. */
static inline void spk_fault_history_record(spk_fault_history_t *h,
                                            uint32_t at_ms)
{
    if (!h) {
        return;
    }
    spk_fault_history_expire(h, at_ms);
    h->at_ms[h->idx] = at_ms;
    h->valid_mask = (uint8_t)(h->valid_mask | (1u << h->idx));
    h->idx = (uint8_t)((h->idx + 1u) % SPK_RECOV_MAX_IN_WINDOW);
}

/* Faults that are still inside the window at now_ms (the one just recorded
 * counts). Purely a function of the timestamps and now_ms, plus the
 * structural expiry of slots that have aged out (the table is mutable so the
 * expiry is durable, not just filtered on the fly). */
static inline uint8_t spk_fault_history_count(spk_fault_history_t *h,
                                              uint32_t now_ms)
{
    uint8_t recent = 0;

    if (!h) {
        return 0;
    }
    spk_fault_history_expire(h, now_ms);
    for (uint8_t i = 0; i < SPK_RECOV_MAX_IN_WINDOW; i++) {
        if (h->valid_mask & (1u << i)) {
            recent++;
        }
    }
    return recent;
}

/* The latch decision: >= SPK_RECOV_MAX_IN_WINDOW faults within
 * SPK_RECOV_WINDOW_MS, independent of any open/reconnect in between. */
static inline bool spk_fault_history_should_latch(spk_fault_history_t *h,
                                                  uint32_t now_ms)
{
    return spk_fault_history_count(h, now_ms) >= SPK_RECOV_MAX_IN_WINDOW;
}

/* ---- fault event claim/record ledger (review finding 3) ----
 *
 * The ISR publishes one timestamp per fault and the task claims it. The old
 * consumer claimed the timestamp *after* deciding whether the scheduled
 * bounce was still relevant, so a "fault -> host reopen -> task pass"
 * sequence threw the fault away before it reached the history: three genuine
 * stalls separated by host reopens recorded zero faults.
 *
 * This ledger fixes the ordering by construction: spk_fault_ledger_claim()
 * records the fault into the history at the moment the event is claimed, and
 * is the only entry point that may consume an ISR timestamp. Dropping the
 * bounce (because the host rebuilt the endpoint) only clears pending_at; it
 * cannot touch the history. That is why the API is split in two and why
 * there is no "unrecord" call:
 *
 *     claim(ev)  -> if ev.valid, history gets ev.at_ms exactly once and the
 *                   pending slot is armed with it
 *     take()     -> the pending event, for the bounce decision
 *     drop_bounce() -> forget the bounce, keep the recorded fault
 *
 * Counters: `recorded` counts claimed events appended to the history,
 * `skipped` counts claim calls that found no event (ev.valid == false). No
 * claimed event can be skipped, so recorded + skipped == claim calls.
 *
 * The pending slot is a spk_fault_event_t, not a bare uint32_t: an event
 * stamped at 0 ms (legitimate right after the 32-bit wrap) is still a real
 * pending bounce.
 *
 * Header-only and SDK-free like the history above, so the exact ordering the
 * firmware uses is compiled and asserted by the host test (T23/T26). */
typedef struct {
    spk_fault_event_t pending;  /* claimed event not yet turned into a bounce */
    uint32_t recorded;          /* events appended to the history             */
    uint32_t skipped;           /* claim calls with no event                  */
} spk_fault_ledger_t;

static inline void spk_fault_ledger_init(spk_fault_ledger_t *l)
{
    if (!l) {
        return;
    }
    l->pending = spk_fault_event_none();
    l->recorded = 0;
    l->skipped = 0;
}

/* Claim one ISR event. Must be called before any consumer decision (epoch
 * consumption, host-reopen handling, state switch). Records the fault
 * unconditionally; returns false only when there was no event at all. */
static inline bool spk_fault_ledger_claim(spk_fault_ledger_t *l,
                                          spk_fault_history_t *h,
                                          spk_fault_event_t ev)
{
    if (!l) {
        return false;
    }
    if (!ev.valid) {
        l->skipped++;
        return false;
    }
    spk_fault_history_record(h, ev.at_ms);
    l->recorded++;
    if (!l->pending.valid) {
        l->pending = ev;          /* earliest unhandled fault */
    }
    return true;
}

/* Earliest claimed fault not yet turned into a bounce (valid == false means
 * none). */
static inline spk_fault_event_t spk_fault_ledger_pending(
    const spk_fault_ledger_t *l)
{
    return l ? l->pending : spk_fault_event_none();
}

/* Consume the pending fault for scheduling. The history already owns it. */
static inline spk_fault_event_t spk_fault_ledger_take(spk_fault_ledger_t *l)
{
    spk_fault_event_t ev;

    if (!l) {
        return spk_fault_event_none();
    }
    ev = l->pending;
    l->pending = spk_fault_event_none();
    return ev;
}

/* The host rebuilt the endpoint: the bounce is pointless, the fault is not.
 * Deliberately does not touch the history or the counters. */
static inline void spk_fault_ledger_drop_bounce(spk_fault_ledger_t *l)
{
    if (l) {
        l->pending = spk_fault_event_none();
    }
}

/* ---- recovery states + recurrence ordering (review finding 1) ----------
 *
 * The driver's state machine used to evaluate the latch only inside its IDLE
 * branch, after the pending bounce had already been consumed. A host reopen
 * between every fault therefore dropped the bounce, skipped the only latch
 * check, and three genuine faults left the machine in IDLE even though the
 * history said three.
 *
 * The recurrence decision is now a separate pure step evaluated immediately
 * after the fault is claimed/recorded and BEFORE the pending bounce or the
 * host epoch is consumed. `pending_bounce` is passed in on purpose and
 * deliberately ignored: whether the scheduled bounce still looks relevant
 * must not be able to veto the latch.
 *
 * The state values live here (not in the .c) so the host test drives the same
 * enum the firmware uses. */
typedef enum {
    SPK_RECOV_IDLE = 0,
    SPK_RECOV_COOLDOWN,
    SPK_RECOV_DISCONNECTED,
    SPK_RECOV_CONNECTING,
    SPK_RECOV_LATCHED,
} spk_recov_state_t;

/* Pure recurrence transition. Returns LATCHED when the history says so;
 * otherwise the state is unchanged. */
static inline spk_recov_state_t
spk_recovery_next_state(spk_fault_history_t *h, uint32_t now_ms,
                        bool pending_bounce, spk_recov_state_t state)
{
    /* Not part of the decision: a bounce consumed by a host reopen cannot
     * erase the fault that produced it. */
    (void)pending_bounce;

    if (spk_fault_history_should_latch(h, now_ms)) {
        return SPK_RECOV_LATCHED;
    }
    return state;
}

/* Decision time for a recurrence evaluation (review finding 2).
 *
 * The ISR stamps a fault with the clock value at the moment it publishes the
 * event. A caller that samples "now" BEFORE claiming can therefore hold a
 * time that is older than the timestamp it is about to claim: the fault
 * arrived in the gap. The history predicate's unsigned subtraction would then
 * place that fault in the future and silently exclude it from the count
 * ((now - at) wraps to a huge number). If it was the third fault, no latch
 * happens, and later fault-free passes never reconsider it because
 * spk_recovery_claim_decide() returns early when there is no event.
 *
 * The invariant is: fault_at_ms <= decision_ms for every event a claim can
 * observe. Pass the pre-claim sample and a sample taken AFTER the claim; this
 * helper returns the later one (wrap-safe). The firmware always passes a
 * post-claim sample, and the host test (T24/T26) exercises the forward-stamped
 * interleaving and the 32-bit wrap directly.
 *
 * At the wrap the later sample can legitimately be 0 ms; that is a timestamp,
 * not a sentinel, and the history predicate accepts it. */
static inline uint32_t spk_recovery_decision_ms(uint32_t pre_ms,
                                                uint32_t post_ms)
{
    return ((int32_t)(post_ms - pre_ms) >= 0) ? post_ms : pre_ms;
}

/* Claim one ISR fault and return the state after the recurrence decision.
 * The order is fixed here: claim -> record -> decide, all before the caller
 * touches the pending bounce or the host epoch. The helper returns the state;
 * the caller applies it (and counts a transition) so the counters stay in the
 * state-machine owner.
 *
 * `ev.valid` separates "no fault" from "fault at timestamp 0".
 * `now_ms` MUST be sampled after the claim (or be
 * spk_recovery_decision_ms(pre_claim, post_claim)); every timestamp the claim
 * can observe is then <= now_ms (review finding 2). */
static inline spk_recov_state_t
spk_recovery_claim_decide(spk_fault_history_t *h, spk_fault_ledger_t *l,
                          spk_fault_event_t ev, uint32_t now_ms,
                          spk_recov_state_t state)
{
    if (!spk_fault_ledger_claim(l, h, ev)) {
        return state;
    }
    return spk_recovery_next_state(h, now_ms,
                                   spk_fault_ledger_pending(l).valid, state);
}

/* ---- visibility handoff (review finding 1) ------------------------------
 *
 * LATCHED is a verdict about the speaker endpoint, not permission to keep the
 * whole USB persona hidden: a latched recovery performs no reconnect and main
 * used to publish CONNECT only from BT events, so a steady BT link left USB
 * hidden forever. The handoff below is the shared, policy-level predicate for
 * "the persona should be visible again but is not, and no edge is in flight":
 *
 *   bt_connected      BT link is up, so the persona is expected on USB
 *   phy_up            direction from usb_phy_snapshot() (last applied edge)
 *   request_pending   any owner's edge is queued (never fight a DISCONNECT)
 *
 * Pure and SDK-free so both the recovery's LATCHED handoff and main's loop
 * guarantee use the same rule, and the host test (T25) drives the exact
 * sequence: latch with BT steady -> handoff due -> PHY restored -> the latch
 * itself is untouched until a host reopen. */
static inline bool spk_visibility_handoff_due(bool bt_connected, bool phy_up,
                                              bool request_pending)
{
    return bt_connected && !phy_up && !request_pending;
}

/* Grace for main's visibility handoff, tied to the latest APPLIED PHY edge
 * (review finding 2). The old timer started on the first observation of
 * "PHY down" and was never restarted by an edge, so a CONNECT/DISCONNECT pair
 * that was applied between two passes kept an earlier down timestamp and main
 * could publish CONNECT less than 250 ms after the latest disconnect.
 *
 * The rule is: publish only when the last applied edge was a DISCONNECT and at
 * least SPK_VISIBILITY_GRACE_MS have passed since THAT edge. Every applied
 * edge carries a new generation and a new timestamp, so an intervening edge
 * restarts the window by construction - polling cannot skip it. A no-op
 * publication (same direction) does not bump the generation or restamp the
 * time (review finding 1), so it cannot restart the window either.
 *
 * Wrap-safe via a signed difference, like the history predicate. */
#define SPK_VISIBILITY_GRACE_MS 250u

static inline bool spk_visibility_grace_elapsed_ms(bool last_edge_disconnect,
                                                   uint32_t last_edge_ms,
                                                   uint32_t now_ms,
                                                   uint32_t grace_ms)
{
    if (!last_edge_disconnect) {
        return false;                 /* someone applied CONNECT: PHY is up */
    }
    return (int32_t)(now_ms - last_edge_ms) >= (int32_t)grace_ms;
}

static inline bool spk_visibility_grace_elapsed(bool last_edge_disconnect,
                                                uint32_t last_edge_ms,
                                                uint32_t now_ms)
{
    return spk_visibility_grace_elapsed_ms(last_edge_disconnect, last_edge_ms,
                                           now_ms, SPK_VISIBILITY_GRACE_MS);
}

/* Recovery takeover reconciliation (review finding 2).
 *
 * While the recovery waits for its own DISCONNECT in SPK_RECOV_DISCONNECTED,
 * another owner (main's handoff, or the host path) may apply a CONNECT. The
 * PHY is then up for a reason that is not "our disconnect was lost", and
 * re-publishing DISCONNECT would tear down the link a second time.
 *
 * All three inputs MUST come from the SAME usb_phy_snapshot(): the direction,
 * the generation and the generation captured when we published DISCONNECT.
 * The old code read "is the PHY up?" and "what is the generation?" separately,
 * so our own DISCONNECT landing between the two reads looked like an external
 * CONNECT while the PHY was actually down. The direction is part of the rule:
 *
 *   snapshot_connected && snapshot_gen == gen_at_disconnect_request
 *       no direction-changing edge has been applied since we asked -> our
 *       DISCONNECT is still unapplied (or was refused) -> retry.
 *   snapshot_connected && snapshot_gen != gen_at_disconnect_request
 *       an edge was applied after our request and the PHY is up -> takeover:
 *       resolve to IDLE/confirmed, never disconnect again.
 *   !snapshot_connected
 *       the PHY is down. Our own DISCONNECT (same direction) is never a
 *       takeover, regardless of the generation.
 *
 * SPK_RECOV_CONNECTING with the PHY up is always a confirm: the obligation
 * ("persona visible") is satisfied regardless of who applied the CONNECT. */
static inline bool spk_recovery_edge_takeover(bool snapshot_connected,
                                              uint32_t snapshot_gen,
                                              uint32_t gen_at_disconnect_request)
{
    return snapshot_connected &&
           (snapshot_gen != gen_at_disconnect_request);
}

#endif /* SPK_RECOVERY_POLICY_H */
