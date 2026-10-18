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
    /* Touchpad-modifier turbo state (xinput_turbo.h). */
    xinput_turbo_t turbo;
} xinput_map_t;

/* Start from a neutral state block (sticks 128, hat 8) and disconnected. */
void xinput_map_init(xinput_map_t *m);

/* Publish the latest state block. The copy is unconditional; `connected`
 * decides whether xinput_map_tick() reports it or the neutral frame. */
void xinput_map_set_state(xinput_map_t *m,
                          const uint8_t state[DS4_BT_INPUT_PAYLOAD_SIZE],
                          bool connected);

/* Emit one 20-byte report for now_ms. Disconnected -> 00 14 + 18 zero bytes
 * and the turbo timers are reset. */
void xinput_map_tick(xinput_map_t *m, uint32_t now_ms,
                     uint8_t out[XINPUT_REPORT_LEN]);

#endif /* XINPUT_MAP_H */
