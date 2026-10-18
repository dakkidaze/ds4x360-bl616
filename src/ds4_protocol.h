#ifndef DS4_PROTOCOL_H
#define DS4_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- BT HID transport bytes ---- */
#define DS4_TRANS_DATA_OUTPUT   0xA2  /* host -> controller, interrupt channel */
#define DS4_TRANS_DATA_INPUT    0xA1  /* controller -> host, interrupt channel */
#define DS4_TRANS_DATA_FEATURE  0xA3  /* controller -> host, control channel */
#define DS4_TRANS_GET_FEATURE   0x43  /* host GET_REPORT(feature) */
#define DS4_TRANS_SET_FEATURE   0x53  /* host SET_REPORT(feature) */

/* ---- Report IDs ---- */
#define DS4_BT_INPUT_REPORT_ID     0x11  /* full input report */
#define DS4_BT_INPUT_REPORT_ID_MAX 0x19  /* 0x12..0x19 = state + mic SBC audio */
#define DS4_BT_OUTPUT_REPORT_ID    0x11
#define DS4_BT_AUDIO_REPORT_ID     0x17  /* 4 x SBC frames (audio, P2) */

/* ---- Report sizes ---- */
#define DS4_BT_INPUT_PAYLOAD_SIZE   63   /* state block, identical to USB report 0x01 */
#define DS4_BT_INPUT_HEADER_SIZE    3    /* report id, 0xC0, 0x00 (after 0xA1 transport) */
#define DS4_BT_OUTPUT_PAYLOAD_SIZE  71   /* bytes 3..73 of the BT output report */
#define DS4_BT_OUTPUT_DATA_SIZE     74   /* report id .. last payload byte (CRC covers this) */
#define DS4_BT_OUTPUT_REPORT_SIZE   78   /* 74 data + 4 CRC32 */
#define DS4_BT_AUDIO_REPORT_SIZE    462  /* audio: 6 hdr + 4*112 SBC + 4 pad, incl CRC (P2) */

/* ---- USB report IDs (DS4 v2 wired persona, 054C:09CC) ---- */
#define DS4_USB_INPUT_REPORT_ID     0x01  /* 63-byte state block */
#define DS4_USB_OUTPUT_REPORT_ID    0x05  /* rumble/LED/volume output */

/* ---- Feature report IDs ---- */
#define DS4_FEATURE_REPORT_CALIBRATION_BT  0x05  /* BT: 36B calibration + CRC */
#define DS4_FEATURE_REPORT_FIRMWARE        0xA3  /* BT 45B payload, zero-padded to 48 on USB */
#define DS4_FEATURE_REPORT_PERUNIT_CAL     0x06  /* per-unit calibration / serial blob */
#define DS4_USB_REPORT_CALIBRATION         0x02  /* USB: BT 0x05 with gyro words reordered */
#define DS4_USB_REPORT_PAIRING_INFO        0x12  /* synthesized: ctrl MAC + host MAC */
#define DS4_USB_REPORT_CTRL_MAC            0x81  /* synthesized: controller MAC */
#define DS4_USB_AUTH_REPORT_MIN            0xF0  /* PS4 license auth: cannot proxy, stall */
#define DS4_USB_AUTH_REPORT_MAX            0xF3

/* ---- CRC seeds (equivalent first-byte inits, DS4Dongle formulation) ---- */
#define DS4_CRC_SEED_OUTPUT   0xEADA2D49u  /* "0xA2 seed" */
#define DS4_CRC_SEED_FEATURE  0x2060EFC3u  /* "0x53 seed" */

/* ---- BT output report header byte 2 ----
 * bit7 EnableAudio, bit5 always set, bits0-2 EnableMic */
#define DS4_OUT_HDR2_DEFAULT     0x20
#define DS4_OUT_HDR2_MIC_ENABLE  0x24

/* ---- Input payload offsets (63-byte state block) ---- */
#define DS4_PAYLOAD_STICK_LX     0
#define DS4_PAYLOAD_STICK_LY     1
#define DS4_PAYLOAD_STICK_RX     2
#define DS4_PAYLOAD_STICK_RY     3
#define DS4_PAYLOAD_HAT          4    /* 0x08 = neutral */
#define DS4_PAYLOAD_BTN_MISC     5    /* share/options/L3/R3 */
#define DS4_PAYLOAD_BTN_PS       6    /* bit0 = PS, bit1 = touchpad click */
#define DS4_PAYLOAD_L2           7
#define DS4_PAYLOAD_R2           8
#define DS4_PAYLOAD_BATT_EXT     29   /* bit0-3 pct, bit4 charging, bit5 headset */
#define DS4_PAYLOAD_TOUCH_COUNT  32   /* num_touch_reports */

#define DS4_HAT_NEUTRAL          0x08
#define DS4_BATT_LEVEL_MASK      0x0F
#define DS4_BATT_CHARGING_MASK   0x10
#define DS4_BATT_HEADSET_MASK    0x20

/* ---- Audio targets (0x17 report byte 5, P2) ---- */
#define DS4_AUDIO_TARGET_SPEAKER 0x02
#define DS4_AUDIO_TARGET_HEADSET 0x24

/* ---- Neutral-stick window for the inactivity detector ---- */
#define DS4_STICK_IDLE_MIN 120
#define DS4_STICK_IDLE_MAX 140

/* USB report 0x12 layout */
#define DS4_PAIRING_INFO_SIZE 15

/* Compute the DS4 CRC-32 variant (reflected, poly 0xEDB88320, init ~seed). */
uint32_t ds4_crc32(const uint8_t *data, uint16_t len, uint32_t seed);

/* Fill the trailing CRC-32 of a BT output report (0x11/0x17).
 * CRC covers the first (report_size - 4) bytes, written little-endian. */
void ds4_fill_output_checksum(uint8_t *report, uint16_t report_size);

/* Build a 78-byte BT output report 0x11 from a USB-0x05-style payload.
 * Layout: [0]=0x11 [1]=0xC0 [2]=hdr2 [3..73]=payload (zero-padded)
 *         [74..77]=CRC32. Returns DS4_BT_OUTPUT_REPORT_SIZE. */
uint16_t ds4_build_output(uint8_t *buf, const uint8_t *payload,
                          uint16_t payload_len, uint8_t hdr2);

/* True when the 63-byte state block is fully neutral (idle detection).
 * Sticks within [120,140], hat 0x08, no face/misc/PS/TP buttons, L2/R2 0. */
bool ds4_is_neutral_input(const uint8_t *p, uint16_t len);

/* Translate the BT calibration feature report 0x05 (36-byte payload) into
 * the USB report 0x02 layout. Gyro plus/minus words are grouped
 * all-plus-then-all-minus over BT but interleaved per axis over USB. */
void ds4_calib_bt_to_usb(const uint8_t *bt, uint8_t *usb);

#endif /* DS4_PROTOCOL_H */
