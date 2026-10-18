/*
 * DS4Dongle BL618 - private port layer for the P0 audio protocol core.
 *
 * ds4_audio_proto.c must stay free of SDK dependencies, so the only two
 * hardware-specific primitives it needs live here:
 *
 *   P0_LOCK() / P0_UNLOCK(k)  - one indivisible section (ISR nesting safe)
 *   P0_NOW_US()               - monotonic microsecond clock
 *
 * In the firmware build this header is the ONLY place that pulls in bflb
 * headers, which keeps every host build of the core clean.  In the host build
 * the test provides the three symbols below; the lock is a no-op that only
 * checks for re-entrancy, and time comes from the test clock.
 *
 * Endpoint addresses also live here: the core hands them to the platform
 * callbacks, and the firmware values are the ones from the audio descriptor.
 */
#ifndef DS4_AUDIO_PROTO_PORT_H
#define DS4_AUDIO_PROTO_PORT_H

#include <stdint.h>

#ifdef P0_HOST_TEST

#define P0_EP_SPK_OUT   0x01
#define P0_EP_MIC_IN    0x82

/* Provided by tools/p0_proto_test.c. */
uint32_t p0_host_lock_enter(void);
void     p0_host_lock_leave(uint32_t depth);
uint64_t p0_host_now_us(void);
/* Host-only injection point for p0_pcm_take (spec 3, platform test_hook).
 * p0_pcm_take has no platform argument, so the test binds this symbol to its
 * own platform test_hook. */
void     p0_host_test_hook(int point);

#define P0_LOCK()        uintptr_t p0_irq_key = (uintptr_t)p0_host_lock_enter()
#define P0_UNLOCK(k)     p0_host_lock_leave((uint32_t)(k))
#define P0_NOW_US()      p0_host_now_us()
#define P0_TEST_HOOK(pt) p0_host_test_hook(pt)

#else /* firmware */

#include "bflb_irq.h"
#include "bflb_mtimer.h"
#include "ds4_usb_audio_desc.h"

#define P0_EP_SPK_OUT   DS4_AUDIO_EP_OUT
#define P0_EP_MIC_IN    DS4_AUDIO_EP_IN

#define P0_LOCK()        uintptr_t p0_irq_key = bflb_irq_save()
#define P0_UNLOCK(k)     bflb_irq_restore(k)
#define P0_NOW_US()      bflb_mtimer_get_time_us()
#define P0_TEST_HOOK(pt) do { (void)(pt); } while (0)

#endif

#endif /* DS4_AUDIO_PROTO_PORT_H */
