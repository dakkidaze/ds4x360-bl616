/*
 * XUSB (Xbox 360 wired controller) persona.
 *
 * What this file owns:
 *   - the USB device/configuration/string descriptors for VID 045E /
 *     PID 028E (xusb22.inf matches USB\VID_045E&PID_028E directly, so the
 *     MS OS 1.0 compatible-ID chain is deliberately absent);
 *   - the vendor-specific interface FF/5D/01 on interface 0, including the
 *     17-byte inline class descriptor (type 0x21) that XUSB requires;
 *   - the 0xC1/0x01/0x0100/wIndex=0/20-byte handshake;
 *   - the 20-byte interrupt IN report stream: the content is produced by
 *     src/xinput_map.c (Lane A) and published via usb_xinput_send_report();
 *   - the interrupt OUT endpoint: armed after SET_CONFIGURATION and
 *     re-armed after every completion, parsing 8-byte rumble and 3-byte
 *     LED commands and forwarding them to the registered callbacks.
 *
 * Not here: DS4 state-block parsing/mapping and the 4 ms cadence (main.c
 * calls the mapper, then this module). Audio is Phase 1.
 *
 * Concurrency: the endpoint callbacks run in USB interrupt context. They
 * only copy fixed-size buffers under bflb_irq_save() and bump counters;
 * no logging, no allocation, no scheduler calls.
 */

#include "usb_xinput.h"

#include "usbd_core.h"
#include "usb_dc.h"
#include "bflb_irq.h"
#include "debug_log.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Descriptors                                                        */
/* ------------------------------------------------------------------ */

/*
 * Device descriptor: bDeviceClass/SubClass/Protocol = 0/0/0 (a composite
 * device; xusb22 binds by VID/PID). 64-byte EP0, Full-Speed.
 * bcdDevice lives in USB_XINPUT_BCD_DEVICE and must be bumped whenever any
 * descriptor below changes (Windows usbflags cache).
 */
static const uint8_t xinput_device_desc[18] = {
    0x12,                   /* bLength */
    0x01,                   /* bDescriptorType: Device */
    0x00, 0x02,             /* bcdUSB: 2.00 */
    0x00,                   /* bDeviceClass */
    0x00,                   /* bDeviceSubClass */
    0x00,                   /* bDeviceProtocol */
    0x40,                   /* bMaxPacketSize0: 64 */
    (USB_XINPUT_VID & 0xFF), (USB_XINPUT_VID >> 8),
    (USB_XINPUT_PID & 0xFF), (USB_XINPUT_PID >> 8),
    (USB_XINPUT_BCD_DEVICE & 0xFF), (USB_XINPUT_BCD_DEVICE >> 8),
    0x01,                   /* iManufacturer */
    0x02,                   /* iProduct */
    0x00,                   /* iSerialNumber: none, like a real wired pad */
    0x01,                   /* bNumConfigurations */
};

/*
 * Configuration: Config(9) + Interface(9) + XUSB class(17) + EP IN(7) +
 * EP OUT(7) = 49 bytes.
 *
 * The 17-byte 0x21 block is the XUSB class descriptor. The host never
 * issues GET_DESCRIPTOR(0x21); it is only valid inline here. Bytes 7 and
 * 14 are the real endpoint addresses, and they must match the endpoint
 * descriptors below (this is what makes Plan A's 0x82/0x03 legal).
 */
static const uint8_t xinput_config_desc[] = {
    /* Configuration descriptor */
    0x09, 0x02,
    49, 0x00,               /* wTotalLength: 49 */
    0x01,                   /* bNumInterfaces: 1 (XUSB only in Phase 0) */
    0x01,                   /* bConfigurationValue */
    0x00,                   /* iConfiguration */
    0xA0,                   /* bmAttributes: bus powered + remote wakeup */
    250,                    /* bMaxPower: 500 mA */

    /* Interface 0: XUSB, vendor specific FF/5D/01, two interrupt EPs */
    0x09, 0x04,
    0x00,                   /* bInterfaceNumber: 0 (wIndex 0 handshake) */
    0x00,                   /* bAlternateSetting */
    0x02,                   /* bNumEndpoints */
    0xFF,                   /* bInterfaceClass: vendor specific */
    0x5D,                   /* bInterfaceSubClass: XUSB */
    0x01,                   /* bInterfaceProtocol */
    0x00,                   /* iInterface */

    /* XUSB class descriptor: 17 bytes, type 0x21, inline */
    0x11,                   /* bLength */
    0x21,                   /* bDescriptorType: XUSB */
    0x00, 0x01,             /* bcdUSB 1.00 */
    0x01,                   /* fixed */
    0x25,                   /* fixed */
    USB_XINPUT_EP_IN,       /* IN endpoint address 0x82 */
    USB_XINPUT_REPORT_LEN,  /* 20-byte IN report */
    0x00, 0x00, 0x00, 0x00, /* reserved */
    0x13,                   /* fixed */
    USB_XINPUT_EP_OUT,      /* OUT endpoint address 0x03 */
    USB_XINPUT_RUMBLE_LEN,  /* 8-byte OUT report */
    0x00, 0x00,             /* reserved */

    /* EP IN: interrupt, 32-byte MPS, 4 ms (250 Hz) */
    0x07, 0x05,
    USB_XINPUT_EP_IN,
    0x03,
    USB_XINPUT_EP_MPS, 0x00,
    USB_XINPUT_INTERVAL_IN,

    /* EP OUT: interrupt, 32-byte MPS, 8 ms */
    0x07, 0x05,
    USB_XINPUT_EP_OUT,
    0x03,
    USB_XINPUT_EP_MPS, 0x00,
    USB_XINPUT_INTERVAL_OUT,
};

_Static_assert(sizeof(xinput_config_desc) == 49,
               "XUSB config descriptor must stay 49 bytes "
               "(wTotalLength is a literal)");
_Static_assert(sizeof(xinput_device_desc) == 18,
               "device descriptor must be 18 bytes");

const uint8_t *usb_xinput_device_desc(void)
{
    return xinput_device_desc;
}

const uint8_t *usb_xinput_config_desc(void)
{
    return xinput_config_desc;
}

/* The interface block inside xinput_config_desc, for the composite
 * configuration descriptor (XUSB + UAC1 audio). 40 bytes = Interface (9) +
 * inline 0x21 (17) + EP IN (7) + EP OUT (7); config byte 9 is exactly where
 * the interface descriptor starts. No copy: the caller memcpy's from this
 * single source so the bytes cannot drift. */
const uint8_t *usb_xinput_interface_block(uint16_t *len)
{
    if (len) {
        *len = (uint16_t)(sizeof(xinput_config_desc) - 9);
    }
    return xinput_config_desc + 9;
}

/* A real wired 360 pad reports no serial number and the driver's device
 * name comes from xusb22.inf, not from iProduct. Keep the strings plain. */
const char *usb_xinput_string_desc(uint8_t index)
{
    switch (index) {
    case 0: return "\x09\x04"; /* en-US LANGID, little endian */
    case 1: return "Microsoft";
    case 2: return "Xbox 360 Controller for Windows";
    default: return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static struct usbd_interface xinput_intf;
static struct usbd_endpoint xinput_ep_in;
static struct usbd_endpoint xinput_ep_out;

static USB_NOCACHE_RAM_SECTION uint8_t xinput_in_buf[USB_XINPUT_REPORT_LEN];
static USB_NOCACHE_RAM_SECTION uint8_t xinput_out_buf[USB_XINPUT_EP_MPS];

/* Latest IN snapshot. Published complete under the IRQ mask; the endpoint
 * always transmits the newest snapshot, so the report stream never mixes
 * two states. */
static volatile uint8_t pending_report[USB_XINPUT_REPORT_LEN];
static volatile bool pending_valid;

static volatile bool xinput_configured;
static volatile bool in_busy;
static volatile bool out_armed;

/* 20-byte reply to the 0xC1/0x01 handshake. All zero is accepted. */
static uint8_t handshake_status[USB_XINPUT_REPORT_LEN];

volatile uint32_t g_xinput_in_sent;
volatile uint32_t g_xinput_in_err;
volatile uint32_t g_xinput_out_reports;
volatile uint32_t g_xinput_out_arm_err;
volatile uint32_t g_xinput_rumble_cnt;
volatile uint32_t g_xinput_led_cnt;
volatile uint32_t g_xinput_handshake_cnt;
volatile uint8_t g_xinput_last_rumble_strong;
volatile uint8_t g_xinput_last_rumble_weak;
volatile uint8_t g_xinput_last_led_cmd;

/* OUT consumers (usb_gamepad.c glue). Written once at init from task context,
 * read from the OUT completion ISR; the pointer-sized store is atomic on this
 * target and both are only set before the bus is configured. */
static usb_xinput_rumble_cb_t xinput_rumble_cb = NULL;
static usb_xinput_led_cb_t    xinput_led_cb = NULL;

/* ------------------------------------------------------------------ */
/* Endpoint plumbing                                                  */
/* ------------------------------------------------------------------ */

static void xinput_arm_out(void)
{
    uintptr_t key;
    int ret;

    if (!xinput_configured || out_armed)
        return;

    /* Guard check, submit and failure rollback are ONE interrupt-masked
     * region. SET_INTERFACE (notify handler) and the OUT completion both run
     * in interrupt context and clear/observe out_armed; without the mask they
     * could land between the test and usbd_ep_start_read(), clear the guard
     * and arm another read, and this path would then submit a second one.
     * usbd_ep_start_read() takes the same mask internally (bflb_usb_v2.c:
     * "the mask simply nests"), so this is safe and adds no re-entrancy. */
    key = bflb_irq_save();
    if (!xinput_configured || out_armed) {
        bflb_irq_restore(key);
        return;
    }
    out_armed = true;
    ret = usbd_ep_start_read(0, USB_XINPUT_EP_OUT, xinput_out_buf,
                             sizeof(xinput_out_buf));
    if (ret < 0) {
        out_armed = false;
        g_xinput_out_arm_err++;
    }
    bflb_irq_restore(key);
}

static void xinput_try_send_in(void)
{
    uintptr_t key;
    int ret;

    if (!xinput_configured || in_busy || !pending_valid)
        return;

    /* From the re-check to the submit (and its rollback) is one masked
     * region. The SET_INTERFACE notify handler clears in_busy from interrupt
     * context; if it landed after the old re-check but before the submit, it
     * would release the guard, re-arm the endpoint, and the submit below would
     * arm a second transfer on the same endpoint. usbd_ep_start_write() takes
     * the same mask internally, and the mask nests. */
    key = bflb_irq_save();
    if (!xinput_configured || in_busy || !pending_valid) {
        bflb_irq_restore(key);
        return;
    }
    memcpy(xinput_in_buf, (const void *)pending_report,
           USB_XINPUT_REPORT_LEN);
    in_busy = true;

    ret = usbd_ep_start_write(0, USB_XINPUT_EP_IN, xinput_in_buf,
                              USB_XINPUT_REPORT_LEN);
    if (ret < 0) {
        in_busy = false;
        g_xinput_in_err++;
    } else {
        g_xinput_in_sent++;
    }
    bflb_irq_restore(key);
}

/* IN completion: the host polled, so re-arm immediately with the newest
 * snapshot. This is what paces the stream to bInterval (4 ms) without a
 * timer: the endpoint is busy until the host polls it. */
static void xinput_ep_in_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;
    (void)nbytes;

    in_busy = false;
    xinput_try_send_in();
}

/* OUT completion: parses one host→device report and re-arms. */
static void xinput_ep_out_handler(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    (void)ep;

    out_armed = false;
    g_xinput_out_reports++;

    if (nbytes >= USB_XINPUT_RUMBLE_LEN &&
        xinput_out_buf[0] == 0x00 && xinput_out_buf[1] == 0x08) {
        /* 00 08 00 <strong> <weak> 00 00 00 */
        g_xinput_last_rumble_strong = xinput_out_buf[3];
        g_xinput_last_rumble_weak   = xinput_out_buf[4];
        g_xinput_rumble_cnt++;
        if (xinput_rumble_cb) {
            xinput_rumble_cb(g_xinput_last_rumble_strong,
                             g_xinput_last_rumble_weak);
        }
    } else if (nbytes >= USB_XINPUT_LED_LEN &&
               xinput_out_buf[0] == 0x01 && xinput_out_buf[1] == 0x03) {
        /* 01 03 <cmd>, 0x06-0x09 = player 1-4 */
        g_xinput_last_led_cmd = xinput_out_buf[2];
        g_xinput_led_cnt++;
        if (xinput_led_cb) {
            xinput_led_cb(g_xinput_last_led_cmd);
        }
    }

    xinput_arm_out();
}

/* ------------------------------------------------------------------ */
/* Control requests                                                   */
/* ------------------------------------------------------------------ */

/*
 * XUSB handshake: 0xC1 (vendor, IN, interface) / bRequest 0x01 /
 * wValue 0x0100 / wIndex 0 / wLength 20. XUSB is interface 0 precisely so
 * wIndex 0 is correct. Returning 20 bytes is required; failing this can
 * leave xusb22 in Code 10 even though enumeration succeeded.
 */
static int xinput_vendor_handler(uint8_t busid, struct usb_setup_packet *setup,
                                 uint8_t **data, uint32_t *len)
{
    (void)busid;

    if (setup->bmRequestType != 0xC1 ||
        setup->bRequest != 0x01 ||
        setup->wValue != 0x0100 ||
        setup->wIndex != 0x0000) {
        return -1;
    }

    memset(handshake_status, 0, sizeof(handshake_status));
    *data = handshake_status;
    *len = USB_XINPUT_REPORT_LEN;
    g_xinput_handshake_cnt++;
    return 0;
}

/* No class-specific requests are defined for XUSB. Keep the hook so an
 * unexpected request is counted (ISR-safe: counter only) instead of being
 * silently swallowed by the core. */
static int xinput_class_interface_handler(uint8_t busid,
                                          struct usb_setup_packet *setup,
                                          uint8_t **data, uint32_t *len)
{
    (void)busid;
    (void)setup;
    (void)data;
    (void)len;
    return -1;
}

/* SET_INTERFACE(0, 0): the core cancels and reopens both endpoints before it
 * notifies the class, but that does not clear the software busy guards
 * (in_busy / out_armed). Releasing them here and re-arming is what keeps the
 * IN stream and the OUT read alive across a host interface switch; without it
 * both endpoints stay "busy" forever and the report stream dies.
 * ISR context: flags and endpoint submits only, no logging. */
static void xinput_notify_handler(uint8_t busid, uint8_t event, void *arg)
{
    const struct usb_interface_descriptor *intf = arg;

    (void)busid;

    if (event != USBD_EVENT_SET_INTERFACE || intf == NULL) {
        return;
    }
    if (intf->bInterfaceNumber != 0) {
        return;
    }

    in_busy = false;
    out_armed = false;
    xinput_arm_out();
    xinput_try_send_in();
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

void usb_xinput_register(uint8_t busid)
{
    xinput_configured = false;
    in_busy = false;
    out_armed = false;
    /* No preset snapshot: the mapper publishes the first report, and a
     * pending one from a previous session must not leak across a re-init. */
    pending_valid = false;
    memset(xinput_out_buf, 0, sizeof(xinput_out_buf));

    xinput_intf.class_interface_handler = xinput_class_interface_handler;
    xinput_intf.class_endpoint_handler  = NULL;
    xinput_intf.vendor_handler          = xinput_vendor_handler;
    xinput_intf.notify_handler          = xinput_notify_handler;
    xinput_intf.hid_report_descriptor   = NULL;
    xinput_intf.hid_report_descriptor_len = 0;
    /* Registered first: usbd_add_interface() assigns intf_num in order, so
     * this becomes interface 0. */
    usbd_add_interface(busid, &xinput_intf);

    xinput_ep_in.ep_addr = USB_XINPUT_EP_IN;
    xinput_ep_in.ep_cb   = xinput_ep_in_handler;
    usbd_add_endpoint(busid, &xinput_ep_in);

    xinput_ep_out.ep_addr = USB_XINPUT_EP_OUT;
    xinput_ep_out.ep_cb   = xinput_ep_out_handler;
    usbd_add_endpoint(busid, &xinput_ep_out);

    LOG_INF("[XUSB] interface 0 FF/5D/01 registered (IN 0x%02x / OUT 0x%02x, "
            "handshake 0xC1/0x01)\n",
            USB_XINPUT_EP_IN, USB_XINPUT_EP_OUT);
}

void usb_xinput_configured(void)
{
    xinput_configured = true;
    in_busy = false;
    out_armed = false;

    xinput_arm_out();
    xinput_try_send_in();
}

void usb_xinput_reset(void)
{
    xinput_configured = false;
    in_busy = false;
    out_armed = false;
}

void usb_xinput_suspend(void)
{
    /* Keep the host's configuration; stop arming until RESUME. */
    xinput_configured = false;
    in_busy = false;
    out_armed = false;
}

void usb_xinput_tick(void)
{
    if (!xinput_configured)
        return;
    xinput_arm_out();
    xinput_try_send_in();
}

int usb_xinput_send_report(const uint8_t report[USB_XINPUT_REPORT_LEN])
{
    uintptr_t key;

    if (report == NULL)
        return -1;

    key = bflb_irq_save();
    memcpy((void *)pending_report, report, USB_XINPUT_REPORT_LEN);
    pending_valid = true;
    bflb_irq_restore(key);

    xinput_try_send_in();
    return 0;
}

void usb_xinput_set_output_cbs(usb_xinput_rumble_cb_t r,
                               usb_xinput_led_cb_t l)
{
    xinput_rumble_cb = r;
    xinput_led_cb = l;
}
