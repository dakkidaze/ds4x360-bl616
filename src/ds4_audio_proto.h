/*
 * DS4Dongle BL618 - P0 audio session protocol core (contract).
 *
 * This header is the frozen interface from p0-protocol-spec-v2 section 3.
 * The implementation (ds4_audio_proto.c) is hardware independent and owns
 * every generation counter, target state, queue-free level register and
 * in-flight transaction.  ds4_usb_audio.c is only an adapter around it.
 *
 * Locking: every public function is atomic and takes the lock internally.
 * Callers must never hold the lock, and platform callbacks run inside that
 * critical section, so they must not re-enter the core or block.
 */
#ifndef DS4_AUDIO_PROTO_H
#define DS4_AUDIO_PROTO_H
#include <stdint.h>
#include <stdbool.h>

#define P0_MIC_PKT_SAMPLES   16      /* 1 ms @16 kHz mono */
#define P0_MIC_RING_SAMPLES  1024
#define P0_MIC_TX_BUFS       2       /* alternating DMA send buffers */
#define P0_PCM_SLOTS         3
#define P0_PCM_FRAMES        512     /* stereo frames per 0x17 report */
#define P0_MIC_BUSY_TIMEOUT_US   5000ULL
#define P0_CANCEL_TIMEOUT_US    20000ULL
#define P0_SETTLE_MAX_PASSES     4

typedef enum { P0_INTF_SPK = 0, P0_INTF_MIC = 1, P0_INTF_COUNT = 2 } p0_intf_t;

/* Why an endpoint still has to be rebuilt (i->ep_dirty_reason).
 *
 * The distinction is load-bearing for the fail-closed invariant: only an
 * *authorized reconstruction* (USB reset, link down, suspend, host close) may
 * clear a settle_failed latch, because the barrier it installs (cancel +
 * close + fresh witness) is a new, host-owned reconstruction. The fault
 * recovery path (p0_spk_fault_clear) marks the endpoint dirty to force that
 * same barrier, but it is not authorized to erase the evidence that the
 * previous witness never completed: if settle_failed is set the whole episode
 * stays latched and service must not arm. */
typedef enum {
    P0_EP_DIRTY_NONE = 0,
    P0_EP_DIRTY_INVALIDATE = 1,    /* reset/link/suspend/host close: authorized */
    P0_EP_DIRTY_FAULT_RECOVERY = 2,/* p0_spk_fault_clear(): never clears latch */
} p0_ep_dirty_reason_t;

typedef enum {
    P0_MIC_IDLE = 0,     /* can submit */
    P0_MIC_INFLIGHT,     /* submitted, waiting for completion */
    P0_MIC_CANCELING,    /* timeout/close: must cancel and witness, no submit */
    P0_MIC_FAULT         /* cancel failed: fault, wait for session rebuild */
} p0_mic_state_t;

typedef struct {
    uint8_t  desired;        /* 1 = host asked for alt>0 */
    uint8_t  applied;
    uint8_t  settle;         /* waiting for EP cancel witness */
    uint8_t  settle_passes;  /* service rounds spent in settle */
    uint8_t  settle_failed;  /* witness deadline expired: fail closed */
    uint8_t  ep_dirty;       /* EP may still be open, force close first (RESET) */
    uint8_t  ep_dirty_reason;/* p0_ep_dirty_reason_t: authorized vs recovery */
    uint32_t desired_seq;    /* +1 per host request (level + sequence) */
    uint32_t applied_seq;
    uint32_t desired_epoch;  /* bus_epoch at the time desired was written */
} p0_intf_state_t;

typedef struct {
    /* ---- intent (EP0/ISR writes, consumers only read) ---- */
    p0_intf_state_t intf[P0_INTF_COUNT];
    uint8_t  link_desired,    link_applied;
    uint8_t  suspend_desired, suspend_applied;
    uint32_t global_seq,      global_applied_seq;
    uint32_t bus_epoch;
    uint8_t  spk_fault_latch; /* F0 terminal fault: wait for a host reopen */

    /* ---- session ---- */
    uint32_t session_gen;

    /* ---- mic in-flight transaction ---- */
    p0_mic_state_t mic_state;
    uint32_t mic_submit_gen;
    uint64_t mic_submit_us;
    uint64_t mic_cancel_us;
    uint8_t  mic_cancel_issued;      /* abort already handed to the driver */
    uint8_t  mic_tx_idx;
    uint8_t  mic_streaming;
    uint16_t mic_ring[P0_MIC_RING_SAMPLES];
    uint32_t mic_wr, mic_rd;
    uint8_t  mic_frozen;             /* reset window: producers must not write */
    uint8_t  mic_ring_reset_pending; /* consumer still has to reset the ring */
    int16_t  mic_tx[P0_MIC_TX_BUFS][P0_MIC_PKT_SAMPLES];

    /* ---- speaker PCM ring ---- */
    int16_t  pcm[P0_PCM_SLOTS][P0_PCM_FRAMES * 2];
    uint32_t pcm_wr, pcm_rd, pcm_pending, pcm_write_pos;
    uint8_t  pcm_reset_pending;
    uint8_t  pcm_reserved;
    uint32_t pcm_epoch;              /* +1 per reset; unchanged when take returns true */

    /* ---- diagnostics (monotonic) ---- */
    struct {
        uint32_t mic_submit_ok, mic_submit_fail;
        uint32_t mic_stale_cplt, mic_unexpected_cplt;
        uint32_t mic_cancel_ok,   mic_cancel_fail;
        uint32_t mic_publish_gen_drop, mic_publish_frozen_drop, mic_ring_full_drop;
        uint32_t pcm_drop, pcm_reset_cnt, pcm_late_take_reject;
        uint32_t spk_arm_err, mic_arm_err;
        uint32_t settle_timeout, epoch_drop, spk_inactive_drop;
        uint32_t spk_fault_clear_refused;  /* recovery refused: fail-closed */
    } st;
} p0_state_t;

typedef struct {
    int      (*ep_cancel)(uint8_t ep_addr);          /* 0 on success */
    bool     (*ep_cancel_settled)(uint8_t ep_addr);
    int      (*ep_close)(uint8_t ep_addr);           /* 0 on success */
    int      (*ep_arm_spk)(void);                    /* usbd_ep_start_read */
    int      (*submit_mic)(const int16_t *buf, uint32_t samples);
    void     (*sem_wake)(void);                      /* ISR safe */
    void     (*notify_mic)(bool on);                 /* task context */
    void     (*notify_mic_fault)(void);
    void     (*notify_spk_fault)(void);
    /* Host test only: inject an ISR-side event at p0_pcm_take's reserve point /
     * commit point.  point 1 = slot reserved, before memcpy; point 2 = after
     * memcpy, before commit.  Always NULL in firmware. */
    void     (*test_hook)(int point);
} p0_platform_t;

void     p0_init(p0_state_t *s);
uint32_t p0_session_gen(const p0_state_t *s);

/* EP0 / USB ISR context: intent only, atomic, no endpoint I/O */
void p0_host_alt(p0_state_t *s, p0_intf_t intf, bool open);
void p0_bus_link(p0_state_t *s, bool up);
void p0_bus_suspend(p0_state_t *s, bool suspended);
void p0_bus_reset(p0_state_t *s);

/* ISO OUT completion (ISR): copy into the PCM ring; true = re-arm the OUT EP now */
bool p0_pcm_write(p0_state_t *s, const int16_t *src, uint32_t samples,
                  const p0_platform_t *p);

/* ISO IN completion (ISR) */
void p0_mic_complete(p0_state_t *s);

/* Driver reports a speaker OUT endpoint fault (ISR): applied=0, wait for the
 * host to reopen. */
void p0_spk_fault(p0_state_t *s);

/* Task context: controlled recovery from a terminal F0 fault. Clears the
 * latch and forces a fresh cancel/close + witness before anything may be
 * armed again; the caller performs the USB reconnect. Fail-closed state
 * (settle_failed) is NOT bypassed: while it is set this refuses (returns
 * false) and leaves the fault latch in place, so the caller must stay
 * latched instead of reconnecting. Returns true when the reconstruction was
 * authorized. */
bool p0_spk_fault_clear(p0_state_t *s);

/* Task context: is a controlled reconstruction still allowed? False while a
 * witness deadline has expired (settle_failed), i.e. while the speaker
 * endpoint has not been proven silent. Recovery must not bounce the bus in
 * that state. */
bool p0_spk_rebuild_allowed(const p0_state_t *s);

/* Task context: apply every deferred action (endpoint close/arm/notify) */
void p0_service(p0_state_t *s, const p0_platform_t *p);

/* Task context: 1 ms tick, gate + sample + submit in one atomic step */
void p0_mic_feed(p0_state_t *s, const p0_platform_t *p);

/* Decode task: publish PCM into the mic ring; gen is the session the frame
 * was produced in. */
uint32_t p0_mic_publish(p0_state_t *s, const int16_t *src, uint32_t samples,
                        uint32_t gen);

/* Audio task: take one 512-frame block; false = no data (or invalidated by a
 * reset). */
bool p0_pcm_take(p0_state_t *s, int16_t *dst);

#endif
