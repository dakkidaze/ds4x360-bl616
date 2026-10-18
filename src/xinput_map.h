/*
 * xinput_map.h - DS4 63-byte state block -> XUSB 20-byte input report.
 *
 * Pure mapping logic: no SDK headers, no USB driver calls, no globals. The
 * firmware feeds it the latest state block once per 4 ms tick and sends the
 * bytes it produces; tools/xinput_map_test.c feeds it the same way on the
 * host, so the exact mapping the device runs is the one under test.
 *
 * Spec: docs/phase2-gamepad-spec.md sections 3-6.
 *
 * Input offsets come from ds4_protocol.h (project header, stdint/stdbool only)
 * so a layout change cannot silently desynchronise the two.
 */
#ifndef XINPUT_MAP_H
#define XINPUT_MAP_H

#include <stdint.h>
#include <stdbool.h>

#include "ds4_protocol.h"
#include "xinput_turbo.h"

/* 20-byte XUSB interrupt IN report (see usb_xinput.h). */
#define XINPUT_REPORT_LEN 20

/* Legal button-word mask: bit 11 is reserved by xusb22 and must stay 0. */
#define XINPUT_BTN_VALID_MASK 0xF7FFu

#define XINPUT_BTN_DPAD_UP    0x0001u
#define XINPUT_BTN_DPAD_DOWN  0x0002u
#define XINPUT_BTN_DPAD_LEFT  0x0004u
#define XINPUT_BTN_DPAD_RIGHT 0x0008u
#define XINPUT_BTN_START      0x0010u
#define XINPUT_BTN_BACK       0x0020u
#define XINPUT_BTN_L3         0x0040u
#define XINPUT_BTN_R3         0x0080u
#define XINPUT_BTN_LB         0x0100u
#define XINPUT_BTN_RB         0x0200u
#define XINPUT_BTN_GUIDE      0x0400u
#define XINPUT_BTN_A          0x1000u
#define XINPUT_BTN_B          0x2000u
#define XINPUT_BTN_X          0x4000u
#define XINPUT_BTN_Y          0x8000u

typedef struct {
    /* Latest DS4 state block, copied verbatim by xinput_map_set_state(). */
    uint8_t state[DS4_BT_INPUT_PAYLOAD_SIZE];
    /* False while the controller is not connected: report stays neutral. */
    bool connected;
    /* True once a state block for the current session has been received.
     * Cleared on every session change (gen mismatch), so a reconnect can
     * never report the previous controller's last input. */
    bool have_state;
    /* Session generation last applied by xinput_map_set_connected(). */
    uint32_t gen;
    /* Touchpad-modifier turbo state (xinput_turbo.h). */
    xinput_turbo_t turbo;
} xinput_map_t;

/* Start from a neutral state block (sticks 128, hat 8), disconnected,
 * no state for any session, generation 0. */
void xinput_map_init(xinput_map_t *m);

/* Publish the connection state and the session generation. Whenever `gen`
 * differs from the last one applied, this is a new controller session: the
 * turbo timers are reset, the stored state block is invalidated (the first
 * frames stay neutral) and the generation is adopted. Repeating the same
 * generation is a no-op apart from the `connected` flag. */
void xinput_map_set_connected(xinput_map_t *m, bool connected, uint32_t gen);

/* Publish the latest state block for the current session. It is only
 * reported while connected and after the block for this generation has
 * arrived. */
void xinput_map_set_state(xinput_map_t *m,
                          const uint8_t state[DS4_BT_INPUT_PAYLOAD_SIZE]);

/* Emit one 20-byte report for now_ms. Not connected, or no state block yet
 * for this session -> 00 14 + 18 zero bytes, turbo timers reset. */
void xinput_map_tick(xinput_map_t *m, uint32_t now_ms,
                     uint8_t out[XINPUT_REPORT_LEN]);

#endif /* XINPUT_MAP_H */
