#ifndef USB_XINPUT_H
#define USB_XINPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Phase 0: minimal XUSB persona for the wireless Xbox 360 controller
 * (wired pad identity).
 *
 * Windows binds xusb22.sys through the hardware ID USB\VID_045E&PID_028E;
 * xusb22.inf contains that entry, so no MS OS 1.0 compatible-ID chain
 * (string 0xEE / 0x01 feature request) is needed and none is implemented.
 * The vendor request 0xC1/0x01 is a different thing and is implemented.
 *
 * XInput is NOT HID: there is no report descriptor. The interface is
 * vendor-specific FF/5D/01 and carries a 17-byte inline class descriptor
 * (bDescriptorType 0x21) between the interface and its two endpoints.
 */

#define USB_XINPUT_VID 0x045E
#define USB_XINPUT_PID 0x028E

/*
 * bcdDevice. Windows caches device state per VID+PID+bcdDevice in
 * HKLM\SYSTEM\CurrentControlSet\Control\usbflags, so EVERY descriptor change
 * must increment this. 0x0200 = first XUSB descriptor revision.
 */
#define USB_XINPUT_BCD_DEVICE 0x0200

/*
 * Plan A endpoint reservation (BL616 has 4 data EPs / 4 FIFOs, FIFO index =
 * EP index - 1, and an IN/OUT pair on the same index shares one FIFO):
 *
 *   audio OUT (speaker) 0x01  idx1  F0
 *   XUSB IN             0x82  idx2  F1
 *   XUSB OUT            0x03  idx3  F2
 *   mic IN              0x84  idx4  F3
 *
 * Phase 0 uses only the two XUSB endpoints. The addresses below are also
 * written into the inline 0x21 class descriptor, because xusb22 reads the
 * endpoint numbers from there; the endpoint descriptors must agree.
 */
#define USB_XINPUT_EP_IN 0x82
#define USB_XINPUT_EP_OUT 0x03
#define USB_XINPUT_EP_MPS 32
#define USB_XINPUT_INTERVAL_IN 4  /* FS: 4 ms poll = 250 Hz */
#define USB_XINPUT_INTERVAL_OUT 8 /* FS: 8 ms poll */

/* 20-byte interrupt IN report, 8-byte rumble / 3-byte LED OUT reports. */
#define USB_XINPUT_REPORT_LEN 20
#define USB_XINPUT_RUMBLE_LEN 8
#define USB_XINPUT_LED_LEN 3

/* Fixed Phase 0 report: header + A pressed, sticks centred, triggers idle. */
#define USB_XINPUT_BTN_A 0x1000u

/* Descriptor accessors for usb_gamepad.c's advance-descriptor callbacks. */
const uint8_t *usb_xinput_device_desc(void);
const uint8_t *usb_xinput_config_desc(void);
const char *usb_xinput_string_desc(uint8_t index);

/* Register interface 0 + both endpoints. Must be called before
 * usbd_initialize() and before any other interface (usbd_add_interface()
 * assigns intf_num in registration order, and the XUSB interface must be
 * interface 0 so the 0xC1/0x01 handshake can use wIndex 0). */
void usb_xinput_register(uint8_t busid);

/* Bus state hooks, called from usb_gamepad.c's usbd_event_handler. */
void usb_xinput_configured(void); /* arms OUT and the first IN report */
void usb_xinput_reset(void);
void usb_xinput_suspend(void);

/* Retry arming from task context (called from usb_task's loop). */
void usb_xinput_tick(void);

/* Publish a complete 20-byte snapshot; the pending IN transfer is re-armed
 * as soon as the endpoint is idle. Phase 2 will drive this from the DS4
 * state block; Phase 0 keeps sending the fixed report. */
int usb_xinput_send_report(const uint8_t report[USB_XINPUT_REPORT_LEN]);

/* Phase 0 diagnostic helper: A pressed (default) or released. */
void usb_xinput_fixed_a(bool pressed);

/* Diagnostics. Written in ISR context, read from task context. */
extern volatile uint32_t g_xinput_in_sent;
extern volatile uint32_t g_xinput_in_err;
extern volatile uint32_t g_xinput_out_reports;
extern volatile uint32_t g_xinput_out_arm_err;
extern volatile uint32_t g_xinput_rumble_cnt;
extern volatile uint32_t g_xinput_led_cnt;
extern volatile uint32_t g_xinput_handshake_cnt;
extern volatile uint8_t g_xinput_last_rumble_strong;
extern volatile uint8_t g_xinput_last_rumble_weak;
extern volatile uint8_t g_xinput_last_led_cmd;

#endif /* USB_XINPUT_H */
