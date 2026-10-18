#include "ds4_protocol.h"
#include <string.h>

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_initialized = true;
}

uint32_t ds4_crc32(const uint8_t *data, uint16_t len, uint32_t seed)
{
    if (!crc32_initialized)
        crc32_init();

    /* DS4/DualSense scheme: init = ~seed, process data bytes only.
     * seed 0xEADA2D49 == "0xA2" first byte, 0x2060EFC3 == "0x53". */
    uint32_t crc = ~seed;
    for (uint16_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];
    }
    return ~crc;
}

void ds4_fill_output_checksum(uint8_t *report, uint16_t report_size)
{
    uint32_t crc = ds4_crc32(report, report_size - 4, DS4_CRC_SEED_OUTPUT);
    report[report_size - 4] = (crc >> 0) & 0xFF;
    report[report_size - 3] = (crc >> 8) & 0xFF;
    report[report_size - 2] = (crc >> 16) & 0xFF;
    report[report_size - 1] = (crc >> 24) & 0xFF;
}

uint16_t ds4_build_output(uint8_t *buf, const uint8_t *payload,
                          uint16_t payload_len, uint8_t hdr2)
{
    memset(buf, 0, DS4_BT_OUTPUT_REPORT_SIZE);
    buf[0] = DS4_BT_OUTPUT_REPORT_ID;
    buf[1] = 0xC0; /* HID + CRC flag */
    buf[2] = hdr2;
    if (payload_len > DS4_BT_OUTPUT_PAYLOAD_SIZE)
        payload_len = DS4_BT_OUTPUT_PAYLOAD_SIZE;
    memcpy(&buf[3], payload, payload_len);
    ds4_fill_output_checksum(buf, DS4_BT_OUTPUT_REPORT_SIZE);
    return DS4_BT_OUTPUT_REPORT_SIZE;
}

bool ds4_is_neutral_input(const uint8_t *p, uint16_t len)
{
    if (len < 9)
        return false;

    if (p[0] < DS4_STICK_IDLE_MIN || p[0] > DS4_STICK_IDLE_MAX ||
        p[1] < DS4_STICK_IDLE_MIN || p[1] > DS4_STICK_IDLE_MAX ||
        p[2] < DS4_STICK_IDLE_MIN || p[2] > DS4_STICK_IDLE_MAX ||
        p[3] < DS4_STICK_IDLE_MIN || p[3] > DS4_STICK_IDLE_MAX ||
        p[DS4_PAYLOAD_HAT] != DS4_HAT_NEUTRAL ||
        p[DS4_PAYLOAD_BTN_MISC] != 0x00 ||
        (p[DS4_PAYLOAD_BTN_PS] & 0x03) != 0x00 ||
        p[DS4_PAYLOAD_L2] != 0x00 ||
        p[DS4_PAYLOAD_R2] != 0x00) {
        return false;
    }
    return true;
}

void ds4_calib_bt_to_usb(const uint8_t *bt, uint8_t *usb)
{
    /* Bias words (bytes 0-5) and accel words (bytes 18-35) share the same
     * layout on both transports; only the gyro range words need reordering:
     * BT: pitch+ yaw+ roll+ pitch- yaw- roll-
     * USB: pitch+ pitch- yaw+ yaw- roll+ roll- */
    memcpy(usb, bt, 36);
    usb[6]  = bt[6];   usb[7]  = bt[7];   /* gyro pitch plus */
    usb[8]  = bt[12];  usb[9]  = bt[13];  /* gyro pitch minus */
    usb[10] = bt[8];   usb[11] = bt[9];   /* gyro yaw plus */
    usb[12] = bt[14];  usb[13] = bt[15];  /* gyro yaw minus */
    usb[14] = bt[10];  usb[15] = bt[11];  /* gyro roll plus */
    usb[16] = bt[16];  usb[17] = bt[17];  /* gyro roll minus */
}
