/*
 * DS4Dongle BL618 — USB device side (CherryUSB).
 *
 * Presents the dongle as a wired DualShock 4 v2 (054C:09CC) with the
 * byte-identical 507-byte HID report descriptor of a real DS4 v2.
 *
 * P2 scope: HID gamepad (intf 3) + UAC1 audio (intf 0/1/2), descriptors
 * byte-identical to a real DS4 v2. Output reports arrive via EP0
 * SET_REPORT (report 0x05); there is no interrupt OUT endpoint.
 * Speaker: ISO OUT EP 0x01 (32kHz stereo S16). Headset mic: ISO IN EP 0x82
 * (16kHz mono S16). Config feature reports 0xF6-0xF9 are deliberately NOT
 * declared in the HID descriptor (stealth, identical to DS4Dongle).
 */

#include "usb_gamepad.h"
/* Shared, SDK-free visibility policy: usb_phy_main_connect_if_due() applies
 * the same grace rule main (and the host test) uses, inside its masked
 * snapshot-and-publish region. */
#include "spk_recovery_policy.h"
#include "compiler/compiler_ld.h"
#include "bt_hid_host.h"
#include "ds4_protocol.h"
#include "ds4_hid_desc.h"
#include "ds4_usb_audio.h"
#include "ds4_usb_audio_desc.h"
#include "config.h"
#include "usbd_core.h"
#include "usbd_hid.h"
#include "debug_console.h"
#include "log_event.h"
#include "bl616_glb.h"
#include "bflb_efuse.h"
#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "board.h"
#include <string.h>
#include <stdio.h>
#include "board_config.h"
#include "debug_log.h"
#include "FreeRTOS.h"
#include "task.h"

#if defined(BOARD_LCTECH_616)
  #ifdef FORCE_FS_MODE
    #define FIRMWARE_VERSION "LCT616-DS4 0.1"
  #else
    #define FIRMWARE_VERSION "LCT616-DS4 0.1H"
  #endif
#elif defined(BOARD_M0S_DOCK)
#define FIRMWARE_VERSION "M0S-DS4 0.1"
#else
#define FIRMWARE_VERSION "BL618-DS4 0.1"
#endif

#define USBD_MAX_POWER      250
#define USBD_LANGID         0x0409

/* Config descriptor (P2): Config(9) + Audio(184) + Gamepad intf(9) +
 * HID desc(9) + EP IN(7). Audio descriptor is spliced in at init.
 * One fixed layout for every build: gamepad IN stays 0x84, audio keeps
 * speaker OUT 0x01 / mic IN 0x82. */
#define USB_GAMEPAD_EP_IN      0x84

#ifdef DEBUG_CONSOLE
/* Host-facing debug log report (src/usb_gamepad.c GET_REPORT handler): the
 * DS4's own vendor feature report 0xB4, already declared in
 * ds4_hid_desc.h — 63 data bytes + report ID = 64 bytes on the wire. */
#define DS4_DEBUG_LOG_REPORT_ID   0xB4
#define DS4_DEBUG_LOG_REPORT_LEN  64
#endif

#ifdef DS4_FEATURE_AUDIO
#define DS4_AUDIO_DESC_LEN   DS4_AUDIO_DESC_SIZE
#else
#define DS4_AUDIO_DESC_LEN   0   /* gamepad-only build */
#endif
#ifdef DS4_NO_GAMEPAD
#define DS4_GAMEPAD_DESC_LEN 0
#else
#define DS4_GAMEPAD_DESC_LEN 25  /* intf 9 + HID desc 9 + EP 7 */
#endif
#define USB_CONFIG_SIZE      (9 + DS4_AUDIO_DESC_LEN + DS4_GAMEPAD_DESC_LEN)
#define AUDIO_DESC_OFF       9
#define GAMEPAD_DESC_OFF     (AUDIO_DESC_OFF + DS4_AUDIO_DESC_LEN)
#define HID_INTF_OFF         GAMEPAD_DESC_OFF
#define HID_DESC_OFF         (HID_INTF_OFF + 9)
#define HID_WDLEN_OFF        (HID_DESC_OFF + 7)
#define BINTERVAL_EP_IN_OFF  (HID_DESC_OFF + 9 + 6)

#ifdef DS4_FEATURE_AUDIO
#define DS4_N_AUDIO_INTF     3
#else
#define DS4_N_AUDIO_INTF     0
#endif
#ifdef DS4_NO_GAMEPAD
#define DS4_N_GAMEPAD_INTF   0
#else
#define DS4_N_GAMEPAD_INTF   1
#endif
#define DS4_N_INTERFACES     (DS4_N_AUDIO_INTF + DS4_N_GAMEPAD_INTF)

static volatile bool usb_config_save_pending = false;
static volatile bool usb_reset_pending = false;

/* R01/R18: the HID completion callback and the USB event handler run in ISR
 * context. They record counters only — formatting/printing there blocks the
 * interrupt. Read these from task context when diagnosing. */
volatile uint32_t g_usb_in_sent = 0;      /* input reports armed */
volatile uint32_t g_usb_in_err = 0;       /* input report arm failures */
volatile uint32_t g_usb_evt_configured = 0;
volatile uint32_t g_usb_evt_reset = 0;
volatile uint32_t g_usb_evt_suspend = 0;
volatile uint32_t g_usb_evt_resume = 0;
volatile uint32_t g_usb_evt_other = 0;
volatile uint32_t g_usb_soft_conn = 0;
volatile uint32_t g_usb_soft_disc = 0;
volatile uint32_t g_usb_poll_set = 0;
volatile uint32_t g_hid_get_report_cnt = 0;   /* GET_REPORT in EP0 path */
volatile uint32_t g_hid_set_report_cnt = 0;   /* SET_REPORT config command */
volatile uint32_t g_usb_feature_err = 0;      /* deferred feature GET dropped */

/* Pending feature GET request, deferred from the EP0 path to task context.
 *
 * Claim/commit protocol. The publisher (usbd_hid_get_report(), i.e. the host's
 * GET_REPORT that missed the cache) writes the report id and bumps a monotonic
 * sequence inside one bflb_irq_save() window. The consumer
 * (usb_gamepad_process_deferred()) snapshots {rid, seq} under the same mask,
 * releases the mask while bt_hid_host_get_feature() runs, and afterwards
 * re-acquires it and clears the request ONLY if pending_feature_seq still
 * equals the claimed sequence.
 *
 * Consequently a publication that interleaves with an in-flight send (a newer
 * request B arriving while A is being sent) has a different sequence and is
 * left pending: completing or failing A can no longer erase B. A send that
 * returns BT_HID_SEND_WOULD_BLOCK leaves the request pending unconditionally
 * (retryable, nothing was accepted).
 *
 * rid 0 is not a valid feature report id, so it doubles as "no pending work".
 * The sequence is free-running uint32 and only ever compared for equality;
 * the single wrap (~4G publications) is not reachable in practice.
 *
 * Only the 1-byte report id is retained across a retry: the GET_REPORT PDU
 * carries nothing else and bt_hid_host_get_feature() builds/consumes its own
 * buffer per call, so no payload copy has to be kept here. */
static volatile uint8_t  pending_feature_rid = 0;
static volatile uint32_t pending_feature_seq = 0;

/* Publish (or republish) a deferred feature GET. Callable from EP0/ISR
 * context: two stores under one short mask, no allocation, no logging. */
static void publish_pending_feature(uint8_t rid)
{
    uintptr_t key = bflb_irq_save();

    pending_feature_rid = rid;
    pending_feature_seq++;

    bflb_irq_restore(key);
}

static uint8_t device_desc[] = {
    0x12,                    /* bLength */
    0x01,                    /* bDescriptorType: Device */
    0x00, 0x02,              /* bcdUSB: 2.00 */
    0x00,                    /* bDeviceClass */
    0x00,                    /* bDeviceSubClass */
    0x00,                    /* bDeviceProtocol */
    0x40,                    /* bMaxPacketSize0 */
    (USB_GAMEPAD_VID & 0xFF), (USB_GAMEPAD_VID >> 8),
    (USB_GAMEPAD_PID & 0xFF), (USB_GAMEPAD_PID >> 8),
    0x00, 0x01,              /* bcdDevice: 1.00 */
    0x01,                    /* iManufacturer */
    0x02,                    /* iProduct */
    0x00,                    /* iSerialNumber: none (patched when enable_usb_sn) */
    0x01,                    /* bNumConfigurations */
};

static uint8_t config_desc[USB_CONFIG_SIZE] = {
    /* Configuration Descriptor (9 bytes) */
    0x09, 0x02,
    USB_CONFIG_SIZE & 0xFF, (USB_CONFIG_SIZE >> 8) & 0xFF,
    DS4_N_INTERFACES,        /* bNumInterfaces: audio + gamepad as built */
    0x01,                    /* bConfigurationValue */
    0x00,                    /* iConfiguration */
    0xC0,                    /* bmAttributes: self-powered (remote-wake added in P3) */
    USBD_MAX_POWER,          /* bMaxPower: 500mA */
    /* [AUDIO_DESC_OFF ..] 184 bytes: DS4 UAC1 audio (intf 0/1/2)
     * [HID_INTF_OFF ..]  25 bytes: gamepad intf 3 + HID desc + EP IN
     * all spliced from their arrays in usb_gamepad_init() */
};

/* Gamepad interface block: Interface 3 + HID descriptor + EP IN
 * (wDescriptorLength patched in init before splicing). */
static const uint8_t gamepad_desc_block[] = {
    0x09, 0x04,
    USB_INTF_GAMEPAD,        /* bInterfaceNumber */
    0x00,                    /* bAlternateSetting */
    0x01,                    /* bNumEndpoints: IN only (output via EP0 SET_REPORT) */
    0x03,                    /* bInterfaceClass: HID */
    0x00,                    /* bInterfaceSubClass */
    0x00,                    /* bInterfaceProtocol */
    0x00,                    /* iInterface */

    /* HID Descriptor (Gamepad) */
    0x09, 0x21,
    0x11, 0x01,              /* bcdHID: 1.11 */
    0x00,                    /* bCountryCode */
    0x01,                    /* bNumDescriptors */
    0x22,                    /* bDescriptorType: Report */
    0x00, 0x00,              /* wDescriptorLength patched in init */

    /* EP IN (Gamepad) */
    0x07, 0x05,
    USB_GAMEPAD_EP_IN,       /* bEndpointAddress (0x84) */
    0x03,                    /* bmAttributes: Interrupt */
    USB_GAMEPAD_EP_MPS, 0x00,
    USB_GAMEPAD_INTERVAL_MS,
};

/* ---- Advance-mode descriptor callbacks ---- */

static const uint8_t *desc_device_cb(uint8_t speed)
{
    (void)speed;
    device_desc[16] = config_usb_sn() ? 0x03 : 0x00;
    return device_desc;
}

static const uint8_t *desc_config_cb(uint8_t speed)
{
    (void)speed;
    return config_desc;
}

static const uint8_t device_qualifier_desc[] = {
    0x0A,        /* bLength */
    0x06,        /* bDescriptorType: Device Qualifier */
    0x00, 0x02,  /* bcdUSB: 2.00 */
    0x00, 0x00, 0x00,
    0x40,        /* bMaxPacketSize0: 64 */
    0x01, 0x00,
};

static const uint8_t *desc_qualifier_cb(uint8_t speed)
{
    (void)speed;
    return device_qualifier_desc;
}

/* R09: Full-Speed only. There is deliberately no OTHER_SPEED descriptor
 * callback: CherryUSB serves USB_DESCRIPTOR_TYPE_OTHER_SPEED without an HS
 * guard, and the old callback returned a copy of the FS configuration with
 * only bDescriptorType patched to 0x07. With no HS host possible that was a
 * bogus "other speed" configuration. Leaving the callback NULL makes
 * CherryUSB stall the request (same as the device qualifier path under
 * !CONFIG_USB_HS). */

static char serial_str[24]; /* 16 hex + nul */
static bool serial_init = false;

static void serial_str_build(void)
{
    uint8_t chipid[8];
    bflb_efuse_get_chipid(chipid);
    snprintf(serial_str, sizeof(serial_str),
             "%02X%02X%02X%02X%02X%02X%02X%02X",
             chipid[0], chipid[1], chipid[2], chipid[3],
             chipid[4], chipid[5], chipid[6], chipid[7]);
    serial_init = true;
}

static const char *desc_string_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    switch (index) {
    case 0: return "\x09\x04";
    case 1: return "Sony Interactive Entertainment";
    case 2: return "Wireless Controller";
    case 3:
        /* R01: no snprintf/printf in the descriptor callback (EP0 path).
         * The serial is built once in usb_gamepad_init(). */
        return serial_init ? serial_str : "";
    default: return NULL;
    }
}

static const struct usb_descriptor usb_desc = {
    .device_descriptor_callback          = desc_device_cb,
    .config_descriptor_callback          = desc_config_cb,
    .device_quality_descriptor_callback  = desc_qualifier_cb,
    .other_speed_descriptor_callback     = NULL,   /* FS-only: see note above */
    .string_descriptor_callback          = desc_string_cb,
    .msosv1_descriptor                   = NULL,
    .msosv2_descriptor                   = NULL,
    .webusb_url_descriptor               = NULL,
    .bos_descriptor                      = NULL,
};

/* Gamepad interface / endpoints */
static struct usbd_interface hid_intf;
static struct usbd_endpoint  hid_ep_in;

static volatile bool usb_configured = false;
static volatile bool ep_in_busy = false;
static usb_gamepad_output_cb_t output_callback = NULL;

/* Suspend/resume hooks for wake module (P3) */
static void (*hook_suspend)(void)    = NULL;
static void (*hook_resume)(void)     = NULL;
static void (*hook_configured)(void) = NULL;

static USB_NOCACHE_RAM_SECTION uint8_t usb_in_buf[USB_GAMEPAD_EP_MPS];

/* N02: pending HID report ownership.
 *
 * Writers: usb_gamepad_send_raw_input() is called from two task contexts
 * (the main loop and the BT HID state callback in main.c) while the IN
 * completion ISR reads it through try_send_pending(). The payload is never
 * mutated in place piecemeal: publish_pending() installs a complete 63-byte
 * snapshot in one IRQ-masked step, so a reader sees either the previous
 * complete report or the new complete report — never a mix of both.
 * try_send_pending() copies the published snapshot under the same mask,
 * because it also runs in task context (the writer calls it directly) and a
 * higher-priority writer could otherwise republish mid-copy.
 * Nothing here blocks, allocates or prints; only the 63-byte copy is done
 * with interrupts masked. */
static volatile uint8_t pending_payload[DS4_USB_INPUT_PAYLOAD_LEN];
static volatile bool    pending_active = false;

/* Publish a complete report as the current pending snapshot. Callable from any
 * task/ISR context; the copy is bounded and interrupt masking is restored. */
#ifndef DS4_NO_GAMEPAD
ATTR_TCM_SECTION
static void publish_pending(const uint8_t *payload)
{
    uintptr_t key = bflb_irq_save();

    memcpy((void *)pending_payload, payload, DS4_USB_INPUT_PAYLOAD_LEN);
    pending_active = true;
    bflb_irq_restore(key);
}
#endif

ATTR_TCM_SECTION
static void try_send_pending(void)
{
    if (!pending_active || !usb_configured || ep_in_busy)
         return;

    /* Re-check and copy under one mask: between the fast-path test above and
     * here the completion ISR may have armed a transfer, and a task-context
     * writer must not observe a snapshot that is being replaced underneath it.
     * The endpoint submit stays outside the critical section. */
    uintptr_t key = bflb_irq_save();
    if (!pending_active || !usb_configured || ep_in_busy) {
        bflb_irq_restore(key);
        return;
    }
    usb_in_buf[0] = DS4_USB_INPUT_REPORT_ID;
    memcpy(usb_in_buf + 1, (const void *)pending_payload,
           DS4_USB_INPUT_PAYLOAD_LEN);
    ep_in_busy = true;
    bflb_irq_restore(key);

    int ret = usbd_ep_start_write(0, USB_GAMEPAD_EP_IN, usb_in_buf,
                                  DS4_USB_INPUT_PAYLOAD_LEN + 1);
    if (ret < 0) {
        ep_in_busy = false;
        g_usb_in_err++;
    } else {
        g_usb_in_sent++;
    }
}

ATTR_TCM_SECTION
static void hid_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    ep_in_busy = false;
    try_send_pending();
}

#ifndef DS4_NO_GAMEPAD
/* N01: SET_INTERFACE(hid, 0) is legal and HID alt 0 owns the IN endpoint, so
 * the core reconfigures that endpoint instead of closing it. usbd_set_endpoint()
 * retires the transfer that may still be active there (usbd_ep_cancel() before
 * usbd_ep_open()) and notifies the class after that point.
 *
 * Clearing ep_in_busy here is only a bookkeeping release: it is NOT what makes
 * the next submit safe. Until the cancellation witness settles, the endpoint is
 * in BFLB_EP_CANCEL_PENDING and usbd_ep_start_write() rejects the submission
 * with a negative value, so the producer cannot arm a transfer whose completion
 * dispatch would drop (which would leave ep_in_busy set forever with no
 * completion left to clear it). try_send_pending() counts that rejection and
 * retries on the next report; once the witness settles (a clean G3 pass or the
 * driver's hardware-state witness poll) the submit succeeds normally. The
 * driver's PENDING test and VDMA arm share one interrupt-masked region with
 * usbd_ep_cancel(), so a SET_INTERFACE cancel cannot interleave between them.
 * Runs in EP0/ISR context - flag only, never printf. */
static void hid_notify_handler(uint8_t busid, uint8_t event, void *arg)
{
    const struct usb_interface_descriptor *intf = arg;

    (void)busid;

    if (event != USBD_EVENT_SET_INTERFACE || intf == NULL)
        return;
    if (intf->bInterfaceNumber != USB_INTF_GAMEPAD)
        return;

    ep_in_busy = false;
}
#endif

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_CONFIGURED:
        usb_configured = true;
        ep_in_busy = false;
        g_usb_evt_configured++;   /* ISR context: counter, no printf */
        log_event_put(LOG_EVT_USB_STATE, 0, 0);
        if (hook_configured)
            hook_configured();
        break;
    case USBD_EVENT_RESET:
        usb_configured = false;
        ep_in_busy = false;
        /* R08: a bus reset drops host intent too — the host will re-enumerate
         * and re-open the audio interfaces. */
        usb_audio_reset();
        g_usb_evt_reset++;
        log_event_put(LOG_EVT_USB_STATE, 1, 0);
        break;
    case USBD_EVENT_SUSPEND:
        usb_configured = false;
        ep_in_busy = false;
        /* R08: keep the host's alt state, stop endpoint I/O only. */
        usb_audio_suspend();
        g_usb_evt_suspend++;
        log_event_put(LOG_EVT_USB_STATE, 2, 0);
        if (hook_suspend)
            hook_suspend();
        break;
    case USBD_EVENT_RESUME:
        usb_configured = true;
        ep_in_busy = false;
        /* R08: restore endpoint I/O for whatever the host still has open. */
        usb_audio_resume();
        g_usb_evt_resume++;
        log_event_put(LOG_EVT_USB_STATE, 3, 0);
        if (hook_resume)
            hook_resume();
        break;
    default:
        g_usb_evt_other++;
        break;
    }
}

ATTR_TCM_SECTION
void usb_gamepad_process_deferred(void)
{
    if (usb_reset_pending) {
        usb_reset_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x03: USB reconnect via reset\n");
        vTaskDelay(pdMS_TO_TICKS(100));
        GLB_SW_System_Reset();
    }
    if (usb_config_save_pending) {
        usb_config_save_pending = false;
        config_save();
        LOG_INF("[USB] CMD 0x02: config saved\n");
    }

    /* Claim: snapshot the publication under the mask. The send itself runs
     * unmasked; the commit below is guarded by the captured sequence so a
     * newer publication (host asked for another report while this one was in
     * flight) is never cleared by the completion of an older request. */
    uintptr_t fkey  = bflb_irq_save();
    uint8_t   frid  = pending_feature_rid;
    uint32_t  fseq  = pending_feature_seq;
    bflb_irq_restore(fkey);

    if (frid != 0) {
        int err = bt_hid_host_get_feature(frid);
        if (err == 0) {
            /* Commit this generation only. */
            fkey = bflb_irq_save();
            if (pending_feature_seq == fseq)
                pending_feature_rid = 0;
            bflb_irq_restore(fkey);
            LOG_INF("[USB] Deferred GET_REPORT(Feature 0x%02x) → BT\n", frid);
        } else if (err != BT_HID_SEND_WOULD_BLOCK) {
            /* Hard error: clear only the claimed generation and count it
             * once. If a newer request was published meanwhile it survives. */
            fkey = bflb_irq_save();
            if (pending_feature_seq == fseq)
                pending_feature_rid = 0;
            bflb_irq_restore(fkey);
            g_usb_feature_err++;
            LOG_INF("[USB] Deferred GET_REPORT(Feature 0x%02x) failed (%d)\n",
                    frid, err);
        }
        /* BT_HID_SEND_WOULD_BLOCK: control TX path is temporarily out of
         * buffer/credit and the PDU was not accepted. Leave pending_feature_rid
         * set regardless of the sequence — the request is retried on the next
         * keepalive/USB task pass. Only the 1-byte report id is retained: the
         * GET_REPORT PDU carries nothing else, and
         * bt_hid_host_get_feature() allocates and consumes its own net_buf on
         * every call, so no payload copy is kept here and none is duplicated
         * per retry. Not counted as a failure. */
    }
}

int usb_gamepad_init(usb_gamepad_output_cb_t output_cb)
{
    output_callback = output_cb;
    memset(usb_in_buf, 0, sizeof(usb_in_buf));

    LOG_INF("[USB-INIT] === USB Initialization Start ===\n");

    /* Splice the DS4 UAC1 audio descriptor after the config header */
#if defined(DS4_FEATURE_AUDIO)
    {
        uint16_t alen = 0;
        const uint8_t *adesc = usb_audio_get_desc(&alen);
        memcpy(config_desc + AUDIO_DESC_OFF, adesc, alen);
    }
#endif
    /* Splice the gamepad interface block after the audio descriptor, then
     * patch the DS4 v2 report-descriptor length into the HID descriptor */
#ifndef DS4_NO_GAMEPAD
    memcpy(config_desc + HID_INTF_OFF, gamepad_desc_block,
           sizeof(gamepad_desc_block));
#endif
#ifndef DS4_NO_GAMEPAD
    config_desc[HID_WDLEN_OFF]     = DS4_HID_REPORT_DESC_SIZE & 0xFF;
    config_desc[HID_WDLEN_OFF + 1] = (DS4_HID_REPORT_DESC_SIZE >> 8) & 0xFF;
    usb_gamepad_set_polling_rate(config_polling_mode());
#endif

    /* R01: build the serial string once, here, not inside the descriptor
     * callback (EP0 path). Needed for any build that advertises iSerialNumber. */
    serial_str_build();

    usbd_desc_register(0, &usb_desc);
    LOG_INF("[USB-INIT] Descriptors registered (dev=%uB, cfg=%uB)\n",
           (unsigned)sizeof(device_desc), (unsigned)sizeof(config_desc));

    /* Interfaces 0/1/2: DS4 UAC1 audio (must be added before the gamepad —
     * CherryUSB numbers interfaces in registration order) */
#if defined(DS4_FEATURE_AUDIO)
    usb_audio_register(0);
    LOG_INF("[USB-INIT] Audio UAC1 intfs added (spk EP 0x01 / mic EP 0x82)\n");
#endif

#ifndef DS4_NO_GAMEPAD
    /* Interface 3: DS4 v2 gamepad (507-byte byte-identical report desc in
     * every build — DEBUG_CONSOLE adds no descriptor items) */
    usbd_add_interface(0, usbd_hid_init_intf(
        0, &hid_intf, hid_report_desc_ds4, DS4_HID_REPORT_DESC_SIZE));
    /* N01: the HID class driver leaves notify_handler NULL; without it the
     * app cannot learn that a SET_INTERFACE rebuilt/removed its IN endpoint. */
    hid_intf.notify_handler = hid_notify_handler;
    LOG_INF("[USB-INIT] Gamepad HID intf added (report_desc=%uB)\n",
           (unsigned)DS4_HID_REPORT_DESC_SIZE);

    hid_ep_in.ep_addr = USB_GAMEPAD_EP_IN;
    hid_ep_in.ep_cb = hid_ep_in_handler;
    usbd_add_endpoint(0, &hid_ep_in);
    LOG_INF("[USB-INIT] Endpoints: GP_IN=0x%02x (output via EP0)\n",
           USB_GAMEPAD_EP_IN);
#endif

#ifdef DEBUG_CONSOLE
    /* Set up the log ring + console tap (optional HID feature 0xB4 reader;
     * UART remains the primary log path). */
    debug_console_init();
#endif

    /* Read registers BEFORE usbd_initialize */
    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
        LOG_INF("[USB-INIT] PRE-init: DEV_CTL=0x%08lx PHY_TST=0x%08lx\n",
               (unsigned long)*dev_ctl, (unsigned long)*phy_tst);
    }

    LOG_INF("[USB-INIT] Calling usbd_initialize(0, 0x%08lx, handler)...\n",
           (unsigned long)USB_BASE);
    usbd_initialize(0, USB_BASE, usbd_event_handler);
    LOG_INF("[USB-INIT] usbd_initialize returned\n");

    {
        volatile uint32_t *dev_ctl = (volatile uint32_t *)(USB_BASE + 0x100);
        uint32_t v = *dev_ctl;
        v |= (1 << 2);  /* USB_GLINT_EN_HOV */
        v &= ~(1 << 25); /* Clear USB_LPM_EN — refuse L1 power state */
        v &= ~(1 << 26); /* Clear USB_LPM_ACCEPT — NYET all LPM requests */
        *dev_ctl = v;
    }

    LOG_INF("[USB-INIT] === USB Ready. Waiting for host to send RESET ===\n");
#if USB_NATIVE_TYPEC
    LOG_INF("[USB-INIT] USB Type-C native — no wiring needed\n");
#else
    LOG_INF("[USB-INIT] If no [USB-EVT] RESET appears, check wiring:\n");
    LOG_INF("[USB-INIT]   Kit Pin 37 (USB_DM) -> cable D- (white)\n");
    LOG_INF("[USB-INIT]   Kit Pin 38 (USB_DP) -> cable D+ (green)\n");
    LOG_INF("[USB-INIT]   Kit Pin 21/22 (GND) -> cable GND (black)\n");
#endif
    return 0;
}

void usb_gamepad_set_suspend_hooks(void (*on_suspend)(void),
                                   void (*on_resume)(void),
                                   void (*on_configured)(void))
{
    hook_suspend    = on_suspend;
    hook_resume     = on_resume;
    hook_configured = on_configured;
}

void usb_gamepad_set_polling_rate(uint8_t mode)
{
#ifdef DS4_NO_GAMEPAD
    (void)mode;   /* no HID interface in this build: nothing to patch */
    return;
#else
    uint8_t interval;
#ifdef FORCE_FS_MODE
    /* FS: bInterval = N ms — DS4Dongle's mapping
     * (mode 0 = stock DS4 v2 cadence) */
    switch (mode) {
    case 0:  interval = 5; break;  /* FS 5ms — stock DS4 v2 */
    case 1:  interval = 2; break;  /* FS 2ms = 500Hz */
    default: interval = 1; break;  /* FS 1ms = 1000Hz (real-time) */
    }
#else
    /* HS: bInterval = 2^(N-1) microframes */
    switch (mode) {
    case 0:  interval = 5; break;  /* 16µf = 2ms → ~250Hz effective */
    case 1:  interval = 4; break;  /* 8µf  = 1ms → ~500Hz effective */
    default: interval = 3; break;  /* 4µf  = 0.5ms → real-time */
    }
#endif
    config_desc[BINTERVAL_EP_IN_OFF] = interval;
    g_usb_poll_set++;   /* EP0 path: counter only */
#endif
}

ATTR_TCM_SECTION
int usb_gamepad_send_raw_input(const uint8_t *payload)
{
#ifdef DS4_NO_GAMEPAD
    (void)payload;
    return 0;
#else
    if (payload == NULL)
        return -1;

    /* N02: publish the whole report atomically, then let the common path arm
     * the IN transfer if the endpoint is idle. */
    publish_pending(payload);
    try_send_pending();
    return 0;
#endif
}

bool usb_gamepad_is_ready(void)
{
    return usb_configured && !ep_in_busy;
}

/* ---------- HID class request callbacks (feature report handling) ---------- */

/* +1 for Report ID prefix required by CherryUSB */
static uint8_t feature_resp_buf[FEATURE_DATA_MAX + 1];

/* N05: EP0 never dereferences the BT feature cache. Calibration is converted
 * from this private snapshot so the BT payload cannot change mid-conversion. */
static uint8_t feature_calib_snap[36];

static bool is_dongle_cmd(uint8_t report_id)
{
    return (report_id >= 0xF6 && report_id <= 0xF9);
}

/*
 * Synthesize USB report 0x12 (15 bytes): controller MAC (reversed) +
 * {0x08, 0x25, 0x00} + paired-host MAC (reversed).
 */
static uint16_t build_pairing_info(uint8_t *buf)
{
    uint8_t mac[6], host[6];
    memset(buf, 0, DS4_PAIRING_INFO_SIZE);
    if (!bt_hid_host_get_connected_addr(mac))
        memset(mac, 0, 6);
    bt_hid_host_get_local_addr(host);
    /* MAC byte order is reversed (LSB first) in the report. */
    for (int i = 0; i < 6; i++)
        buf[i] = mac[5 - i];
    buf[6] = 0x08;
    buf[7] = 0x25;
    buf[8] = 0x00;
    for (int i = 0; i < 6; i++)
        buf[9 + i] = host[5 - i];
    return DS4_PAIRING_INFO_SIZE;
}

/*
 * Static fallback for DS4 feature report 0x05 (calibration). Used when
 * the BT cache is cold so host drivers can probe without hanging.
 */
static bool ds4_feature_fallback(uint8_t report_id, uint8_t *buf, uint32_t *len)
{
    if (report_id != DS4_USB_REPORT_CALIBRATION)
        return false;

    memset(buf, 0, 36);
    int16_t *cal = (int16_t *)(buf);
    cal[0]  = 0;       /* gyro_pitch_bias */
    cal[1]  = 0;       /* gyro_yaw_bias   */
    cal[2]  = 0;       /* gyro_roll_bias  */
    cal[3]  = 8192;    /* gyro_pitch_plus  */
    cal[4]  = -8192;   /* gyro_pitch_minus */
    cal[5]  = 8192;    /* gyro_yaw_plus    */
    cal[6]  = -8192;   /* gyro_yaw_minus   */
    cal[7]  = 8192;    /* gyro_roll_plus   */
    cal[8]  = -8192;   /* gyro_roll_minus  */
    cal[9]  = 1024;    /* gyro_speed_plus  */
    cal[10] = -1024;   /* gyro_speed_minus */
    cal[11] = 8192;    /* acc_x_plus   */
    cal[12] = -8192;   /* acc_x_minus  */
    cal[13] = 8192;    /* acc_y_plus   */
    cal[14] = -8192;   /* acc_y_minus  */
    cal[15] = 8192;    /* acc_z_plus   */
    cal[16] = -8192;   /* acc_z_minus  */
    *len = 36;
    return true;
}

ATTR_TCM_SECTION
void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid;

    if (report_type != 0x03) {
        *len = 0;
        return;
    }

#ifdef DEBUG_CONSOLE
    /* 0xB4: debug log read (pops buffered console output). 0xB4 is a real
     * DS4 report and the 507-byte descriptor already declares it (usage page
     * 0xFF80, usage 0x55, 63 x 8 bits Feature), i.e. 64 bytes on the wire
     * including the report ID. Nothing is added to the descriptor, so the
     * report descriptor is byte-identical in every build.
     *
     * Safe to repurpose: it is a genuine DS4 declaration, this firmware does
     * not implement 0xB4 for any other purpose, and Linux hid-sony does not
     * use it in normal operation. The override is compile-time gated: in
     * release builds this branch does not exist and 0xB4 falls through to
     * the previous behaviour, unchanged.
     *
     * Windows rejects GET_REPORT for an undeclared report ID before the
     * handler runs (and a short EP0 response is rejected too), hence a
     * declared report and always exactly DS4_DEBUG_LOG_REPORT_LEN bytes:
     * report ID, the popped log bytes, then zero padding. */
    if (report_id == DS4_DEBUG_LOG_REPORT_ID) {
        memset(feature_resp_buf, 0, DS4_DEBUG_LOG_REPORT_LEN);
        feature_resp_buf[0] = DS4_DEBUG_LOG_REPORT_ID;
        (void)debug_console_read(feature_resp_buf + 1,
                                 DS4_DEBUG_LOG_REPORT_LEN - 1);
        *data = feature_resp_buf;
        *len  = DS4_DEBUG_LOG_REPORT_LEN;   /* 64 bytes, never short */
        return;
    }
#endif

    /* Dongle config reports 0xF6-0xF9 (NOT declared in the HID descriptor;
     * Windows drops them, Linux/macOS/ChromeOS pass them — like DS4Dongle) */
    if (is_dongle_cmd(report_id)) {
        feature_resp_buf[0] = report_id;

        if (report_id == 0xF7) {
            struct config_body *cfg = config_get();
            uint16_t cfg_len = sizeof(struct config_body);
            if (cfg_len > FEATURE_DATA_MAX)
                cfg_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, cfg, cfg_len);
            *data = feature_resp_buf;
            *len  = 1 + cfg_len;
            g_hid_get_report_cnt++;   /* EP0 path: counter only */
        } else if (report_id == 0xF8) {
            uint16_t ver_len = strlen(FIRMWARE_VERSION);
            if (ver_len > FEATURE_DATA_MAX)
                ver_len = FEATURE_DATA_MAX;
            memcpy(feature_resp_buf + 1, FIRMWARE_VERSION, ver_len);
            *data = feature_resp_buf;
            *len  = 1 + ver_len;
            g_hid_get_report_cnt++;
        } else if (report_id == 0xF9) {
            extern int8_t bt_hid_host_get_cached_rssi(void);
            extern uint8_t get_battery_level(void);
            extern uint8_t get_battery_state(void);

            feature_resp_buf[1] = (uint8_t)bt_hid_host_get_cached_rssi();
            uint8_t aflags = 0x80;
            feature_resp_buf[2] = aflags;
            /* DS4Dongle 0xF9 layout: [1]=RSSI [2]=flags; the battery and
             * OTA fields are zero until audio support (P2). */
            feature_resp_buf[3]  = 0;
            feature_resp_buf[4]  = 0;
            feature_resp_buf[5]  = 0;
            feature_resp_buf[6]  = 0;
            feature_resp_buf[7]  = 0;
            feature_resp_buf[8]  = 0;
            feature_resp_buf[9]  = 0;
            feature_resp_buf[10] = 0;
            feature_resp_buf[11] = 0;
            feature_resp_buf[12] = get_battery_level();
            feature_resp_buf[13] = get_battery_state();
            *data = feature_resp_buf;
            *len  = 14;
        } else {
            *len = 0;
        }
        return;
    }

    /* Auth (0xF0-0xF3): we can't proxy PS4 license auth; stall. */
    if (report_id >= DS4_USB_AUTH_REPORT_MIN &&
        report_id <= DS4_USB_AUTH_REPORT_MAX) {
        *len = 0;
        return;
    }

    /* 0x12 / 0x81: controller MAC (+ paired host MAC for 0x12), synthesized
     * from the BT layer instead of a controller round-trip. */
    if (report_id == DS4_USB_REPORT_PAIRING_INFO) {
        feature_resp_buf[0] = report_id;
        build_pairing_info(feature_resp_buf + 1);
        *data = feature_resp_buf;
        *len  = 1 + DS4_PAIRING_INFO_SIZE;
        return;
    }
    if (report_id == DS4_USB_REPORT_CTRL_MAC) {
        uint8_t mac[6];
        feature_resp_buf[0] = report_id;
        if (!bt_hid_host_get_connected_addr(mac))
            memset(mac, 0, 6);
        for (int i = 0; i < 6; i++)
            feature_resp_buf[1 + i] = mac[5 - i];
        *data = feature_resp_buf;
        *len  = 7;
        return;
    }

    /* Calibration: host asks for USB report 0x02; the data lives in BT
     * report 0x05 (fetched at connect; cached payload + trailing CRC). */
    if (report_id == DS4_USB_REPORT_CALIBRATION) {
        uint16_t snap_len = 0;
        if (bt_hid_host_feature_snapshot(DS4_FEATURE_REPORT_CALIBRATION_BT,
                                         feature_calib_snap,
                                         (uint16_t)sizeof(feature_calib_snap),
                                         &snap_len) &&
            snap_len >= (uint16_t)sizeof(feature_calib_snap)) {
            feature_resp_buf[0] = report_id;
            ds4_calib_bt_to_usb(feature_calib_snap, feature_resp_buf + 1);
            *data = feature_resp_buf;
            *len  = 37;
            g_hid_get_report_cnt++;
            return;
        }
        if (ds4_feature_fallback(report_id, feature_resp_buf + 1, len)) {
            feature_resp_buf[0] = report_id;
            *data = feature_resp_buf;
            *len  = 37;
            g_hid_get_report_cnt++;
            return;
        }
        publish_pending_feature(DS4_FEATURE_REPORT_CALIBRATION_BT);
        *len = 0;
        return;
    }

    /* Everything else: forward the cached BT feature payload. BT feature
     * responses carry a trailing CRC-32 that USB must not see. The snapshot
     * is copied into the report buffer at its final offset, so the length we
     * report is exactly the number of bytes we own. */
    uint16_t snap_len = 0;
    if (bt_hid_host_feature_snapshot(report_id, feature_resp_buf + 1,
                                     FEATURE_DATA_MAX, &snap_len)) {
        if (snap_len < 4) {
            *len = 0;
            return;
        }
        uint16_t payload_len = snap_len - 4;
        if (payload_len > FEATURE_DATA_MAX)
            payload_len = FEATURE_DATA_MAX;
        feature_resp_buf[0] = report_id;

        /* 0xA3 firmware info: the BT payload is 3 bytes shorter than the
         * USB report. hid-playstation ("expected 49 got 45") and clone
         * checks require the full 48+id bytes; zero-pad the tail. */
        if (report_id == DS4_FEATURE_REPORT_FIRMWARE &&
            payload_len < 48) {
            memset(feature_resp_buf + 1 + payload_len, 0, 48 - payload_len);
            payload_len = 48;
        }

        *data = feature_resp_buf;
        *len  = 1 + payload_len;
        g_hid_get_report_cnt++;
    } else {
        publish_pending_feature(report_id);
        *len = 0;
        g_hid_get_report_cnt++;
    }
}

ATTR_TCM_SECTION
void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report,
                         uint32_t report_len)
{
    (void)busid;
    (void)intf;

    const uint8_t *payload = report;
    uint32_t payload_len = report_len;
    if (report_len > 0 && report[0] == report_id) {
        payload++;
        payload_len--;
    }

    /* Dongle config command (0xF6 SET) */
    if (report_type == 0x03 && report_id == 0xF6 && payload_len > 0) {
        uint8_t cmd = payload[0];
        if (cmd == 0x01 && payload_len > 1) {
            config_set(payload + 1, payload_len - 1);
            usb_gamepad_set_polling_rate(config_polling_mode());
            g_hid_set_report_cnt++;   /* EP0 path: counter only */
        } else if (cmd == 0x02) {
            usb_config_save_pending = true;
        } else if (cmd == 0x03) {
            usb_reset_pending = true;
        }
        return;
    }

    /* INTERRUPT-OUT or SET_REPORT: USB output report 0x05 (rumble/LED)
     * maps 1:1 onto BT report 0x11 bytes 3.. — forward as-is.
     * lock_volume (flag-bit strip) is applied in the BT output path. */
    if (report_type == 0x02) {
        if (report_id == DS4_USB_OUTPUT_REPORT_ID && output_callback) {
            output_callback(report, (uint16_t)report_len);
        } else if (report_id == 0 && payload_len >= 1 &&
                   payload[0] == DS4_USB_OUTPUT_REPORT_ID && output_callback) {
            /* Host sent a raw report with the ID embedded */
            output_callback(payload, (uint16_t)payload_len);
        }
        return;
    }

    /* All other Feature SET_REPORTs are dropped — DS4Dongle forwards
     * nothing (forwarding 0x08 would power-off/re-pair the controller). */
}

/* ---- USB soft plug/unplug via PHY UNPLUG bit ----
 *
 * Single-writer discipline (review finding 3): the UNPLUG bit has exactly one
 * owner, usb_task. Everything else publishes a request and lets that task
 * apply it, so main's BT connect/deferred-loss path and the speaker fault
 * recovery can no longer interleave two direct writes on the same register.
 *
 * Ordering: usb_phy_process() drains one request per call. The final
 * validation and the UNPLUG write happen inside a single IRQ-masked region
 * (finding 1): the host-reconstruction recorders (USB reset / SET_INTERFACE
 * ISR, via usb_audio_phy_confirm -> spk_phy_gates_ok) bump the speaker epoch
 * under the same mask, so no host reopen can slip between "the decision is
 * still valid" and "the register is written". A stale recovery request is
 * dropped instead of tearing down a stream the host just rebuilt. */

static volatile uint8_t usb_phy_req;        /* usb_phy_req_t, 0 = none */
static volatile uint8_t usb_phy_req_owner;  /* usb_phy_owner_t of the request */
/* Last direction this task VERIFIED in the PHY (see the readback below).
 * NONE means "no verified write has happened yet". It is NOT the source of
 * truth for the direction: every decision and every snapshot reads the
 * PHY_TST.UNPLUG bit itself (review finding 1). */
static volatile uint8_t usb_phy_applied;    /* usb_phy_req_t last verified */
/* Applied-edge identity: one bump per REAL direction change. Written only
 * inside usb_phy_process()'s masked validate-and-write region, so a consumer
 * can never see the register and the identity disagree. */
static volatile uint32_t usb_phy_edge_gen;
static volatile uint32_t usb_phy_edge_ms;
/* Review finding 1: requests whose direction is already applied. Diagnostic
 * counter only; they must not touch the register or the identity above. */
static volatile uint32_t usb_phy_noop_cnt;
/* Review finding 1: writes that did not take. The register was written but
 * the readback still reported the old direction, so the request was left
 * pending for the next pass. Diagnostic counter only. */
static volatile uint32_t usb_phy_write_lost;

/* PHY_TST.UNPLUG (USB_BASE + 0x114, bit 0): 1 = bus held disconnected,
 * 0 = connected. The register - not a software shadow - is the direction
 * truth. Callers hold the IRQ mask across the whole decision/write/readback
 * sequence, so this single read is coherent with usb_soft_connect/disconnect.
 * A volatile read forces the load; it cannot be satisfied from a cached
 * store, which is what makes the readback meaningful after a lost MMIO
 * write. */
#define USB_PHY_TST_OFF        0x114u
#define USB_PHY_TST_UNPLUG     (1u << 0)

static inline bool usb_phy_unplug_bit(void)
{
    volatile uint32_t *phy_tst =
        (volatile uint32_t *)(USB_BASE + USB_PHY_TST_OFF);

    return (*phy_tst & USB_PHY_TST_UNPLUG) != 0;
}

static inline bool usb_phy_direction_connected(void)
{
    return !usb_phy_unplug_bit();
}

void usb_phy_request(usb_phy_req_t req, usb_phy_owner_t owner)
{
    uintptr_t key = bflb_irq_save();

    usb_phy_req = (uint8_t)req;
    usb_phy_req_owner = (uint8_t)owner;
    bflb_irq_restore(key);
}

bool usb_phy_request_pending(usb_phy_owner_t owner)
{
    uintptr_t key = bflb_irq_save();
    bool pending = (usb_phy_req != (uint8_t)USB_PHY_REQ_NONE) &&
                   (usb_phy_req_owner == (uint8_t)owner);
    bflb_irq_restore(key);
    return pending;
}

/* Any unapplied request, from any owner. Read atomically (request and owner
 * are written together under the mask) so a caller cannot observe a torn
 * {req, owner} pair. Used by main's visibility progress guarantee: it must
 * not overwrite an in-flight DISCONNECT from the recovery. */
bool usb_phy_request_any_pending(void)
{
    uintptr_t key = bflb_irq_save();
    bool pending = (usb_phy_req != (uint8_t)USB_PHY_REQ_NONE);
    bflb_irq_restore(key);
    return pending;
}

bool usb_phy_request_if_gen(usb_phy_req_t req, usb_phy_owner_t owner,
                            uint32_t expect_gen)
{
    uintptr_t key = bflb_irq_save();
    bool queued = false;

    /* One masked region with usb_phy_process(): either the generation still
     * matches (publish) or an edge was applied and the stale request is
     * dropped instead of undoing it. An in-flight request is never
     * overwritten, so a retry cannot clobber main's pending edge. */
    if (usb_phy_req == (uint8_t)USB_PHY_REQ_NONE &&
        usb_phy_edge_gen == expect_gen) {
        usb_phy_req = (uint8_t)req;
        usb_phy_req_owner = (uint8_t)owner;
        queued = true;
    }
    bflb_irq_restore(key);
    return queued;
}

/* Caller holds the IRQ mask. Fills the snapshot from one instant. The
 * direction is READ FROM THE PHY (review finding 1): a shadow could claim a
 * direction a lost MMIO write never applied, and every grace/takeover
 * consumer would then reason about a bus that is not in that state. */
static void usb_phy_snapshot_masked(usb_phy_snapshot_t *out)
{
    out->gen = usb_phy_edge_gen;
    out->last_ms = usb_phy_edge_ms;
    out->connected = usb_phy_direction_connected();
}

void usb_phy_snapshot(usb_phy_snapshot_t *out)
{
    uintptr_t key;

    if (!out) {
        return;
    }
    key = bflb_irq_save();
    usb_phy_snapshot_masked(out);
    bflb_irq_restore(key);
}

bool usb_phy_main_connect_if_due(uint32_t now_ms, uint32_t grace_ms)
{
    usb_phy_snapshot_t snap;
    uintptr_t key;
    bool queued = false;

    /* Review finding 3: snapshot, predicate and publication are one
     * indivisible step. An edge applied after the snapshot cannot leave the
     * decision using an older disconnect timestamp. */
    key = bflb_irq_save();
    usb_phy_snapshot_masked(&snap);
    if (usb_phy_req == (uint8_t)USB_PHY_REQ_NONE &&
        snap.connected == false &&
        spk_visibility_grace_elapsed_ms(true, snap.last_ms, now_ms,
                                        grace_ms)) {
        usb_phy_req = (uint8_t)USB_PHY_REQ_CONNECT;
        usb_phy_req_owner = (uint8_t)USB_PHY_OWNER_MAIN;
        queued = true;
    }
    bflb_irq_restore(key);
    return queued;
}

void usb_phy_process(void)
{
    uint8_t req;
    uint8_t owner;

    uintptr_t key = bflb_irq_save();

    req = usb_phy_req;
    owner = usb_phy_req_owner;
    usb_phy_req = (uint8_t)USB_PHY_REQ_NONE;
    usb_phy_req_owner = (uint8_t)USB_PHY_OWNER_NONE;

    if (req == (uint8_t)USB_PHY_REQ_NONE) {
        bflb_irq_restore(key);
        return;
    }

    /* Review finding 1: a request that does not change the APPLIED direction
     * is a no-op. Redundant CONNECT->CONNECT / DISCONNECT->DISCONNECT
     * publications (main's periodic handoff, a recovery retry) must not write
     * the register, bump the generation or restamp the time: a bumped
     * generation would falsely satisfy a takeover check and a restamped time
     * would restart main's grace window.
     *
     * The direction is the PHY_TST.UNPLUG bit read under this mask, never a
     * software shadow: a lost MMIO write must not be able to make the next
     * same-direction retry disappear. `usb_phy_applied` only says whether
     * this task has ever verified a write (NONE = unknown, so the first
     * request is issued). When the bit already matches, the shadow is
     * reconciled to it - it records verified state, nothing else. */
    if (usb_phy_applied != (uint8_t)USB_PHY_REQ_NONE &&
        !usb_phy_identity_changes(usb_phy_direction_connected(),
                                  req == (uint8_t)USB_PHY_REQ_CONNECT)) {
        usb_phy_noop_cnt++;
        usb_phy_applied = req;      /* verified by the bit just read */
        bflb_irq_restore(key);
        return;
    }

    /* Finding 1: validate-then-write is one indivisible step.
     *
     * Why the mask is required: a recovery edge is only valid for the epoch
     * the decision was made under. The SET_INTERFACE / USB-reset recorders
     * increment spk_host_epoch from ISR context under this same mask. If the
     * mask were released between usb_audio_phy_confirm() and the UNPLUG
     * write, a host reopen arriving in that gap would already be up (its
     * epoch bumped) while this task would still write the old decision --
     * unplugging the stream the host just rebuilt, or reporting a CONNECT
     * for a decision that is stale. Holding the mask across both makes "the
     * epoch is still valid" and "the PHY write happened" a single event:
     * either the reopen is recorded before the check (we skip) or it is
     * recorded after the write (the next pass consumes it), never between.
     *
     * The window contains only flag reads, the gate evaluation and one
     * register write (plus a diagnostic counter). No delays, no logging and
     * no scheduler/task calls are made while masked. usb_audio_phy_confirm()
     * is ISR-safe and lock-free by contract (ds4_usb_audio.h); its nested
     * bflb_irq_save()/restore() cannot unmask early. */
    if (owner == (uint8_t)USB_PHY_OWNER_RECOVERY &&
        !usb_audio_phy_confirm(req == (uint8_t)USB_PHY_REQ_CONNECT)) {
        bflb_irq_restore(key);
        return;
    }

    if (req == (uint8_t)USB_PHY_REQ_CONNECT) {
        usb_soft_connect();
    } else {
        usb_soft_disconnect();
    }

    /* Review finding 1: a write is not an applied edge until the PHY says it
     * took. Read the UNPLUG bit back inside the same masked region. If it
     * still reports the old direction the store was lost: keep the shadow and
     * the generation untouched (nothing was applied), count it, and put the
     * request back so the NEXT pass retries it instead of suppressing it as a
     * same-direction no-op. The recovery's DISCONNECTED/COOLDOWN retry and
     * main's handoff therefore observe the pre-write state, not a phantom
     * edge. No wait/retry loop here: the mask is held by contract. */
    {
        const bool want_connected = (req == (uint8_t)USB_PHY_REQ_CONNECT);

        if (usb_phy_direction_connected() != want_connected) {
            usb_phy_write_lost++;
            /* Never clobber a request published in the (masked) gap with the
             * one that failed: the newer publication is the current intent. */
            if (usb_phy_req == (uint8_t)USB_PHY_REQ_NONE) {
                usb_phy_req = req;
                usb_phy_req_owner = owner;
            }
            bflb_irq_restore(key);
            return;
        }
    }

    usb_phy_applied = req;
    /* Identity of a REAL applied edge, in the same masked region as the
     * write: a grace/takeover consumer either sees this direction change
     * with its timestamp or does not see it at all. No-ops returned above. */
    usb_phy_edge_gen++;
    usb_phy_edge_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);

    bflb_irq_restore(key);
}

/* Task-context primitives. Only usb_task (the PHY owner) may call these;
 * every other context publishes a request instead. */
void usb_soft_disconnect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst |= 1u;
    g_usb_soft_disc++;   /* callable from BT callback context */
}

void usb_soft_connect(void)
{
    volatile uint32_t *phy_tst = (volatile uint32_t *)(USB_BASE + 0x114);
    *phy_tst &= ~1u;
    g_usb_soft_conn++;   /* callable from BT callback context */
}
