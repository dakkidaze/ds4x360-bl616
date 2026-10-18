/* Host-side check: replicate the firmware config descriptor assembly
 * (audio splice + gamepad block) and walk it. Build with -Isrc; add
 * -DDEBUG_CONSOLE to confirm the debug variant has the same 507-byte
 * report descriptor (the debug log reader adds no descriptor items). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ds4_usb_audio_desc.h"
#include "ds4_hid_desc.h"

#define USB_GAMEPAD_EP_IN 0x84
#define USB_GAMEPAD_EP_MPS 64
#define USB_GAMEPAD_INTERVAL_MS 1
#define USB_INTF_GAMEPAD  3
#define USBD_MAX_POWER 250

#define USB_CONFIG_SIZE (9 + DS4_AUDIO_DESC_SIZE + 25)
#define AUDIO_DESC_OFF 9
#define HID_INTF_OFF (AUDIO_DESC_OFF + DS4_AUDIO_DESC_SIZE)
#define HID_DESC_OFF (HID_INTF_OFF + 9)
#define HID_WDLEN_OFF (HID_DESC_OFF + 7)

static uint8_t config_desc[USB_CONFIG_SIZE] = {
    0x09, 0x02, USB_CONFIG_SIZE & 0xFF, (USB_CONFIG_SIZE >> 8) & 0xFF,
    0x04, 0x01, 0x00, 0xC0, USBD_MAX_POWER,
};

static const uint8_t gamepad_desc_block[] = {
    0x09, 0x04, USB_INTF_GAMEPAD, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x00, 0x00,
    0x07, 0x05, USB_GAMEPAD_EP_IN, 0x03, USB_GAMEPAD_EP_MPS, 0x00,
    USB_GAMEPAD_INTERVAL_MS,
};

int main(void)
{
    printf("sizeof gamepad_desc_block=%zu (expect 25)\n",
           sizeof(gamepad_desc_block));
    printf("USB_CONFIG_SIZE=%d\n", USB_CONFIG_SIZE);

    memcpy(config_desc + AUDIO_DESC_OFF, audio_desc, DS4_AUDIO_DESC_SIZE);
    memcpy(config_desc + HID_INTF_OFF, gamepad_desc_block,
           sizeof(gamepad_desc_block));
    config_desc[HID_WDLEN_OFF] = DS4_HID_REPORT_DESC_SIZE & 0xFF;
    config_desc[HID_WDLEN_OFF + 1] = (DS4_HID_REPORT_DESC_SIZE >> 8) & 0xFF;

    uint16_t off = 0;
    int nif = 0, nep = 0;
    while (off < USB_CONFIG_SIZE) {
        uint8_t len = config_desc[off];
        if (len == 0) { printf("!! bLength=0 @%u\n", off); return 1; }
        uint8_t type = config_desc[off + 1];
        if (type == 0x04) {
            nif++;
            printf("@%3u intf=%u alt=%u nep=%u cls=%02x sub=%02x\n",
                   off, config_desc[off+2], config_desc[off+3],
                   config_desc[off+4], config_desc[off+5], config_desc[off+6]);
        } else if (type == 0x05) {
            nep++;
            printf("@%3u   EP ep=0x%02x attr=0x%02x mps=%u\n", off,
                   config_desc[off+2], config_desc[off+3],
                   config_desc[off+4] | (config_desc[off+5] << 8));
        }
        off += len;
    }
    printf("end off=%u total=%u interfaces=%d endpoints=%d\n",
           off, USB_CONFIG_SIZE, nif, nep);
    printf("HID wdlen@%d = %u\n", HID_WDLEN_OFF,
           config_desc[HID_WDLEN_OFF] | (config_desc[HID_WDLEN_OFF+1] << 8));

    /* Report descriptor length: 507 bytes in EVERY build (release and
     * DEBUG_CONSOLE alike, byte-identical). The debug log reader reuses the
     * DS4's own feature report 0xB4, which this descriptor already declares,
     * so nothing is ever appended. */
    printf("report desc: %u bytes%s\n",
           (unsigned)DS4_HID_REPORT_DESC_SIZE,
#ifdef DEBUG_CONSOLE
           " (DEBUG_CONSOLE: stock DS4 v2, descriptor unchanged)"
#else
           " (release: real DS4 v2)"
#endif
           );

    int desc_ok = (DS4_HID_REPORT_DESC_SIZE == 507);
    printf("  expected 507 bytes: %s\n", desc_ok ? "yes" : "NO");
    {
        /* 0xB4 is the report the DEBUG_CONSOLE reader answers; it must be
         * declared in both variants (it is part of the stock descriptor). */
        int has_b4 = 0;
        for (unsigned i = 0; i + 1 < DS4_HID_REPORT_DESC_SIZE; i++) {
            if (hid_report_desc_ds4[i] == 0x85 &&
                hid_report_desc_ds4[i + 1] == 0xB4) {
                has_b4 = 1;
                break;
            }
        }
        printf("  0xB4 Feature report declared: %s\n", has_b4 ? "yes" : "no");
        desc_ok = desc_ok && has_b4;
    }
    return (off == USB_CONFIG_SIZE && desc_ok) ? 0 : 1;
}
