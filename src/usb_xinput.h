#ifndef USB_XINPUT_H
#define USB_XINPUT_H

#include <stdint.h>
#include <stdbool.h>

/*
 * XUSB persona for the wireless Xbox 360 controller (wired pad identity).
 * Phase 2: the 20-byte input report is produced by src/xinput_map.c from the
 * DS4 state block; this module only frames it and drives the OUT callbacks.
 *
 * The identity is pid.codes 0x1209:0x5D01, which xusb22.inf does not match by
 * root hardware ID. Windows therefore enumerates the composite device with
 * usbccgp and binds xusb22 to interface 0 through the MS OS 1.0 compatible ID
 * USB\MS_COMP_XUSB10 (string 0xEE + vendor request 0x20, wIndex 4). The XUSB
 * vendor request 0xC1/0x01 is a different thing and is implemented too.
 *
 * XInput is NOT HID: there is no report descriptor. The interface is
 * vendor-specific FF/5D/01 and carries a 17-byte inline class descriptor
 * (bDescriptorType 0x21) between the interface and its two endpoints.
 */

#define USB_XINPUT_VID 0x1209 /* pid.codes shared VID */
#define USB_XINPUT_PID 0x5D01 /* XUSB subclass 0x5D + protocol 0x01 */

/*
 * bcdDevice. Windows caches device state per VID+PID+bcdDevice in
 * HKLM\SYSTEM\CurrentControlSet\Control\usbflags, so EVERY descriptor change
 * must increment this. 0x0200 = first XUSB descriptor revision,
 * 0x0201 = 1209:5D01 identity + MS OS 1.0 XUSB10 compatible ID.
 */
#define USB_XINPUT_BCD_DEVICE 0x0201

/*
 * bMS_VendorCode of the MS OS 1.0 string descriptor 0xEE. Must NOT be 0x01:
 * that is the XUSB handshake bRequest (0xC1/0x01/wValue 0x0100).
 */
#define USB_XINPUT_MSOSV1_VENDOR_CODE 0x20

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

/* Output callbacks. Both run in USB interrupt context (OUT completion):
 * they must be ISR-safe — no logging, no allocation, no blocking calls. */
typedef void (*usb_xinput_rumble_cb_t)(uint8_t strong, uint8_t weak);
typedef void (*usb_xinput_led_cb_t)(uint8_t cmd);

/* XUSB button-word bit 12 (A). The report content is built in
 * src/xinput_map.c (XINPUT_BTN_A); this constant keeps the wired-pad A bit
 * declared here for the report-layout self-check
 * (tools/xinput_desc_check.py). */
#define USB_XINPUT_BTN_A 0x1000u

/* Descriptor accessors for usb_gamepad.c's advance-descriptor callbacks. */
const uint8_t *usb_xinput_device_desc(void);
const uint8_t *usb_xinput_config_desc(void);

/* MS OS 1.0 descriptor set (string 0xEE + compatible-ID blob, XUSB10 on
 * interface 0). usb_gamepad_init() installs it into usb_desc.msosv1_descriptor
 * for the XINPUT persona only. */
struct usb_msosv1_descriptor;
const struct usb_msosv1_descriptor *usb_xinput_msosv1_desc(void);

/* The 40-byte interface block of the XUSB-only configuration (Interface 9 +
 * inline 0x21 class descriptor 17 + EP IN 7 + EP OUT 7), for splicing into
 * the composite XUSB + audio configuration. Points into the single source
 * array in usb_xinput.c; the caller copies `*len` bytes from it.
 * usb_xinput_config_desc() stays the complete 49-byte XUSB-only config. */
const uint8_t *usb_xinput_interface_block(uint16_t *len);
const char *usb_xinput_string_desc(uint8_t index);

/* Register interface 0 + both endpoints. Must be called before
 * usbd_initialize() and before any other interface (usbd_add_interface()
 * assigns intf_num in registration order, and the XUSB interface must be
 * interface 0 so the 0xC1/0x01 handshake can use wIndex 0). */
void usb_xinput_register(uint8_t busid);

/* Bus state hooks, called from usb_gamepad.c's usbd_event_handler. */
void usb_xinput_configured(void); /* arms OUT; IN follows the mapper snapshot */
void usb_xinput_reset(void);
void usb_xinput_suspend(void);

/* Retry arming from task context (called from usb_task's loop). */
void usb_xinput_tick(void);

/* Publish a complete 20-byte snapshot; the pending IN transfer is re-armed
 * as soon as the endpoint is idle. The mapper (src/xinput_map.c) owns the
 * content and calls this once per 4 ms tick. */
int usb_xinput_send_report(const uint8_t report[USB_XINPUT_REPORT_LEN]);

/* Register the XUSB OUT consumers. Passing NULL for either clears it.
 * Called once from usb_gamepad_init(); the callbacks are invoked from the
 * OUT completion ISR. */
void usb_xinput_set_output_cbs(usb_xinput_rumble_cb_t r,
                               usb_xinput_led_cb_t l);

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
