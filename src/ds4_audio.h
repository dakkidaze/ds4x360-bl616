#ifndef DS4_AUDIO_H
#define DS4_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/*
 * DS4 audio pipeline (P2):
 *
 *   Speaker (host -> controller):
 *     USB ISO OUT 32kHz stereo S16 -> 128-frame PCM blocks -> bluedroid SBC
 *     encoder (dual channel, 16 blocks, 8 subbands, loudness, bitpool 25 =
 *     112-byte frames, header 9c 75 19) -> 4 frames packed into BT output
 *     report 0x17 (462 B incl CRC-32). One report = 512 samples = 16 ms; the
 *     USB isochronous stream is the pacing clock. No resampling.
 *
 *   Headset mic (controller -> host):
 *     BT input reports 0x12..0x19 carry one SBC frame at offset 74 (after the
 *     transport byte is stripped), 16 kHz mono, 16 blocks/8 subbands,
 *     loudness, bitpool 29 -> 66-byte frames, header 9c 31 1d, 128 samples =
 *     8 ms -> OI decoder -> USB ISO IN (32 B / ms).
 *
 *   The controller sends one mic report per output report received, so while
 *   the mic is open and no speaker audio is clocking the link, a no-op 0x11
 *   output is sent every 8 ms (mic keepalive task).
 */

/* One SBC-encoded speaker frame is ready */
void ds4_audio_on_frame(const uint8_t *frame);

/* BT input report 0x12..0x19 arrived (transport byte stripped): scan for the
 * mic SBC frame and queue it for decoding. */
void ds4_audio_mic_bt_data(const uint8_t *data, uint16_t len);

/* Host opened/closed the mic IN interface (from usbd_audio_open/close). */
void ds4_audio_set_mic_active(bool active);

/* Headset plugged into the controller jack (BT input ext byte bit5). */
void ds4_audio_set_headset(bool plugged);
bool ds4_audio_headset_plugged(void);

/* Push current mic-enable (USB mic open + headset present) to the controller. */
void ds4_audio_update_mic_enable(void);

/* USB speaker stream state (for the mic keepalive gate). */
void ds4_audio_set_speaker_streaming(bool on);
bool ds4_audio_speaker_streaming(void);

/* Speaker stream just opened (host selected alt 1) — used for diagnostics. */
void ds4_audio_speaker_opened(void);

/* UAC1 volume/mute callbacks (CherryUSB, int dB in the advertised
 * -100..0 range; rescaled onto the DS4 amp's measured -60..0 dB curve). */
void ds4_audio_push_speaker_volume(int volume_db);
void ds4_audio_push_speaker_mute(bool mute);
int ds4_audio_get_speaker_volume(void);
bool ds4_audio_get_speaker_mute(void);

/* BT link went down / controller disconnected: flush queues. */
void ds4_audio_reset(void);

/* Create internal queues; call from main() before the scheduler starts. */
void ds4_audio_init(void);

/* Track BT connection state (drives enable-mic and keepalive gating). */
void ds4_audio_set_connected(bool connected);
bool ds4_audio_is_connected(void);

/* FreeRTOS tasks */
void ds4_audio_task(void *arg);       /* SBC encode + 0x17 send */
void ds4_audio_mic_task(void *arg);   /* SBC decode -> USB ISO IN */
void ds4_audio_keepalive_task(void *arg);
void ds4_audio_mic_feed_task(void *arg); /* 8 ms no-op output while mic open */

#endif /* DS4_AUDIO_H */
