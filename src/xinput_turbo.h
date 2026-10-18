/*
 * xinput_turbo.h - touchpad-modifier auto-repeat (turbo) for the XUSB persona.
 *
 * Header-only, static inline, zero SDK dependencies (only <stdint.h> and
 * <stdbool.h>), same style as spk_recovery_policy.h. The rule the firmware
 * runs is therefore compiled and asserted by the host test
 * (tools/xinput_map_test.c).
 *
 * Semantics (docs/phase2-gamepad-spec.md section 6, docs/touchpad-turbo.md):
 *
 *   - The touchpad click is a MODIFIER, never reported on its own (the XUSB
 *     20-byte report has no field for it).
 *   - While the touchpad is held, a turbo key held for >= XINPUT_TURBO_ARM_MS
 *     (400 ms) starts pulsing at 15 Hz: 33 ms on / 33 ms off. The off half is
 *     the odd half-period, i.e. ((now_ms / 33) & 1) == 1.
 *   - The arm window starts at max(key press, touchpad press). Both edges are
 *     recovered from one call per frame, the same cadence main.c uses.
 *   - Releasing either the key or the touchpad drops back to plain behaviour
 *     immediately and clears the per-key timer, so a re-press is never armed
 *     with a stale timestamp.
 *   - Only A/B/X/Y + LB/RB participate. Every other bit passes through
 *     untouched, including Guide/Start/Back/dpad/L3/R3.
 *
 * Timestamps are monotonic 32-bit milliseconds. UINT32_MAX is the "not
 * armed" sentinel (spec section 6), not 0, because 0 is a legal timestamp
 * right after the clock wrap.
 */
#ifndef XINPUT_TURBO_H
#define XINPUT_TURBO_H

#include <stdint.h>
#include <stdbool.h>

/* Hold time before a key starts pulsing. */
#define XINPUT_TURBO_ARM_MS   400u
/* Half period of the 15 Hz pulse train (33 ms on, 33 ms off). */
#define XINPUT_TURBO_HALF_MS  33u
/* Number of keys that participate in v1. */
#define XINPUT_TURBO_BUTTONS  6u
/* "No timestamp recorded" sentinel. */
#define XINPUT_TURBO_NONE     UINT32_MAX

/* XUSB button-word bits owned by this module (same values as xinput_map.h). */
#define XINPUT_TURBO_BTN_A    0x1000u
#define XINPUT_TURBO_BTN_B    0x2000u
#define XINPUT_TURBO_BTN_X    0x4000u
#define XINPUT_TURBO_BTN_Y    0x8000u
#define XINPUT_TURBO_BTN_LB   0x0100u
#define XINPUT_TURBO_BTN_RB   0x0200u
#define XINPUT_TURBO_BTN_MASK 0xF300u

typedef struct {
    /* Per-key arm reference time (max of key press and touchpad press).
     * XINPUT_TURBO_NONE while the key is not held or the touchpad is up. */
    uint32_t since_ms[XINPUT_TURBO_BUTTONS];
    /* Touchpad press time, XINPUT_TURBO_NONE while the touchpad is up. */
    uint32_t touchpad_since_ms;
    /* Previous frame touchpad state, used to spot the press edge. */
    bool     prev_touchpad;
    /* Diagnostic: 1 while at least one key is past the arm threshold. */
    uint8_t  armed;
} xinput_turbo_t;

/* Slot <-> button-bit mapping. A switch keeps the table free of an unused
 * translation unit static when only part of the API is used. */
static inline uint16_t xinput_turbo_bit(uint8_t slot)
{
    switch (slot) {
    case 0:  return XINPUT_TURBO_BTN_A;
    case 1:  return XINPUT_TURBO_BTN_B;
    case 2:  return XINPUT_TURBO_BTN_X;
    case 3:  return XINPUT_TURBO_BTN_Y;
    case 4:  return XINPUT_TURBO_BTN_LB;
    default: return XINPUT_TURBO_BTN_RB;
    }
}

/* Clear every timer. Every slot is "not armed", never 0. */
static inline void xinput_turbo_init(xinput_turbo_t *t)
{
    if (!t) {
        return;
    }
    for (uint8_t i = 0; i < XINPUT_TURBO_BUTTONS; i++) {
        t->since_ms[i] = XINPUT_TURBO_NONE;
    }
    t->touchpad_since_ms = XINPUT_TURBO_NONE;
    t->prev_touchpad = false;
    t->armed = 0;
}

/* Apply the turbo rule to one frame.
 *
 *   pressed  button word as mapped from the DS4 state (turbo bits included)
 *   touchpad touchpad click state for this frame (modifier only)
 *   now_ms   monotonic milliseconds
 *
 * Returns the button word to report: non-target bits untouched, target bits
 * gated by the pulse phase once armed. The touchpad is never added. */
static inline uint16_t xinput_turbo_apply(xinput_turbo_t *t, uint16_t pressed,
                                          bool touchpad, uint32_t now_ms)
{
    uint16_t out = pressed;
    uint8_t armed_any = 0;

    if (!t) {
        return pressed;
    }

    /* Touchpad edge: the window reference is the moment it went down. A
     * release clears the reference; the next press restarts every key. */
    if (!touchpad) {
        t->touchpad_since_ms = XINPUT_TURBO_NONE;
    } else if (!t->prev_touchpad ||
               t->touchpad_since_ms == XINPUT_TURBO_NONE) {
        t->touchpad_since_ms = now_ms;
    }

    for (uint8_t i = 0; i < XINPUT_TURBO_BUTTONS; i++) {
        uint16_t bit = xinput_turbo_bit(i);
        bool held = (pressed & bit) != 0u;

        if (!held || !touchpad) {
            /* Plain press, or no modifier: never pulses, timer idle. */
            t->since_ms[i] = XINPUT_TURBO_NONE;
            continue;
        }

        /* First frame the key is held with the touchpad down: whichever of
         * the two was pressed later just happened, so "now" is max().
         * (If the key was already held when the touchpad went down, the
         * previous frames cleared the slot because the modifier was up.) */
        if (t->since_ms[i] == XINPUT_TURBO_NONE) {
            t->since_ms[i] = now_ms;
        }

        /* Not armed: pass the real press through (short taps survive). */
        if ((int32_t)(now_ms - t->since_ms[i]) <
            (int32_t)XINPUT_TURBO_ARM_MS) {
            continue;
        }

        armed_any = 1;
        if (((now_ms / XINPUT_TURBO_HALF_MS) & 1u) != 0u) {
            out = (uint16_t)(out & (uint16_t)~bit);
        }
    }

    t->armed = armed_any;
    t->prev_touchpad = touchpad;
    return out;
}

#endif /* XINPUT_TURBO_H */
