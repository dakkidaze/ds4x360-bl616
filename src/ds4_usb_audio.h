#ifndef DS4_USB_AUDIO_H
#define DS4_USB_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/* UAC1 integration with CherryUSB for the DS4 v2 persona.
 *
 * Speaker: ISO OUT EP 0x01, 2ch 16-bit 32kHz, wMaxPacketSize 132.
 * Mic:     ISO IN  EP 0x82, 1ch 16-bit 16kHz, wMaxPacketSize 34.
 *
 * Session/ownership state lives in the protocol core (ds4_audio_proto.c);
 * this module is the hardware adapter (UAC1 descriptors, endpoint hooks and
 * the core's platform callbacks).
 *
 * Speaker PCM is accumulated into 128-frame (512-byte) stereo blocks inside
 * the core and handed to the audio task via a binary semaphore used purely
 * as a wake-up hint; usb_audio_read() reports the authoritative block count.
 * Mic PCM is published with usb_audio_mic_publish() and clocked out at
 * exactly 32 bytes (16 samples) per ISO IN frame from the core, silence-padded
 * on underrun (matches the real DS4 v2's fixed pacing).
 */

/* One SBC speaker report carries 4 frames x 128 samples = 512 stereo
 * frames (16 ms at 32 kHz). The USB ISO accumulator must therefore fill a
 * 512-frame (2048-byte) block before the encoder packs one 0x17 report. */
#define DS4_USB_AUDIO_BLOCK_FRAMES 512   /* stereo frames per 0x17 report */
#define DS4_USB_AUDIO_BLOCK_BYTES  (DS4_USB_AUDIO_BLOCK_FRAMES * 4)

/* Called from main() before the scheduler starts */
void usb_audio_early_init(void);

/* Register UAC1 interfaces + ISO endpoints with CherryUSB (intf 0/1/2). */
void usb_audio_register(uint8_t busid);

/* Inject the 184-byte audio descriptor into the gamepad config descriptor
 * at the given offset (returns DS4_AUDIO_DESC_SIZE). */
const uint8_t *usb_audio_get_desc(uint16_t *len);

/* Speaker PCM block consumption (audio task; false = no block pending) */
bool usb_audio_read(int16_t *out /* DS4_USB_AUDIO_BLOCK_BYTES */);
void *usb_audio_get_semaphore(void);

/* Speaker streaming state (for the mic keepalive gate) */
bool usb_audio_speaker_streaming(void);

/* Mic PCM delivery (mono 16kHz, from the mic decode task).
 * `gen` is the session generation the frame was produced in; frames from a
 * session that has since been closed are dropped by the core.
 * Returns the number of samples accepted (0 = dropped). */
uint32_t usb_audio_mic_publish(const int16_t *samples, uint32_t count,
                               uint32_t gen);
bool usb_audio_mic_is_active(void);
uint32_t usb_audio_mic_fifo_bc(void);
void usb_audio_mic_feed(void);

/* Tear down stream state on USB reset */
void usb_audio_stop(void);
void usb_audio_mic_stop(void);
/* Full USB reset: clears host alt intent, link/suspend state and buffers */
void usb_audio_reset(void);
/* SUSPEND/RESUME: stop and restore endpoint I/O while keeping host alt state */
void usb_audio_suspend(void);
void usb_audio_resume(void);
/* Bluetooth link lost/restored: link layer only, host alt state is kept */
void usb_audio_link_down(void);
void usb_audio_link_up(void);
/* Session generation of the current microphone stream (decoder-side check) */
uint32_t usb_audio_mic_session_gen(void);

/* Diagnostics: mic frames published as silence because the accepted volume
 * sits at the bottom of the advertised -100..0 dB range (-97 dB and below),
 * where the Q15 gain rounds to zero. */
extern volatile uint32_t g_audio_mic_gain_zero;

/* Deferred ISO arm/close processing — call from a task loop (keepalive). */
void usb_audio_process(void);

/* Single-PHY-owner handshake: usb_task calls this immediately before it
 * applies a speaker-recovery connect/disconnect request. Returns false when
 * the request is stale (BT/suspend changed, the host already rebuilt the
 * stream, or main owns the pending edge) and must be dropped.
 *
 * Called from usb_phy_process() with interrupts masked, so the epoch check
 * and the PHY write are indivisible. It must therefore stay lock-free and
 * must not block, log or call the scheduler. */
bool usb_audio_phy_confirm(bool connect);

/* ---- Driver endpoint cancellation (bflb_usb_v2.c) ----
 * usbd_ep_cancel() aborts the VDMA transfer of an endpoint and latches a
 * pending cancel; usbd_ep_cancel_settled() reports that no completion of the
 * aborted transfer can arrive any more. The protocol core depends on both:
 * a cancelled mic transfer must be witnessed before the next submit. */
int  usbd_ep_cancel(uint8_t busid, const uint8_t ep);
bool usbd_ep_cancel_settled(uint8_t busid, const uint8_t ep);

/* SDK F0 VDMA safety net guard: 0 disables the net entirely, otherwise it is
 * the EP number (no direction bit) the net may recover. Set by
 * usb_audio_register() to the speaker ISO OUT endpoint. */
extern volatile uint8_t g_usb_f0_guard_ep;

/* Active = streaming now, or ISO activity within the last 5 s */
bool usb_audio_is_active(void);

#endif /* DS4_USB_AUDIO_H */
