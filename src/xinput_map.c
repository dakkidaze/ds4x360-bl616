/*
 * xinput_map.c - DS4 state block -> XUSB report mapping.
 *
 * Spec: docs/phase2-gamepad-spec.md sections 3-6.
 *
 * Layout reminders (all offsets from ds4_protocol.h):
 *   byte 4 : low nibble hat, high nibble face buttons
 *   byte 5 : L1/R1/L2/R2/Share/Options/L3/R3
 *   byte 6 : bit0 PS (Guide), bit1 touchpad click (modifier only)
 *   byte 7 : L2 analog, byte 8 : R2 analog
 */
#include "xinput_map.h"

#include <math.h>
#include <string.h>

/* DS4 input bits (spec section 3). */
#define DS4_HAT_MASK     0x0Fu
#define DS4_FACE_SQUARE  0x10u
#define DS4_FACE_CROSS   0x20u
#define DS4_FACE_CIRCLE  0x40u
#define DS4_FACE_TRIANGLE 0x80u
#define DS4_MISC_L1      0x01u
#define DS4_MISC_R1      0x02u
#define DS4_MISC_SHARE   0x10u
#define DS4_MISC_OPTIONS 0x20u
#define DS4_MISC_L3      0x40u
#define DS4_MISC_R3      0x80u
#define DS4_PS_GUIDE     0x01u
#define DS4_PS_TOUCHPAD  0x02u

/* XUSB report offsets (spec section 4). */
#define XINPUT_REPORT_HDR0 0x00u
#define XINPUT_REPORT_HDR1 0x14u
#define XINPUT_OFF_BUTTONS 2
#define XINPUT_OFF_LT      4
#define XINPUT_OFF_RT      5
#define XINPUT_OFF_LX      6
#define XINPUT_OFF_LY      8
#define XINPUT_OFF_RX      10
#define XINPUT_OFF_RY      12
#define XINPUT_OFF_ZERO    14

/* Radial deadzone, applied per stick in normalized (-1..1) space. */
#define XINPUT_STICK_DZ    0.10f
#define XINPUT_STICK_ANTI  0.20f
#define XINPUT_STICK_MAX   1.00f

/* Little-endian 16-bit store. */
static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

/* DS4 raw byte -> normalized axis. Positive half uses /127, negative /128 so
 * raw 0, 128 and 255 land exactly on -1, 0 and +1. `invert` flips Y because
 * DS4 is positive-down and XInput is positive-up. */
static float stick_norm(uint8_t raw, bool invert)
{
    float v = (float)raw - 128.0f;
    float r = (v >= 0.0f) ? (v / 127.0f) : (v / 128.0f);

    return invert ? -r : r;
}

/* Radial deadzone with anti-deadzone expansion (spec section 5). */
static void stick_deadzone(float *x, float *y)
{
    float mag = sqrtf((*x) * (*x) + (*y) * (*y));
    float t;

    if (mag <= XINPUT_STICK_DZ) {
        *x = 0.0f;
        *y = 0.0f;
        return;
    }

    t = (mag - XINPUT_STICK_DZ) / (XINPUT_STICK_MAX - XINPUT_STICK_DZ);
    t *= 1.0f + XINPUT_STICK_ANTI;
    if (t > 1.0f) {
        t = 1.0f;
    }

    *x = (*x) / mag * t;
    *y = (*y) / mag * t;
}

/* Normalized axis -> signed 16-bit XUSB value. Truncation (not rounding)
 * keeps raw 128 exactly at 0: -0.5f truncates to 0. */
static int16_t stick_to_int16(float r)
{
    float f = (r + 1.0f) * 0.5f * 65535.0f - 32768.0f;

    if (f > 32767.0f) {
        f = 32767.0f;
    } else if (f < -32768.0f) {
        f = -32768.0f;
    }

    return (int16_t)f;
}

/* Hat (0..7 clockwise from north, 8 neutral) -> dpad bits; diagonals set
 * both of their cardinal bits. Out-of-range values are treated as neutral. */
static uint16_t hat_to_dpad(uint8_t hat)
{
    switch (hat & DS4_HAT_MASK) {
    case 0:  return XINPUT_BTN_DPAD_UP;
    case 1:  return (uint16_t)(XINPUT_BTN_DPAD_UP | XINPUT_BTN_DPAD_RIGHT);
    case 2:  return XINPUT_BTN_DPAD_RIGHT;
    case 3:  return (uint16_t)(XINPUT_BTN_DPAD_DOWN | XINPUT_BTN_DPAD_RIGHT);
    case 4:  return XINPUT_BTN_DPAD_DOWN;
    case 5:  return (uint16_t)(XINPUT_BTN_DPAD_DOWN | XINPUT_BTN_DPAD_LEFT);
    case 6:  return XINPUT_BTN_DPAD_LEFT;
    case 7:  return (uint16_t)(XINPUT_BTN_DPAD_UP | XINPUT_BTN_DPAD_LEFT);
    default: return 0u;      /* 8 = neutral, 9..15 undefined */
    }
}

/* Button word before turbo. The digital L2/R2 bits are deliberately not
 * mapped: the triggers are reported as analog values only. */
static uint16_t map_buttons(const uint8_t *s)
{
    uint16_t b = hat_to_dpad(s[DS4_PAYLOAD_HAT]);
    uint8_t face = (uint8_t)(s[DS4_PAYLOAD_HAT] & 0xF0u);
    uint8_t misc = s[DS4_PAYLOAD_BTN_MISC];
    uint8_t ps = s[DS4_PAYLOAD_BTN_PS];

    if (face & DS4_FACE_SQUARE) {
        b |= XINPUT_BTN_X;
    }
    if (face & DS4_FACE_CROSS) {
        b |= XINPUT_BTN_A;
    }
    if (face & DS4_FACE_CIRCLE) {
        b |= XINPUT_BTN_B;
    }
    if (face & DS4_FACE_TRIANGLE) {
        b |= XINPUT_BTN_Y;
    }

    if (misc & DS4_MISC_L1) {
        b |= XINPUT_BTN_LB;
    }
    if (misc & DS4_MISC_R1) {
        b |= XINPUT_BTN_RB;
    }
    if (misc & DS4_MISC_SHARE) {
        b |= XINPUT_BTN_BACK;
    }
    if (misc & DS4_MISC_OPTIONS) {
        b |= XINPUT_BTN_START;
    }
    if (misc & DS4_MISC_L3) {
        b |= XINPUT_BTN_L3;
    }
    if (misc & DS4_MISC_R3) {
        b |= XINPUT_BTN_R3;
    }

    if (ps & DS4_PS_GUIDE) {
        b |= XINPUT_BTN_GUIDE;
    }
    /* DS4_PS_TOUCHPAD is a modifier only (turbo), never a report bit. */

    return b;
}

void xinput_map_init(xinput_map_t *m)
{
    if (!m) {
        return;
    }

    memset(m, 0, sizeof(*m));
    m->state[DS4_PAYLOAD_STICK_LX] = 128;
    m->state[DS4_PAYLOAD_STICK_LY] = 128;
    m->state[DS4_PAYLOAD_STICK_RX] = 128;
    m->state[DS4_PAYLOAD_STICK_RY] = 128;
    m->state[DS4_PAYLOAD_HAT] = DS4_HAT_NEUTRAL;
    m->connected = false;
    xinput_turbo_init(&m->turbo);
}

void xinput_map_set_state(xinput_map_t *m,
                          const uint8_t state[DS4_BT_INPUT_PAYLOAD_SIZE],
                          bool connected)
{
    if (!m) {
        return;
    }

    if (state) {
        memcpy(m->state, state, DS4_BT_INPUT_PAYLOAD_SIZE);
    }
    m->connected = connected;
}

void xinput_map_tick(xinput_map_t *m, uint32_t now_ms,
                     uint8_t out[XINPUT_REPORT_LEN])
{
    const uint8_t *s;
    uint16_t buttons;
    float lx, ly, rx, ry;

    if (!m || !out) {
        return;
    }

    /* Header and the always-zero tail, valid for every frame. */
    out[0] = XINPUT_REPORT_HDR0;
    out[1] = XINPUT_REPORT_HDR1;
    memset(&out[XINPUT_OFF_ZERO], 0,
           (size_t)(XINPUT_REPORT_LEN - XINPUT_OFF_ZERO));

    if (!m->connected) {
        /* Neutral frame: header only, timers cleared so a reconnect cannot
         * resume a pulse window from before the disconnect. */
        memset(&out[XINPUT_OFF_BUTTONS], 0,
               (size_t)(XINPUT_REPORT_LEN - XINPUT_OFF_BUTTONS));
        xinput_turbo_init(&m->turbo);
        return;
    }

    s = m->state;

    buttons = map_buttons(s);
    buttons = xinput_turbo_apply(&m->turbo, buttons,
                                 (s[DS4_PAYLOAD_BTN_PS] &
                                  DS4_PS_TOUCHPAD) != 0u,
                                 now_ms);
    buttons = (uint16_t)(buttons & XINPUT_BTN_VALID_MASK);
    put_u16(&out[XINPUT_OFF_BUTTONS], buttons);

    /* Triggers are byte-for-byte passthrough, no scaling. */
    out[XINPUT_OFF_LT] = s[DS4_PAYLOAD_L2];
    out[XINPUT_OFF_RT] = s[DS4_PAYLOAD_R2];

    /* Both sticks: normalize, negate Y, then one radial deadzone each. */
    lx = stick_norm(s[DS4_PAYLOAD_STICK_LX], false);
    ly = stick_norm(s[DS4_PAYLOAD_STICK_LY], true);
    rx = stick_norm(s[DS4_PAYLOAD_STICK_RX], false);
    ry = stick_norm(s[DS4_PAYLOAD_STICK_RY], true);

    stick_deadzone(&lx, &ly);
    stick_deadzone(&rx, &ry);

    put_u16(&out[XINPUT_OFF_LX], (uint16_t)stick_to_int16(lx));
    put_u16(&out[XINPUT_OFF_LY], (uint16_t)stick_to_int16(ly));
    put_u16(&out[XINPUT_OFF_RX], (uint16_t)stick_to_int16(rx));
    put_u16(&out[XINPUT_OFF_RY], (uint16_t)stick_to_int16(ry));
}
