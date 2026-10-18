/*
 * xinput_map_test.c - host tests for the DS4 -> XUSB mapping (spec section 9).
 *
 * No SDK, no device: the shipping mapping core (src/xinput_map.c) and the
 * turbo rule (src/xinput_turbo.h) are compiled and executed here unchanged.
 *
 * Build and run:
 *   cc -std=c11 -Wall -Wextra -I src tools/xinput_map_test.c \
 *      src/xinput_map.c -lm -o /tmp/xinput_map_test && /tmp/xinput_map_test
 *
 * Exit code: 0 all groups pass, 1 any check failed. One PASS line per group.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "xinput_map.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        g_checks++;                                                      \
        if (!(cond)) {                                                   \
            g_failures++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);               \
        }                                                                \
    } while (0)

static void report(const char *name, int failures_before)
{
    if (g_failures == failures_before) {
        printf("PASS %s\n", name);
    } else {
        printf("FAIL %s (%d check(s) failed)\n", name,
               g_failures - failures_before);
    }
}

/* ---- helpers ---------------------------------------------------------- */

static void neutral_state(uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE])
{
    memset(s, 0, DS4_BT_INPUT_PAYLOAD_SIZE);
    s[0] = 128; /* LX */
    s[1] = 128; /* LY */
    s[2] = 128; /* RX */
    s[3] = 128; /* RY */
    s[4] = DS4_HAT_NEUTRAL;
}

static uint16_t out_buttons(const uint8_t out[XINPUT_REPORT_LEN])
{
    return (uint16_t)(out[2] | ((uint16_t)out[3] << 8));
}

static int16_t out_s16(const uint8_t *p)
{
    return (int16_t)(uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

/* Session generation used by the single-frame helpers. A fresh map starts at
 * gen 0, so this is the same session and never invalidates the pushed block. */
#define TEST_GEN 0u

/* One frame from a fresh map (no cross-frame turbo state). */
static void one_shot(const uint8_t s[], uint8_t out[XINPUT_REPORT_LEN])
{
    xinput_map_t m;

    xinput_map_init(&m);
    xinput_map_set_connected(&m, true, TEST_GEN);
    xinput_map_set_state(&m, s);
    xinput_map_tick(&m, 0, out);
}

/* One frame from a persistent map in an explicit session generation. */
static void frame_gen(xinput_map_t *m, const uint8_t s[], uint32_t now,
                      uint8_t out[XINPUT_REPORT_LEN], uint32_t gen)
{
    xinput_map_set_connected(m, true, gen);
    xinput_map_set_state(m, s);
    xinput_map_tick(m, now, out);
}

/* One frame from a persistent map, for timing sequences. */
static void frame(xinput_map_t *m, const uint8_t s[], uint32_t now,
                  uint8_t out[XINPUT_REPORT_LEN])
{
    frame_gen(m, s, now, out, TEST_GEN);
}

static bool near_i32(int v, int want, int tol)
{
    int d = v - want;

    if (d < 0) {
        d = -d;
    }
    return d <= tol;
}

/* ---- group 1: one button at a time ------------------------------------ */

struct button_case {
    int     off;
    uint8_t bit;
    uint16_t expect;
};

static const struct button_case g_button_cases[] = {
    { 4, 0x10u, XINPUT_BTN_X },     /* Square   -> X      */
    { 4, 0x20u, XINPUT_BTN_A },     /* Cross    -> A      */
    { 4, 0x40u, XINPUT_BTN_B },     /* Circle   -> B      */
    { 4, 0x80u, XINPUT_BTN_Y },     /* Triangle -> Y      */
    { 5, 0x01u, XINPUT_BTN_LB },    /* L1       -> LB     */
    { 5, 0x02u, XINPUT_BTN_RB },    /* R1       -> RB     */
    { 5, 0x10u, XINPUT_BTN_BACK },  /* Share    -> Back   */
    { 5, 0x20u, XINPUT_BTN_START }, /* Options  -> Start  */
    { 5, 0x40u, XINPUT_BTN_L3 },
    { 5, 0x80u, XINPUT_BTN_R3 },
    { 6, 0x01u, XINPUT_BTN_GUIDE }, /* PS       -> Guide  */
};

static void group1_buttons(void)
{
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];

    for (size_t i = 0; i < sizeof(g_button_cases) / sizeof(g_button_cases[0]);
         i++) {
        neutral_state(s);
        if (g_button_cases[i].off == 4) {
            /* Byte 4 low nibble is the hat: keep it neutral. */
            s[4] = (uint8_t)(DS4_HAT_NEUTRAL | g_button_cases[i].bit);
        } else {
            s[g_button_cases[i].off] = g_button_cases[i].bit;
        }
        one_shot(s, out);

        /* Exactly the expected bit: nothing else changed. */
        CHECK(out_buttons(out) == g_button_cases[i].expect);
        /* No trigger, no stick movement leaked in. */
        CHECK(out[4] == 0);
        CHECK(out[5] == 0);
        CHECK(out_s16(&out[6]) == 0);
        CHECK(out_s16(&out[8]) == 0);
        CHECK(out_s16(&out[10]) == 0);
        CHECK(out_s16(&out[12]) == 0);
    }

    /* Digital L2/R2 bits are not button-word bits (analog-only triggers). */
    neutral_state(s);
    s[5] = 0x0Cu;
    one_shot(s, out);
    CHECK(out_buttons(out) == 0);

    /* Touchpad click alone is a modifier, never reported. */
    neutral_state(s);
    s[6] = 0x02u;
    one_shot(s, out);
    CHECK(out_buttons(out) == 0);

    /* Combined presses: each maps to its own bit, no overlap. */
    neutral_state(s);
    s[4] = (uint8_t)(DS4_HAT_NEUTRAL | 0x30u); /* Square + Cross */
    s[5] = 0x21u;                              /* L1 + Options   */
    s[6] = 0x01u;                              /* PS             */
    one_shot(s, out);
    CHECK(out_buttons(out) == (uint16_t)(XINPUT_BTN_X | XINPUT_BTN_A |
                                         XINPUT_BTN_LB | XINPUT_BTN_START |
                                         XINPUT_BTN_GUIDE));
}

/* ---- group 2: hat / dpad ---------------------------------------------- */

struct hat_case {
    uint8_t  hat;
    uint16_t expect;
};

static const struct hat_case g_hat_cases[] = {
    { 0, XINPUT_BTN_DPAD_UP },
    { 1, (uint16_t)(XINPUT_BTN_DPAD_UP | XINPUT_BTN_DPAD_RIGHT) },
    { 2, XINPUT_BTN_DPAD_RIGHT },
    { 3, (uint16_t)(XINPUT_BTN_DPAD_DOWN | XINPUT_BTN_DPAD_RIGHT) },
    { 4, XINPUT_BTN_DPAD_DOWN },
    { 5, (uint16_t)(XINPUT_BTN_DPAD_DOWN | XINPUT_BTN_DPAD_LEFT) },
    { 6, XINPUT_BTN_DPAD_LEFT },
    { 7, (uint16_t)(XINPUT_BTN_DPAD_UP | XINPUT_BTN_DPAD_LEFT) },
    { 8, 0 },
    { 0x0F, 0 },
};

static void group2_hat(void)
{
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];

    for (size_t i = 0; i < sizeof(g_hat_cases) / sizeof(g_hat_cases[0]); i++) {
        neutral_state(s);
        s[4] = g_hat_cases[i].hat;
        one_shot(s, out);
        CHECK(out_buttons(out) == g_hat_cases[i].expect);
    }

    /* Hat and face buttons share byte 4: the high nibble must not disturb
     * the hat decode and vice versa. */
    neutral_state(s);
    s[4] = 0x28u; /* Cross, hat neutral */
    one_shot(s, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    neutral_state(s);
    s[4] = 0x21u; /* Cross, hat north-east */
    one_shot(s, out);
    CHECK(out_buttons(out) == (uint16_t)(XINPUT_BTN_A |
                                         XINPUT_BTN_DPAD_UP |
                                         XINPUT_BTN_DPAD_RIGHT));
}

/* ---- group 3: stick endpoints and midpoints ---------------------------- */

static void group3_sticks(void)
{
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];
    int16_t xmid;

    /* Centre: both sticks exactly 0. */
    neutral_state(s);
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 0);
    CHECK(out_s16(&out[8]) == 0);
    CHECK(out_s16(&out[10]) == 0);
    CHECK(out_s16(&out[12]) == 0);

    /* X endpoints. */
    neutral_state(s);
    s[0] = 0;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == -32768);
    neutral_state(s);
    s[0] = 255;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 32767);

    /* Y endpoints, negated: DS4 positive-down -> XInput positive-up. */
    neutral_state(s);
    s[1] = 0;
    one_shot(s, out);
    CHECK(out_s16(&out[8]) == 32767);
    neutral_state(s);
    s[1] = 255;
    one_shot(s, out);
    CHECK(out_s16(&out[8]) == -32768);

    /* Right stick behaves identically to the left. */
    neutral_state(s);
    s[2] = 0;
    one_shot(s, out);
    CHECK(out_s16(&out[10]) == -32768);
    neutral_state(s);
    s[2] = 255;
    one_shot(s, out);
    CHECK(out_s16(&out[10]) == 32767);
    neutral_state(s);
    s[3] = 0;
    one_shot(s, out);
    CHECK(out_s16(&out[12]) == 32767);
    neutral_state(s);
    s[3] = 255;
    one_shot(s, out);
    CHECK(out_s16(&out[12]) == -32768);

    /* Midpoints: normalized 64/127 = 0.5039, anti-deadzone 0.5386, then
     * (r+1)*0.5*65535-32768 = 17647 (X) and -17648 (mirrored Y). */
    neutral_state(s);
    s[0] = 192;
    one_shot(s, out);
    xmid = out_s16(&out[6]);
    CHECK(near_i32(xmid, 17647, 2));
    CHECK(xmid > 0);
    CHECK(xmid < 32767);

    neutral_state(s);
    s[1] = 192;
    one_shot(s, out);
    CHECK(near_i32(out_s16(&out[8]), -17648, 2));
    CHECK(out_s16(&out[8]) < 0);
    /* Mirrored Y is the X value minus the one-LSB centre offset. */
    CHECK(near_i32(-out_s16(&out[8]), xmid, 1));
}

/* ---- group 4: deadzone ------------------------------------------------- */

static void group4_deadzone(void)
{
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];
    int prev;
    int v;

    /* Inside the 0.10 radius: exactly 0 on both axes. */
    neutral_state(s);
    s[0] = 129;
    s[1] = 133;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 0);
    CHECK(out_s16(&out[8]) == 0);

    /* Diagonal just inside the radius (sqrt(2)*0.063 = 0.089). */
    neutral_state(s);
    s[0] = 136;
    s[1] = 136;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 0);
    CHECK(out_s16(&out[8]) == 0);

    /* Boundary on the positive half (/127): 12/127 = 0.0945 is inside,
     * 13/127 = 0.1024 is outside. */
    neutral_state(s);
    s[0] = 140;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 0);

    neutral_state(s);
    s[0] = 141;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) > 0);
    CHECK(near_i32(out_s16(&out[6]), 102, 2));

    /* Monotonic ramp, negative side and positive side, endpoints reached. */
    prev = -32769;
    for (int raw = 0; raw <= 128; raw++) {
        neutral_state(s);
        s[0] = (uint8_t)raw;
        one_shot(s, out);
        v = out_s16(&out[6]);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(prev == 0);

    prev = 0;
    for (int raw = 128; raw <= 255; raw++) {
        neutral_state(s);
        s[0] = (uint8_t)raw;
        one_shot(s, out);
        v = out_s16(&out[6]);
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(prev == 32767);

    /* Past the deadzone the response really moves, not just clamps. */
    neutral_state(s);
    s[0] = 160;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) > 0);
    neutral_state(s);
    s[0] = 200;
    one_shot(s, out);
    CHECK(near_i32(out_s16(&out[6]), 20399, 2));

    /* Sticks are independent: left full, right centred. */
    neutral_state(s);
    s[0] = 255;
    one_shot(s, out);
    CHECK(out_s16(&out[6]) == 32767);
    CHECK(out_s16(&out[10]) == 0);
    CHECK(out_s16(&out[12]) == 0);
}

/* ---- group 5: triggers ------------------------------------------------- */

static void group5_triggers(void)
{
    static const uint8_t levels[] = { 0, 128, 255 };
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];

    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        neutral_state(s);
        s[7] = levels[i];
        s[8] = levels[i];
        one_shot(s, out);
        CHECK(out[4] == levels[i]);
        CHECK(out[5] == levels[i]);
        CHECK(out_buttons(out) == 0);
    }

    /* Asymmetric levels stay independent. */
    neutral_state(s);
    s[7] = 0;
    s[8] = 255;
    one_shot(s, out);
    CHECK(out[4] == 0);
    CHECK(out[5] == 255);

    neutral_state(s);
    s[7] = 255;
    s[8] = 37;
    one_shot(s, out);
    CHECK(out[4] == 255);
    CHECK(out[5] == 37);
}

/* ---- group 6: button-word mask ---------------------------------------- */

static void group6_mask(void)
{
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];
    uint16_t b;

    CHECK(XINPUT_BTN_VALID_MASK == 0xF7FFu);

    /* Every single DS4 bit, one at a time. */
    for (int off = 4; off <= 6; off++) {
        for (int bit = 0; bit < 8; bit++) {
            neutral_state(s);
            s[off] = (uint8_t)(1u << bit);
            one_shot(s, out);
            b = out_buttons(out);
            CHECK((b & (uint16_t)~XINPUT_BTN_VALID_MASK) == 0);
            CHECK((b & 0x0800u) == 0); /* reserved bit 11 */
        }
    }

    /* Everything at once: still no bit outside the mask. */
    neutral_state(s);
    s[4] = (uint8_t)(0xF8u | 0x0Fu);
    s[5] = 0xFFu;
    s[6] = 0xFFu;
    one_shot(s, out);
    b = out_buttons(out);
    CHECK((b & (uint16_t)~XINPUT_BTN_VALID_MASK) == 0);
    CHECK((b & 0x0800u) == 0);
}

/* ---- group 7: touchpad turbo ------------------------------------------ */

static void group7_turbo(void)
{
    xinput_map_t m;
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];
    uint16_t expect;

    /* (a) Touchpad up: a held A never pulses, no matter how long. */
    xinput_map_init(&m);
    neutral_state(s);
    s[4] = DS4_HAT_NEUTRAL | 0x20u; /* Cross */
    {
        static const uint32_t times[] = { 0, 100, 400, 800, 1500, 3333 };
        for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
            frame(&m, s, times[i], out);
            CHECK(out_buttons(out) == XINPUT_BTN_A);
        }
    }

    /* (b) Arm threshold and 15 Hz phase: both pressed at t=0.
     * Off frames are the odd 33 ms half-periods only, and only at/after
     * 400 ms. */
    xinput_map_init(&m);
    s[6] = 0x02u;
    frame(&m, s, 0, out);              /* arm window starts here */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 396, out);            /* 12*33, but not armed yet */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 399, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    for (uint32_t t = 400; t <= 500; t++) {
        bool off = ((t / 33u) & 1u) != 0u;
        frame(&m, s, t, out);
        expect = off ? 0u : (uint16_t)XINPUT_BTN_A;
        CHECK(out_buttons(out) == expect);
    }
    CHECK(m.turbo.armed != 0u);

    /* (c) Release clears: re-press inside an off phase is a real press. */
    neutral_state(s);
    s[6] = 0x02u;                      /* touchpad still held */
    frame(&m, s, 510, out);
    CHECK(out_buttons(out) == 0);
    s[4] = DS4_HAT_NEUTRAL | 0x20u;
    frame(&m, s, 511, out);            /* 15*33 = odd phase */
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    /* (d) Touchpad release resets, next touchpad press restarts the window
     * (key has been held since 511). */
    neutral_state(s);
    s[4] = DS4_HAT_NEUTRAL | 0x20u;
    frame(&m, s, 520, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 561, out);            /* odd phase, still plain press */
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    frame(&m, s, 570, out);            /* touchpad released: plain press */
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    s[6] = 0x02u;                      /* touchpad pressed again at 600 */
    frame(&m, s, 600, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 957, out);            /* 357 ms later: key press was at 511,
                                        * so an arm from the key would already
                                        * pulse here (odd phase). */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 999, out);            /* 399 ms after touchpad */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 1000, out);           /* armed, even phase */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 1033, out);           /* armed, odd phase */
    CHECK(out_buttons(out) == 0);

    /* (e) A held first, touchpad pressed later: the window starts at the
     * touchpad press (max of the two), not at the A press. */
    xinput_map_init(&m);
    neutral_state(s);
    s[4] = DS4_HAT_NEUTRAL | 0x20u;
    frame(&m, s, 0, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    s[6] = 0x02u;
    frame(&m, s, 100, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 450, out);            /* odd phase, 350 ms window */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 499, out);            /* 399 ms */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 500, out);            /* 400 ms, odd phase */
    CHECK(out_buttons(out) == 0);

    /* (f) LB/RB participate too, and pulse together. */
    xinput_map_init(&m);
    neutral_state(s);
    s[5] = 0x03u;
    s[6] = 0x02u;
    frame(&m, s, 0, out);
    CHECK(out_buttons(out) == (uint16_t)(XINPUT_BTN_LB | XINPUT_BTN_RB));
    frame(&m, s, 430, out);
    CHECK(out_buttons(out) == 0);
    frame(&m, s, 462, out);
    CHECK(out_buttons(out) == (uint16_t)(XINPUT_BTN_LB | XINPUT_BTN_RB));

    /* (g) Non-participating buttons are never suppressed: Guide, Start,
     * dpad right. */
    xinput_map_init(&m);
    neutral_state(s);
    s[4] = (uint8_t)(0x02u | 0x20u);   /* hat east + Cross */
    s[5] = 0x20u;                      /* Options */
    s[6] = (uint8_t)(0x02u | 0x01u);   /* touchpad + PS */
    expect = (uint16_t)(XINPUT_BTN_DPAD_RIGHT | XINPUT_BTN_A |
                        XINPUT_BTN_START | XINPUT_BTN_GUIDE);
    {
        static const uint32_t times[] = { 0, 430, 462, 495, 1600 };
        for (size_t i = 0; i < sizeof(times) / sizeof(times[0]); i++) {
            uint16_t want = expect;

            /* A is a turbo target and pulses; the other three bits do not. */
            if (times[i] >= 400u && ((times[i] / 33u) & 1u) != 0u) {
                want = (uint16_t)(want & (uint16_t)~XINPUT_BTN_A);
            }
            frame(&m, s, times[i], out);
            CHECK(out_buttons(out) == want);
            CHECK((out_buttons(out) & (uint16_t)(XINPUT_BTN_DPAD_RIGHT |
                                                 XINPUT_BTN_START |
                                                 XINPUT_BTN_GUIDE)) ==
                  (uint16_t)(XINPUT_BTN_DPAD_RIGHT | XINPUT_BTN_START |
                             XINPUT_BTN_GUIDE));
        }
    }

    /* (h) Per-key timers: A armed at t=0, B pressed at t=200 stays a real
     * press while A is in an off phase. */
    xinput_map_init(&m);
    neutral_state(s);
    s[4] = DS4_HAT_NEUTRAL | 0x20u;    /* Cross only */
    s[6] = 0x02u;
    frame(&m, s, 0, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    s[4] = (uint8_t)(DS4_HAT_NEUTRAL | 0x20u | 0x40u); /* Cross + Circle */
    frame(&m, s, 200, out);
    CHECK(out_buttons(out) == (uint16_t)(XINPUT_BTN_A | XINPUT_BTN_B));
    frame(&m, s, 430, out);            /* odd phase: A off, B not armed */
    CHECK(out_buttons(out) == XINPUT_BTN_B);
}

/* ---- group 8: disconnected neutral frame ------------------------------ */

static void group8_disconnected(void)
{
    xinput_map_t m;
    uint8_t s[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];

    /* Never connected: neutral from the first tick. */
    xinput_map_init(&m);
    xinput_map_tick(&m, 0, out);
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x14);
    for (int i = 2; i < XINPUT_REPORT_LEN; i++) {
        CHECK(out[i] == 0);
    }

    /* Something was pressed, then the link dropped: still neutral. */
    neutral_state(s);
    s[4] = 0xF8u;
    s[5] = 0xFFu;
    s[6] = 0x03u;
    s[7] = 200;
    s[8] = 210;
    s[0] = 0;
    s[1] = 255;
    frame(&m, s, 0, out);
    CHECK(out_buttons(out) != 0);

    xinput_map_set_connected(&m, false, 1);
    xinput_map_tick(&m, 10, out);
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x14);
    for (int i = 2; i < XINPUT_REPORT_LEN; i++) {
        CHECK(out[i] == 0);
    }

    /* Disconnect resets the turbo window: after a reconnect the same held
     * buttons are a plain press again even in an off phase. */
    xinput_map_init(&m);
    neutral_state(s);
    s[4] = DS4_HAT_NEUTRAL | 0x20u;    /* Cross */
    s[6] = 0x02u;                      /* touchpad */
    frame(&m, s, 0, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    frame(&m, s, 500, out);            /* 15*33 = odd: turbo off */
    CHECK(out_buttons(out) == 0);

    xinput_map_set_connected(&m, false, 2);
    xinput_map_tick(&m, 501, out);
    CHECK(out_buttons(out) == 0);

    /* Reconnect in a new generation: neutral until a new block arrives,
     * then the held buttons are a plain press again even in an off phase. */
    frame_gen(&m, s, 825, out, 3);     /* 25*33 = odd phase */
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    /* Tail is always zero on a live frame too. */
    for (int i = 14; i < XINPUT_REPORT_LEN; i++) {
        CHECK(out[i] == 0);
    }
}

/* ---- group 9: session generation -------------------------------------- */

static void group9_session_gen(void)
{
    xinput_map_t m;
    uint8_t a[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t b[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint8_t out[XINPUT_REPORT_LEN];

    neutral_state(a);
    a[4] = DS4_HAT_NEUTRAL | 0x20u;    /* Cross */
    neutral_state(b);
    b[4] = DS4_HAT_NEUTRAL | 0x40u;    /* Circle */

    /* A block for the current session is reported immediately. */
    xinput_map_init(&m);
    xinput_map_set_connected(&m, true, 1);
    xinput_map_set_state(&m, a);
    xinput_map_tick(&m, 0, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);

    /* Disconnect (new generation): neutral frame even though a block for the
     * old session is still stored. */
    xinput_map_set_connected(&m, false, 2);
    xinput_map_tick(&m, 4, out);
    CHECK(out[0] == 0x00);
    CHECK(out[1] == 0x14);
    for (int i = 2; i < XINPUT_REPORT_LEN; i++) {
        CHECK(out[i] == 0);
    }

    /* Reconnect (new generation): still neutral, the previous controller's
     * last input must not leak into the new session. */
    xinput_map_set_connected(&m, true, 3);
    xinput_map_tick(&m, 8, out);
    CHECK(out_buttons(out) == 0);
    CHECK(out_s16(&out[6]) == 0);
    CHECK(out_s16(&out[8]) == 0);
    CHECK(out_s16(&out[10]) == 0);
    CHECK(out_s16(&out[12]) == 0);
    CHECK(out[4] == 0);
    CHECK(out[5] == 0);

    /* A block for the new session resumes output. */
    xinput_map_set_state(&m, b);
    xinput_map_tick(&m, 12, out);
    CHECK(out_buttons(out) == XINPUT_BTN_B);

    /* Repeating the same generation is not a new session: the block stays
     * eligible. */
    xinput_map_set_connected(&m, true, 3);
    xinput_map_tick(&m, 14, out);
    CHECK(out_buttons(out) == XINPUT_BTN_B);

    /* A generation change while still connected also invalidates the block. */
    xinput_map_set_connected(&m, true, 4);
    xinput_map_tick(&m, 16, out);
    CHECK(out_buttons(out) == 0);

    /* And it resets an already armed turbo window. */
    xinput_map_init(&m);
    neutral_state(a);
    a[4] = DS4_HAT_NEUTRAL | 0x20u;    /* Cross */
    a[6] = 0x02u;                      /* touchpad modifier */
    xinput_map_set_connected(&m, true, 10);
    xinput_map_set_state(&m, a);
    xinput_map_tick(&m, 0, out);
    CHECK(out_buttons(out) == XINPUT_BTN_A);
    xinput_map_tick(&m, 500, out);     /* armed, 15*33 = odd phase */
    CHECK(out_buttons(out) == 0);

    xinput_map_set_connected(&m, true, 11);   /* session change, connected */
    xinput_map_tick(&m, 501, out);     /* no block for the new session */
    CHECK(out_buttons(out) == 0);

    xinput_map_set_state(&m, a);
    xinput_map_tick(&m, 502, out);     /* odd phase, but not armed yet */
    CHECK(out_buttons(out) == XINPUT_BTN_A);
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    int before;

    printf("xinput_map host test\n");

    before = g_failures;
    group1_buttons();
    report("group 1: per-button mapping", before);

    before = g_failures;
    group2_hat();
    report("group 2: hat / dpad", before);

    before = g_failures;
    group3_sticks();
    report("group 3: stick endpoints and midpoints", before);

    before = g_failures;
    group4_deadzone();
    report("group 4: radial deadzone", before);

    before = g_failures;
    group5_triggers();
    report("group 5: triggers", before);

    before = g_failures;
    group6_mask();
    report("group 6: button-word mask", before);

    before = g_failures;
    group7_turbo();
    report("group 7: touchpad turbo", before);

    before = g_failures;
    group8_disconnected();
    report("group 8: disconnected neutral frame", before);

    before = g_failures;
    group9_session_gen();
    report("group 9: session generation", before);

    if (g_failures != 0) {
        printf("FAILED: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    printf("OK: %d checks\n", g_checks);
    return 0;
}
