/*
 * DS4Dongle BL618 — DS4 audio pipeline (P2).
 *
 * Speaker: USB ISO OUT 32kHz stereo S16 blocks (ds4_usb_audio) -> bluedroid
 * SBC encoder (dual channel, 16 blocks, 8 subbands, loudness, bitpool 25 =
 * 112-byte frames, header 9c 75 19) -> 4 frames per BT output report 0x17
 * (462 bytes incl CRC-32). One report = 512 samples = 16 ms; the USB
 * isochronous stream is the pacing clock, no resampling.
 *
 * Headset mic: BT input reports 0x12..0x19 carry one SBC frame (16 kHz mono,
 * 16 blocks / 8 subbands, loudness, bitpool 29 -> 66-byte frames, header
 * 9c 31 1d, 128 samples = 8 ms) located by its full header. Decoded with the
 * OI codec (pcmStride=1, no deinterleave) and handed to the USB mic EP.
 *
 * The controller sends one mic report per output report received, so while
 * the mic is open and no speaker audio is clocking the link, a no-op 0x11
 * output is sent every 8 ms (matches DS4Dongle, including its known
 * exactly-8ms TODO behavior).
 */

#include "ds4_audio.h"
#include "ds4_protocol.h"
#include "ds4_output.h"
#include "ds4_usb_audio.h"
#include "bt_hid_host.h"
#include "usb_gamepad.h"
#include "config.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "task.h"
#include "bflb_mtimer.h"
#include "debug_log.h"
#include <string.h>

#include "sbc_encoder.h"          /* bluedroid encoder (libblestack.a) */
#include "oi_codec_sbc.h"         /* OI decoder (libblestack.a) */

#define SBC_ENC_FRAME_SAMPLES 128   /* 16 blocks * 8 subbands, per channel */
#define SBC_ENC_FRAME_LEN     112   /* dual channel, bitpool 25 */
#define FRAMES_PER_REPORT     4
/* Audio header byte (0x17 report offset 5): target of the SBC payload. */

#define MIC_SBC_FRAME_LEN     66
#define MIC_SBC_SCAN_FROM     74    /* audio header start (0xA1 stripped) */
#define MIC_PCM_SAMPLES       128   /* mono 16 kHz samples per frame */

/* ---- Speaker encoder state ---- */
static SBC_ENC_PARAMS sbc_enc;
static bool sbc_enc_ready = false;

/* ---- Mic decoder state ---- */
static OI_CODEC_SBC_DECODER_CONTEXT mic_dec;
/* OI decoder scratch: subdata 1ch*4B*8bands*16blocks=512B + filter buffers
 * 16*2B*8bands*1ch=256B (alloc.c, SBC_CODEC_MIN_FILTER_BUFFERS=16) = 768B. */
#define MIC_DEC_DATA_U32  256
static uint32_t mic_dec_data[MIC_DEC_DATA_U32];
static bool mic_dec_ready = false;
static volatile bool mic_reset_pending = false;
static volatile bool mic_queue_reset_pending = false;   /* R05: decoder-owned drain */
static uint32_t mic_latched_gen = 0;                    /* R05: session of the frames we decode */

/* ---- Mic SBC frame queue (BT callback -> mic task) ----
 * Every entry carries the session generation it was produced in, so the
 * decoder can drop frames that belong to a closed/reopened stream. */
typedef struct {
    uint32_t gen;
    uint8_t  data[MIC_SBC_FRAME_LEN];
} mic_frame_t;

#define MIC_SBC_Q_DEPTH 8
static QueueHandle_t mic_sbc_q;
static StaticQueue_t mic_sbc_q_buf;
/* Sized in words so the static queue storage is naturally aligned for the
 * uint32_t generation field. */
static uint32_t mic_sbc_q_storage[(MIC_SBC_Q_DEPTH * sizeof(mic_frame_t) + 3u) / 4u];

static volatile bool mic_active = false;   /* host opened the mic IN itf */
static volatile bool audio_connected = false;
static volatile int  sbc_dbg_left = 0;     /* kept for source compatibility: unused */

/* R01/R11: hot-path counters. These loops run per audio frame; they must not
 * format or print. Read them from a diagnostic task instead. */
volatile uint32_t g_sbc_tx_ok = 0;
volatile uint32_t g_sbc_tx_fail = 0;
/* Stereo frames that actually differed and were downmixed (L != R). A host
 * that mirrors mono reports 0; a hard-panned host reports every frame. */
volatile uint32_t g_pcm_downmix_frames = 0;
/* Blocks folded to mono because they were routed to the built-in speaker
 * (one increment per 512-frame block, never per frame). */
volatile uint32_t g_pcm_downmix_blocks = 0;
volatile uint32_t g_mic_frames = 0;
volatile uint32_t g_mic_dec_ok = 0;
volatile uint32_t g_mic_dec_fail = 0;
volatile uint32_t g_mic_dec_reset_err = 0;
/* P0-5: publish returned 0 (session already reset or ring full). The publish
 * call itself is atomic; this counter only makes the drop observable. */
volatile uint32_t g_mic_publish_drop = 0;

/* Diagnostic hook from the BT stack (hci_core.c): controller ACL MTU. */
static volatile unsigned g_br_mtu = 0, g_br_pkts = 0;
static void bt_mtu_log(unsigned br_mtu, unsigned br_pkts)
{
    g_br_mtu = br_mtu;
    g_br_pkts = br_pkts;
}
void (*g_bt_mtu_hook)(unsigned br_mtu, unsigned br_pkts) = bt_mtu_log;

void ds4_audio_speaker_opened(void)
{
    /* R01: called from the audio request consumer (keepalive task) — the
     * stream-open event is emitted by usbd_audio_open() itself. */
}

/* Pending controller-output requests from EP0/USB-ISR context. The CherryUSB
 * audio class callbacks (volume/mute/mic-enable) run inside the USB EP0
 * processing path — doing BT l2cap sends there deadlocks the USB ISR against
 * bt_task (EP0 wedges -> Code 10 on every child, BT dies -> lightbar off).
 * These callbacks only record state; the keepalive task (task context)
 * performs the actual BT output within a few ms. */
static volatile bool pending_volume   = false;
static volatile bool pending_mute     = false;
static volatile bool pending_mic_en   = false;

/* ---- Volume / mute (host-side, advertised dB range) ----
 * CherryUSB advertises -100..0 dB / 1 dB (fixed in usbd_audio.c). The DS4
 * amp is linear in dB at 1.0 dB per byte from byte 20 to byte 80 and
 * saturates hard at 80 (bytes 80..255 identical), so byte = 80 + dB over
 * the usable -60..0 dB range. Host dB is linearly rescaled from the
 * advertised span onto the usable span; GET_CUR echoes the raw host-set
 * value so the Linux kernel's sticky-volume probe passes. */
#define SPK_ADV_MIN_DB    (-100.0f)
#define SPK_ADV_MAX_DB    (0.0f)
#define SPK_USABLE_MIN_DB (-60.0f)
#define SPK_USABLE_MAX_DB (0.0f)
#define DS4_VOLUME_SAT    80.0f

/* N04/R15: host-accepted speaker state. `spk_volume_db` is the authoritative
 * value returned by GET_CUR; it is written synchronously by the SET path and
 * the controller amp is updated asynchronously from the keepalive task.
 * Mute is also synchronous so GET agrees immediately. */
static volatile int  spk_volume_db = 0;    /* 0 dB = level 80 = amp max */
static volatile bool spk_muted = false;

/* ---- Headset presence (BT input ext byte bit5) ---- */
static volatile bool plug_headset = false;

/* ---- Internal helpers ---- */
void ds4_audio_update_mic_enable(void);

/* Push the current USB mute/volume state to the controller as a DS4 0x11
 * volume report (headset L/R + speaker all set to the same byte). */
static void ds4_push_volume(void)
{
    if (config_lock_volume())
        return;

    uint8_t level = 0;
    if (!spk_muted) {
        float adv = (float)spk_volume_db;
        if (adv < SPK_ADV_MIN_DB) adv = SPK_ADV_MIN_DB;
        if (adv > SPK_ADV_MAX_DB) adv = SPK_ADV_MAX_DB;
        float frac = (adv - SPK_ADV_MIN_DB) / (SPK_ADV_MAX_DB - SPK_ADV_MIN_DB);
        float usable = SPK_USABLE_MIN_DB + frac * (SPK_USABLE_MAX_DB - SPK_USABLE_MIN_DB);
        float v = DS4_VOLUME_SAT + usable;
        if (v < 0.0f) v = 0.0f;
        if (v > DS4_VOLUME_SAT) v = DS4_VOLUME_SAT;
        level = (uint8_t)(v + 0.5f);
    }

    uint8_t payload[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0};
    payload[0] = 0xB0;  /* update headset L/R (0x10/0x20) + speaker (0x80) */
    payload[18] = level;
    payload[19] = level;
    payload[21] = level;
    ds4_send_output_payload(payload, sizeof(payload));
}

/* Start/stop headset-mic streaming: the EnableMic header bits make the
 * controller switch its input reports from 0x11 to 0x12..0x19. The header
 * only takes effect with the next output report, so send the enable payload
 * (which also carries the mic volume) immediately. */
static void ds4_enable_mic(bool on)
{
    ds4_set_output_hdr2(on ? DS4_OUT_HDR2_MIC_ENABLE : DS4_OUT_HDR2_DEFAULT);
    LOG_INF("[AUDIO] enable_mic=%d hdr2=0x%02x\n", (int)on,
            ds4_get_output_hdr2());

    uint8_t payload[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0};
    payload[0] = 0x40;                /* EnableVolumeMicUpdate */
    payload[20] = on ? 0x40 : 0x00;   /* VolumeMic: 0x40 = max gain (real DS4
                                       * runs its mic hot; lower reads quiet) */
    ds4_send_output_payload(payload, sizeof(payload));
}

/* ---- Public API ---- */

void ds4_audio_on_frame(const uint8_t *frame)
{
    (void)frame; /* unused: blocks are pulled directly in the task */
}

void ds4_audio_mic_bt_data(const uint8_t *data, uint16_t len)
{
#ifdef DS4_MIC_DIAG
    /* R17: diagnostic builds may keep decoding BT mic frames even when the
     * host has not opened the capture stream, so the path can be observed.
     * This is a separate switch on purpose: a DEBUG log build must not
     * silently change audio behaviour. */
#else
    if (!mic_active)
        return;
#endif

    /* NOTE: this runs in the Bluetooth receive callback. Do NOT printf here:
     * the SDK console path is blocking and stack-hungry, and doing so wedged
     * the link when the controller first started sending 0x16 mic reports. */

    for (uint16_t i = MIC_SBC_SCAN_FROM; i + MIC_SBC_FRAME_LEN <= len - 4; i++) {
        /* Match the full frame header (16 kHz mono 16/8 loudness, bitpool
         * 29), not just the syncword: a lone 0x9C also occurs inside SBC
         * payloads and a misaligned frame wedges the decoder. */
        if (data[i] != 0x9C || data[i + 1] != 0x31 || data[i + 2] != 0x1D)
            continue;

        /* Drop oldest on overflow (frame-rate pacing comes from our own
         * output reports, so steady-state never overflows). */
        if (uxQueueSpacesAvailable(mic_sbc_q) == 0) {
            mic_frame_t dummy;
            xQueueReceive(mic_sbc_q, &dummy, 0);
        }
        /* R05: stamp the session *before* the copy. If a close/reopen lands
         * while the frame is being staged, it is then correctly rejected as
         * belonging to the previous session. */
        mic_frame_t f;
        f.gen = usb_audio_mic_session_gen();
        memcpy(f.data, data + i, MIC_SBC_FRAME_LEN);
        xQueueSend(mic_sbc_q, &f, 0);
        i += MIC_SBC_FRAME_LEN - 1;
    }
}

void ds4_audio_set_mic_active(bool active)
{
    if (active && !mic_active) {
        /* R05: the EP0/USB context must not touch the decoder queue. The mic
         * decode task is the only consumer; ask it to drain stale frames at
         * its next iteration (it also resets the SBC decoder). */
        mic_queue_reset_pending = true;
    }
    mic_active = active;
    pending_mic_en = true;   /* applied by the keepalive task */
}

/* Headset presence steers both the 0x17 target byte and the mic enable:
 * a real DS4 v2 has no built-in mic — without a headset plugged, EnableMic
 * stays clear (controller keeps 0x11 input reports, no keepalive churn). */
void ds4_audio_set_headset(bool plugged)
{
    if (plug_headset != plugged) {
        plug_headset = plugged;
        pending_mic_en = true;   /* applied by the keepalive task */
        pending_volume = true;   /* re-send the current host volume on any jack
                                  * change: doing so restores headphone output
                                  * after re-insertion (measured on hardware);
                                  * the exact amp/routing mechanism behind that
                                  * is not established, so do not state one here */
        /* No printf here: this is called from the Bluetooth receive callback.
         * The 0x17 target and the mic-enable report are both applied later in
         * the keepalive task, so the state change needs no logging here. */
    }
}

bool ds4_audio_headset_plugged(void)
{
    return plug_headset;
}

/* Push the current mic-enable state to the controller (task context only). */
void ds4_audio_update_mic_enable(void)
{
    LOG_INF("[AUDIO] mic_enable cond: active=%d headset=%d conn=%d\n",
            (int)mic_active, (int)plug_headset, (int)audio_connected);
#ifdef DS4_MIC_DIAG
    /* R17: diagnostic-only behaviour, gated by its own build switch rather
     * than by DEBUG_CONSOLE. Enables the controller mic whenever a headset
     * is present so the BT mic + decode path can be observed without a
     * working USB capture stream. */
    ds4_enable_mic(plug_headset && audio_connected);
#else
    ds4_enable_mic(mic_active && plug_headset && audio_connected);
#endif
}

void ds4_audio_set_speaker_streaming(bool on)
{
    (void)on; /* state lives in ds4_usb_audio (usb_audio_speaker_streaming) */
}

bool ds4_audio_speaker_streaming(void)
{
    return usb_audio_speaker_streaming();
}

void ds4_audio_push_speaker_volume(int volume_db)
{
    /* N04: publish the accepted value before flagging the async BT update, so
     * a SET_CUR/GET_CUR round-trip that does not wait for the 4 ms keepalive
     * already reads this value. The amp update still happens in the task. */
    spk_volume_db = volume_db;
    pending_volume = true;
}

void ds4_audio_push_speaker_mute(bool mute)
{
    spk_muted = mute;
    pending_mute = true;     /* applied by the keepalive task */
}

int ds4_audio_get_speaker_volume(void)
{
    return spk_volume_db;
}

bool ds4_audio_get_speaker_mute(void)
{
    return spk_muted;
}

void ds4_audio_reset(void)
{
    /* R03: a Bluetooth link loss must NOT discard the host's alt state, or a
     * reconnect has nothing to re-arm. The decode task drains its own queue
     * (R05) — this path only marks it. */
    mic_queue_reset_pending = true;
    mic_active = false;
    mic_dec_ready = false;
    plug_headset = false;
    usb_audio_link_down();
    /* Restore the mic-enable header so a reconnected controller isn't
     * switched into 0x12-0x19 input reports by a stale header. */
    ds4_set_output_hdr2(DS4_OUT_HDR2_DEFAULT);
}

/* ---- Tasks ---- */

/* SBC encode + 0x17 send. Pacing comes from the USB ISO stream: one
 * semaphore unit = one 512-sample block. */
void ds4_audio_task(void *arg)
{
    (void)arg;

    memset(&sbc_enc, 0, sizeof(sbc_enc));
    sbc_enc.s16SamplingFreq      = SBC_sf32000;
    sbc_enc.s16ChannelMode       = SBC_DUAL;
    sbc_enc.s16NumOfSubBands     = SUB_BANDS_8;
    sbc_enc.s16NumOfChannels     = 2;
    sbc_enc.s16NumOfBlocks       = SBC_BLOCK_3;
    sbc_enc.s16AllocationMethod  = SBC_LOUDNESS;
    sbc_enc.s16BitPool           = 25;
    sbc_enc.u8NumPacketToEncode  = 1;
    sbc_enc.sbc_mode             = SBC_MODE_STD;
    SBC_Encoder_Init(&sbc_enc);
    sbc_enc_ready = true;
    LOG_INF("[AUDIO] SBC encoder init (32k dual 16/8 loudness bitpool 25)\n");

    static int16_t pcm_block[DS4_USB_AUDIO_BLOCK_FRAMES * 2];
    uint16_t frame_counter = 0;

    for (;;) {
        if (xSemaphoreTake(usb_audio_get_semaphore(), pdMS_TO_TICKS(100)) != pdTRUE)
            continue;

        /* P2-1: the semaphore is a wake-up hint, not a block counter — one
         * token can stand for several completed blocks. Drain everything that
         * is ready before blocking again; a stale token finds nothing and
         * simply breaks out. */
        for (;;) {
            if (!usb_audio_read(pcm_block)) {
                break;
            }
            if (!audio_connected) {
                continue; /* dropped block, USB keeps clocking */
            }

            /* Snapshot the route once per block. The DS4 has a mono built-in
             * speaker and a stereo headphone jack, both fed by this same
             * stereo SBC stream. Taking the snapshot here (and reusing it for
             * the 0x17 target byte below) means a jack hot-plug mid-block can
             * never produce a block whose audio was processed for one route
             * and transmitted to the other. */
            bool to_speaker = !ds4_audio_headset_plugged();

            if (to_speaker) {
                /* Mono speaker needs L+R: encoding the stereo block as-is
                 * sends hard-panned host content to only one side (a
                 * right-panned clip lands entirely in the R SBC channel and
                 * the speaker gets nothing usable). Fold every frame to
                 * (L+R)/2 and duplicate it into both channels so either pan
                 * is audible; the int32 sum avoids int16 overflow.
                 * The headphone jack must keep full stereo, so this fold only
                 * runs when the target is the speaker. */
                for (int i = 0; i < DS4_USB_AUDIO_BLOCK_FRAMES; i++) {
                    int16_t l = pcm_block[2 * i];
                    int16_t r = pcm_block[2 * i + 1];
                    int32_t sum = (int32_t)l + (int32_t)r;
                    int16_t mix = (int16_t)(sum / 2);

                    if (l != r)
                        g_pcm_downmix_frames++;
                    pcm_block[2 * i]     = mix;
                    pcm_block[2 * i + 1] = mix;
                }
                g_pcm_downmix_blocks++;   /* hot path: counter only */
            }

            uint8_t pkt[DS4_BT_AUDIO_REPORT_SIZE] = {0};
            pkt[0] = DS4_BT_AUDIO_REPORT_ID;
            pkt[1] = 0x40;
            /* Audio header: carry the mic-enable bits so the mic keeps streaming
             * while speaker audio is active (the controller sends one mic report
             * per received output report). */
            pkt[2] = 0xA0 | (ds4_get_output_hdr2() & 0x07);
            pkt[3] = frame_counter & 0xFF;
            pkt[4] = (frame_counter >> 8) & 0xFF;
            /* Same per-block snapshot as the downmix above: the route that
             * shaped this block's PCM is the route the block is sent to. */
            pkt[5] = to_speaker ? DS4_AUDIO_TARGET_SPEAKER
                                : DS4_AUDIO_TARGET_HEADSET;

            for (int i = 0; i < FRAMES_PER_REPORT; i++) {
                sbc_enc.ps16PcmBuffer = pcm_block + i * SBC_ENC_FRAME_SAMPLES * 2;
                sbc_enc.pu8Packet = pkt + 6 + i * SBC_ENC_FRAME_LEN;
                SBC_Encoder(&sbc_enc);
            }
            {
                (void)sbc_dbg_left;
            }
            frame_counter += FRAMES_PER_REPORT;

            ds4_fill_output_checksum(pkt, DS4_BT_AUDIO_REPORT_SIZE);
            /* Hot path (per audio frame): counters only, never printf. */
            if (bt_hid_host_send_output(pkt, DS4_BT_AUDIO_REPORT_SIZE) < 0)
                g_sbc_tx_fail++;
            else
                g_sbc_tx_ok++;
        }
    }
}

/* Mic SBC decode -> USB mic ring. */
void ds4_audio_mic_task(void *arg)
{
    (void)arg;

    for (;;) {
        /* R05: drain frames from a previous session here — the decoder task
         * owns the consumer side, so the EP0 open path never races with it. */
        if (mic_queue_reset_pending) {
            mic_queue_reset_pending = false;
            mic_frame_t stale;
            while (xQueueReceive(mic_sbc_q, &stale, 0) == pdTRUE) {
            }
            mic_reset_pending = true;
            mic_dec_ready = false;
            mic_latched_gen = usb_audio_mic_session_gen();
        }

        mic_frame_t frame;
        if (xQueueReceive(mic_sbc_q, &frame, portMAX_DELAY) != pdTRUE)
            continue;

        /* R05: a frame queued before a close/reopen belongs to the old
         * session — drop it instead of publishing stale audio. */
        if (frame.gen != mic_latched_gen) {
            uint32_t cur = usb_audio_mic_session_gen();
            mic_latched_gen = cur;
            if (frame.gen != cur)
                continue;
        }

        g_mic_frames++;   /* hot path: counter only */

        if (mic_reset_pending) {
            mic_reset_pending = false;
            mic_dec_ready = false;
        }
        if (!mic_dec_ready) {
            OI_STATUS st = OI_CODEC_SBC_DecoderReset(&mic_dec, mic_dec_data,
                                                     sizeof(mic_dec_data),
                                                     1, 1, FALSE, FALSE);
            if (st != OI_STATUS_SUCCESS) {
                g_mic_dec_reset_err++;
                continue;
            }
            mic_dec_ready = true;
        }

        /* Decode regardless of the USB stream state (usb_audio_mic_publish
         * drops when the host mic stream is closed); this keeps the mic
         * pipeline observable in the debug build. */

        const OI_BYTE *frame_ptr = frame.data;
        uint32_t frame_bytes = MIC_SBC_FRAME_LEN;
        int16_t pcm[MIC_PCM_SAMPLES];
        uint32_t pcm_bytes = sizeof(pcm);
        OI_STATUS st = OI_CODEC_SBC_DecodeFrame(&mic_dec, &frame_ptr,
                                                &frame_bytes, pcm, &pcm_bytes);
        if (st != OI_STATUS_SUCCESS) {
            g_mic_dec_fail++;
            continue;
        }
        g_mic_dec_ok++;
        /* P0-5 (R5.1/S5.3): no second gen check here. The generation is
         * passed into the publish call and checked atomically against the
         * session inside the protocol core, so a reset landing during the
         * decode either lets the whole frame in or drops it entirely. */
        uint32_t accepted = usb_audio_mic_publish(pcm,
                                                  pcm_bytes / sizeof(int16_t),
                                                  frame.gen);
        if (accepted == 0)
            g_mic_publish_drop++;
    }
}

/* 8 ms no-op output while the mic is open and no speaker audio is clocking
 * the link (the controller emits one mic report per output report). Also
 * runs the deferred ISO endpoint arm/close (never from EP0/ISR context). */
void ds4_audio_mic_feed_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (usb_audio_mic_is_active())
            usb_audio_mic_feed();
    }
}

void ds4_audio_keepalive_task(void *arg)
{
    (void)arg;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(4));
        usb_audio_process();

#ifdef DS4_MIC_DIAG
        /* R17: diagnostic lightbar. This sends a real 0x11 output report
         * every ~300 ms, so it changes BT queue load and can affect the
         * audio cadence — it belongs to the diagnostic switch, not to
         * "logging enabled".
         *   RED    = host never opened the mic interface
         *   YELLOW = opened, but mic EP never armed
         *   ORANGE = armed, but no EP IN completions
         *   CYAN   = completing, but the mic FIFO accumulates (the DCIA
         *            never puts the packet on the bus)
         *   GREEN  = completing and the FIFO drains (host reads the data) */
        {
            extern volatile uint32_t g_mic_open_cnt, g_mic_ep_tx_cnt,
                                    g_mic_ep_done_cnt, g_mic_fifo_bc_max;
            static uint32_t led_last_ms = 0;
            uint32_t now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
            if (now_ms - led_last_ms >= 300) {
                led_last_ms = now_ms;
                uint8_t r = 0, g = 0, b = 0;
                if (g_mic_open_cnt == 0)             { r = 0x60; }
                else if (g_mic_ep_tx_cnt == 0)       { r = 0x60; g = 0x60; }
                else if (g_mic_ep_done_cnt == 0)     { r = 0x60; g = 0x30; }
                else if (g_mic_fifo_bc_max != 0)     { g = 0x60; b = 0x60; } /* CYAN */
                else                                 { g = 0x60; }
                uint8_t p[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0};
                p[0] = 0x02; p[5] = r; p[6] = g; p[7] = b;
                ds4_send_output_payload(p, sizeof(p));
            }
        }
#endif
        /* Apply deferred volume/mute/mic-enable (recorded from EP0 context).
         * The volume/mute values themselves were already published
         * synchronously by the SET path (N04); this only pushes them to the
         * controller. */
        if (pending_volume || pending_mute) {
            pending_volume = false;
            pending_mute = false;
            ds4_push_volume();
        }
        if (pending_mic_en) {
            pending_mic_en = false;
            ds4_audio_update_mic_enable();
        }

        if (!audio_connected || !mic_active || !plug_headset ||
            usb_audio_speaker_streaming())
            continue;

        static uint64_t last_keepalive_us = 0;
        uint64_t now_us = bflb_mtimer_get_time_us();
        if (now_us - last_keepalive_us >= 8000) {
            last_keepalive_us = now_us;
            const uint8_t payload[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0}; /* flags 0x00: change nothing */
            ds4_send_output_payload(payload, sizeof(payload));
        }
    }
}

/* ---- Init (called from main before the scheduler starts) ---- */
void ds4_audio_init(void)
{
    mic_sbc_q = xQueueCreateStatic(MIC_SBC_Q_DEPTH, sizeof(mic_frame_t),
                                   (uint8_t *)mic_sbc_q_storage, &mic_sbc_q_buf);
    usb_audio_early_init();
}

void ds4_audio_set_connected(bool connected)
{
    if (audio_connected && !connected) {
        ds4_audio_reset();
    }
    audio_connected = connected;
    if (connected) {
        pending_mic_en = true;   /* re-apply mic enable on (re)connect */
        pending_volume = true;   /* amp starts at its power-on default: push
                                  * the host volume (seeded at 0 dB = max) */
        /* R03: the host still has its alt settings open; re-arm them. */
        usb_audio_link_up();
        /* R03: the DS4-layer mic enable was cleared on link loss; restore it
         * when the USB host still has the capture stream open. */
        if (usb_audio_mic_is_active())
            ds4_audio_set_mic_active(true);
    }
}

bool ds4_audio_is_connected(void)
{
    return audio_connected;
}
