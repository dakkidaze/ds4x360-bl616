#ifndef DS4_OUTPUT_H
#define DS4_OUTPUT_H

#include <stdint.h>

/* Shared BT output plumbing (implemented in main.c).
 * ds4_send_output_payload builds a 78-byte BT output report 0x11 from a
 * USB-0x05-style payload (flags byte first), fills the CRC-32 and queues it
 * via bt_hid_host_send_output(). hdr2 carries the EnableMic bits owned by
 * ds4_audio (see ds4_audio_set_mic_active). */
void ds4_send_output_payload(const uint8_t *payload, uint16_t len);

uint8_t ds4_get_output_hdr2(void);
void ds4_set_output_hdr2(uint8_t v);

#endif /* DS4_OUTPUT_H */
