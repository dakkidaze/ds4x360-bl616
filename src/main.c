/*
 * DS4Dongle BL618 — entry, FreeRTOS orchestration and data bridge.
 *
 * Bridges a Bluetooth Classic DualShock 4 to a wired-DS4-v2 USB persona.
 * Ported from awalol/DS4Dongle (Pico 2W) onto the DS5Dongle BL618
 * architecture: BT HID host (Bouffalo/Zephyr stack) + CherryUSB device.
 *
 * P1 scope: input bridge (BT 0x11 → USB 0x01), output bridge
 * (USB 0x05 → BT 0x11), pairing/blacklist/multi-controller, config
 * feature reports 0xF6-0xF9, LED status, idle disconnect.
 * Audio (SBC speaker / headset mic) follows in P2.
 */

#include "board.h"
#include "bflb_mtimer.h"
#include "bflb_gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "bt_hid_host.h"
#include "ds4_protocol.h"
#include "ds4_output.h"
#include "ds4_audio.h"
#include "usb_gamepad.h"
#include "spk_recovery_policy.h"
#include "led_status.h"
#include "config.h"

#include "bl616_glb.h"
#include "btble_lib_api.h"
#include "rfparam_adapter.h"
#include "hci_driver.h"
#include "bflb_mtd.h"
#include "easyflash.h"
#include "board_config.h"
#include "debug_log.h"
#include "log_event.h"
#ifdef DEBUG_CONSOLE
#include "debug_console.h"
#endif
#include <string.h>

#define STACK_WORDS(bytes) \
    (((bytes) + sizeof(StackType_t) - 1) / sizeof(StackType_t))

#define BT_TASK_STACK_SIZE    STACK_WORDS(1024*24)
#define BT_TASK_PRIORITY      (configMAX_PRIORITIES - 3)
#define USB_TASK_STACK_SIZE   STACK_WORDS(1024*12)
#define USB_TASK_PRIORITY     (configMAX_PRIORITIES - 1)
#define LED_TASK_STACK_SIZE   STACK_WORDS(1024*2)
#define LED_TASK_PRIORITY     (tskIDLE_PRIORITY + 1)
#define AUDIO_TASK_STACK_SIZE STACK_WORDS(1024*8)
#define AUDIO_TASK_PRIORITY   (configMAX_PRIORITIES - 2)
#define MICAUDIO_TASK_STACK_SIZE STACK_WORDS(1024*8)
#define MICAUDIO_TASK_PRIORITY  (configMAX_PRIORITIES - 4)
#define KEEPALIVE_TASK_STACK_SIZE STACK_WORDS(1024*2)
#define KEEPALIVE_TASK_PRIORITY   (tskIDLE_PRIORITY + 2)
/* R11: the feed task used to run at 1 KB while its path contained printf.
 * The prints are gone, but the value is still a configuration number, not a
 * measured margin — doubled here and to be confirmed with
 * main_log_stack_watermarks() on hardware. */
#define MICFEED_TASK_STACK_SIZE   STACK_WORDS(1024*2)
#define BOOT_HOLD_TICKS      30   /* 30 x 100ms = 3 seconds */
#define BOOT_CLICK_WINDOW    5    /* 500ms window between clicks */
#define RECONNECT_TIMEOUT_MS 2000
#define HANDSHAKE_TIMEOUT_US  (10ULL * 1000000ULL)
#define CONNECTING_TIMEOUT_TICKS pdMS_TO_TICKS(8000)
#define L2CAP_FALLBACK_TIMEOUT_TICKS pdMS_TO_TICKS(3000)

#define INPUT_QUEUE_ITEM_SZ  DS4_BT_INPUT_PAYLOAD_SIZE      /* 63 */
#define USB_OUTPUT_BUF_SZ    (DS4_USB_OUTPUT_PAYLOAD_LEN + 1) /* 32 */

static QueueHandle_t input_queue;
static QueueHandle_t output_queue;
static volatile bool ds4_connected = false;
static volatile bool ever_connected = false;
static volatile bool battery_low = false;
static volatile bool bt_init_done = false;

static volatile uint8_t cached_battery_level = 0xFF;
static volatile uint8_t cached_battery_state = 0;

/* BT output report header byte 2 (bit7 EnableAudio, bit5 set,
 * bits0-2 EnableMic). P2 audio will toggle this for headset-mic. */
static volatile uint8_t ds4_out_hdr2 = DS4_OUT_HDR2_DEFAULT;

/* LED auto-off timers */
#define LED_DISABLE_OFF_US   (60ULL * 1000000ULL)  /* 1 minute */
static volatile uint64_t conn_led_start_us = 0;
static volatile bool conn_led_off = false;
static volatile uint64_t handshake_start_us;
static volatile TickType_t connecting_start_tick = 0;
static volatile bool prev_led_disabled = false;
static volatile bool scan_after_disconnect = false;
/* Deferred USB soft-disconnect: wait after BT disconnect before pulling
 * USB, so quick controller switches don't cause a host re-enumeration. */
#define USB_DEFERRED_DISC_US  (10ULL * 1000000ULL)
/* Finding 2 liveness backstop: how long the PHY may stay down while BT is
 * connected before main republishes a MAIN CONNECT. Tied to the latest
 * APPLIED PHY edge (usb_phy_snapshot), not to a poll observation, so an
 * intervening CONNECT/DISCONNECT pair always restarts it. Must exceed the
 * speaker recovery's planned hidden period (SPK_RECOV_DISCONNECT_MS = 100 ms
 * plus one apply tick) so a healthy bounce is never pre-empted; it bounds the
 * "latched with no BT event" case instead. The interval itself lives in
 * spk_recovery_policy.h (SPK_VISIBILITY_GRACE_MS) so the host test drives the
 * exact rule main uses. */
static volatile uint64_t usb_deferred_disc_us = 0;
static uint32_t usb_fwd_count = 0;

/* R01/R18: BT callbacks and per-report paths record counters, never printf. */
static volatile uint32_t bt_bad_rid_cnt = 0;
static volatile uint32_t bt_input_reports = 0;
static volatile uint32_t ext_change_cnt = 0;
static volatile uint32_t bt_primer_cnt = 0;

/* R11: defined with the task bookkeeping below, called from usb_task. */
static void log_stack_watermarks(void);
/* Drop the current task's handle before it deletes itself. */
static void task_unregister_current(void);

static volatile uint64_t out_isr_ts_us = 0;   /* timestamp set in USB ISR */
static volatile uint32_t out_drop_count = 0;  /* queue-full drops */

/* ---- BT HID callbacks ---- */

/* Build and queue a DS4 BT output report 0x11 from a USB-0x05-style
 * payload (flags byte first). ds4_build_output() writes the CRC-32;
 * bt_hid_host_send_output() prepends the 0xA2 transport byte.
 * Shared with ds4_audio (volume/mic-enable pushes) — see ds4_output.h. */
void ds4_send_output_payload(const uint8_t *payload, uint16_t len)
{
    uint8_t pkt[DS4_BT_OUTPUT_REPORT_SIZE];
    ds4_build_output(pkt, payload, len, ds4_out_hdr2);
    bt_hid_host_send_output(pkt, DS4_BT_OUTPUT_REPORT_SIZE);
}

uint8_t ds4_get_output_hdr2(void)
{
    return ds4_out_hdr2;
}

void ds4_set_output_hdr2(uint8_t v)
{
    ds4_out_hdr2 = v;
}

/* Called from l2cap_intr_connected (earliest possible moment).
 * DS4Dongle on HID open: gold lightbar, then headset L/R + speaker
 * volume defaults 0x50. */
static void on_hid_primer(void)
{
    uint8_t payload[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0};

    payload[0] = 0x02;   /* flags: enable LED update */
    payload[5] = 0xFF;   /* LED R — gold */
    payload[6] = 0xD7;   /* LED G */
    payload[7] = 0x00;   /* LED B */
    ds4_send_output_payload(payload, sizeof(payload));

    memset(payload, 0, sizeof(payload));
    payload[0] = 0xB0;   /* update headset L/R (0x10/0x20) + speaker (0x80) */
    payload[18] = 0x50;  /* VolumeHeadphones left */
    payload[19] = 0x50;  /* VolumeHeadphones right */
    payload[21] = 0x50;  /* VolumeSpeaker */
    ds4_send_output_payload(payload, sizeof(payload));

    bt_primer_cnt++;   /* L2CAP connected callback: counter only */
}

/* DS4 BT input report handler. `data` excludes the 0xA1 transport byte:
 * data[0] = report id (0x11 state, 0x12..0x19 state + headset-mic audio),
 * data[1..2] = 0xC0 0x00 header, data[3..] = 63-byte state block identical
 * to USB report 0x01's payload. */
static void on_hid_input(const uint8_t *data, uint16_t len)
{
    if (len < 2)
        return;

    uint8_t rid = data[0];
    if (rid < DS4_BT_INPUT_REPORT_ID || rid > DS4_BT_INPUT_REPORT_ID_MAX) {
        bt_bad_rid_cnt++;   /* BT receive callback: counter only */
        return;
    }

    if (len < DS4_BT_INPUT_HEADER_SIZE + DS4_BT_INPUT_PAYLOAD_SIZE)
        return;

    /* Headset-mic audio rides in reports 0x12..0x19 (one SBC frame each).
     * Parse it regardless of the state-block sanity gate below — DS4Dongle
     * decodes the mic frame even when the state block is garbled. */
    if (rid != DS4_BT_INPUT_REPORT_ID) {
        /* No logging here: this is the Bluetooth receive callback. */
#ifdef DS4_FEATURE_AUDIO
        ds4_audio_mic_bt_data(data, len);
#endif
    }

    const uint8_t *p = data + DS4_BT_INPUT_HEADER_SIZE;

    /* During duplex audio some report variants carry a state block that is
     * NOT 0x11-layout (hid-playstation rejected forwarded reports with
     * num_touch_reports=213). Only forward states that pass the touch-count
     * sanity check; audio parsing (P2) is unaffected. */
    if (rid != DS4_BT_INPUT_REPORT_ID &&
        p[DS4_PAYLOAD_TOUCH_COUNT] > 4) {
        return;
    }

    uint8_t report[DS4_BT_INPUT_PAYLOAD_SIZE];
    memcpy(report, p, DS4_BT_INPUT_PAYLOAD_SIZE);
    xQueueOverwrite(input_queue, report);
    bt_input_reports++;   /* BT receive callback: counter only */
}

static void on_hid_state(enum bt_hid_host_state state)
{
    switch (state) {
    case BT_HID_STATE_IDLE: {
        handshake_start_us = 0;
        connecting_start_tick = 0;
#ifdef DS4_FEATURE_AUDIO
        ds4_audio_set_connected(false);
#endif
        bool was_connected = ds4_connected;
        if (ds4_connected) {
            ds4_connected = false;
            battery_low = false;
            cached_battery_level = 0xFF;
            cached_battery_state = 0;
            /* Neutral USB report so the host doesn't freeze on stale state */
            uint8_t neutral[DS4_USB_INPUT_PAYLOAD_LEN] = {0};
            neutral[0] = 0x80; neutral[1] = 0x80;
            neutral[2] = 0x80; neutral[3] = 0x80;
            neutral[DS4_PAYLOAD_HAT] = DS4_HAT_NEUTRAL;
            usb_gamepad_send_raw_input(neutral);
        }
        usb_fwd_count = 0;
        if (was_connected) {
            usb_deferred_disc_us = bflb_mtimer_get_time_us();
            conn_led_start_us = bflb_mtimer_get_time_us();
            conn_led_off = false;
        }
        if (!conn_led_off)
            led_status_set(was_connected ? LED_RED_BLINK : LED_PURPLE_BLINK_SLOW);
        if (scan_after_disconnect) {
            scan_after_disconnect = false;
            bt_hid_host_scan_start(0);
        } else {
        }
        break;
    }

    case BT_HID_STATE_SCANNING:
        if (!conn_led_off)
            led_status_set(LED_PURPLE_BLINK_FAST);
        break;

    case BT_HID_STATE_CONNECTING:
    case BT_HID_STATE_SDP_QUERY:
    case BT_HID_STATE_L2CAP_CONTROL:
    case BT_HID_STATE_L2CAP_INTERRUPT:
        connecting_start_tick = xTaskGetTickCount();
        if (!conn_led_off)
            led_status_set(LED_PURPLE_BLINK_FAST);
        break;

    case BT_HID_STATE_HANDSHAKE:
        connecting_start_tick = 0;
        handshake_start_us = bflb_mtimer_get_time_us();
        break;

    case BT_HID_STATE_CONNECTED: {
        connecting_start_tick = 0;
        handshake_start_us = 0;

        if (ever_connected) {
            if (usb_deferred_disc_us) {
                /* Reconnected inside the deferred-disconnect window. The
                 * deferred DISCONNECT may already have reached the PHY (or a
                 * recovery CONNECT may have been refused as stale), so the
                 * cancelled timer alone does not prove USB is visible. Never
                 * cancel it while the persona is hidden: re-assert the edge
                 * (usb_task applies it; overwriting a recovery request is
                 * fine, both want the same end state). */
                usb_deferred_disc_us = 0;
                {
                    usb_phy_snapshot_t phy_snap;

                    usb_phy_snapshot(&phy_snap);
                    if (!phy_snap.connected)
                        usb_phy_request(USB_PHY_REQ_CONNECT,
                                        USB_PHY_OWNER_MAIN);
                }
            } else {
                /* Owned PHY edge: usb_task applies it, so a speaker recovery
                 * in flight can no longer interleave with it. */
                usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_MAIN);
            }
        } else {
            /* First connection: USB was soft-disconnected at boot.
             * The DS4 handshake GETs (0x05) already switched the controller
             * to full 0x11 input reports — expose the device now. */
            usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_MAIN);
        }

        ds4_connected = true;
        ever_connected = true;
        battery_low = false;
        usb_deferred_disc_us = 0;
#ifdef DS4_FEATURE_AUDIO
        ds4_audio_set_connected(true);
#endif
        conn_led_start_us = bflb_mtimer_get_time_us();
        conn_led_off = false;
        led_status_set(LED_GREEN_SOLID);
        break;
    }

    default:
        break;
    }
}

/* ---- USB output callback (host → device) ----
 * Runs in USB interrupt context — only copy raw bytes, let bt_task
 * do the heavy lifting (CRC, BT report construction). */
static void on_usb_output(const uint8_t *data, uint16_t len)
{
    if (!ds4_connected || len < 2)
        return;

    if (data[0] != DS4_USB_OUTPUT_REPORT_ID)
        return;

    uint8_t msg[USB_OUTPUT_BUF_SZ];
    uint16_t copy = (len <= USB_OUTPUT_BUF_SZ) ? len : USB_OUTPUT_BUF_SZ;
    memcpy(msg, data, copy);
    if (copy < USB_OUTPUT_BUF_SZ)
        memset(msg + copy, 0, USB_OUTPUT_BUF_SZ - copy);
    if (xQueueIsQueueFullFromISR(output_queue)) {
        uint8_t dummy[USB_OUTPUT_BUF_SZ];
        xQueueReceiveFromISR(output_queue, dummy, NULL);
        out_drop_count++;
    }
    out_isr_ts_us = bflb_mtimer_get_time_us();
    xQueueSendToBackFromISR(output_queue, msg, NULL);
}

/* ---- FreeRTOS tasks ---- */

static struct bflb_device_s *btn_gpio;

static void boot_button_init(void)
{
    btn_gpio = bflb_device_get_by_name("gpio");
    /* GPIO2 is the bootstrap pin — the board's external pull-down keeps
     * it LOW for normal boot; pressing the BOOT button pulls it HIGH.
     * Do NOT add an internal pull-up (conflicts with external circuit). */
    bflb_gpio_init(btn_gpio, BOOT_BUTTON_PIN,
                   GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    LOG_INF("[BTN] GPIO%d init, raw=%d\n", BOOT_BUTTON_PIN,
           (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN));
}

static bool boot_button_pressed(void)
{
    /* Active HIGH: BOOT button connects GPIO2 to VCC (HIGH = pressed) */
    return bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN) != 0;
}

static void bt_task(void *arg)
{
    (void)arg;
    LOG_INF("[MAIN] Board: %s\n", BOARD_NAME);

    LOG_INF("[BT_TASK] btble_controller_init...\n");
    btble_controller_init(configMAX_PRIORITIES - 1);
    LOG_INF("[BT_TASK] hci_driver_init...\n");
    hci_driver_init();
    LOG_INF("[BT_TASK] bt_hid_host_init...\n");

    struct bt_hid_host_config cfg = {
        .input_cb   = on_hid_input,
        .state_cb   = on_hid_state,
        .primer_cb  = on_hid_primer,
        .target_vid = USB_GAMEPAD_VID,
        .target_pid = USB_GAMEPAD_PID,
    };

    int err = bt_hid_host_init(&cfg);
    if (err) {
        LOG_ERR("[MAIN] BT init failed: %d\n", err);
        task_unregister_current();   /* handle must not survive this task */
        vTaskDelete(NULL);
        return;
    }

    /* BT init complete — re-enable USB peripheral clock that BT may have disabled */
    GLB_Set_USB_CLK_From_WIFIPLL(1);
    bt_init_done = true;
    LOG_INF("[BT_TASK] BT init done, USB clock re-enabled, signaling usb_task\n");

    led_status_set(LED_PURPLE_BLINK_SLOW);

    LOG_INF("[BT_TASK] try_reconnect...\n");
    if (bt_hid_host_try_reconnect() == 0) {
        /* Passive reconnect: page scan is enabled, wait for the
         * controller to page us and create L2CAP channels. */
        LOG_INF("[MAIN] Waiting for controller to reconnect...\n");
        TickType_t start = xTaskGetTickCount();
        while (!ds4_connected &&
               (xTaskGetTickCount() - start) < pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS)) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!ds4_connected) {
            LOG_WRN("[MAIN] Reconnect timed out, starting scan\n");
            bt_hid_host_scan_start(0);
        }
    } else {
        LOG_INF("[MAIN] No bonded device, starting scan\n");
        bt_hid_host_scan_start(0);
    }
    LOG_INF("[BT_TASK] Entering main loop\n");

    uint8_t usb_out[USB_OUTPUT_BUF_SZ];
    uint16_t idle_ticks = 0;
    uint16_t loop_count = 0;
    uint64_t disconnecting_since = 0;
    uint64_t stale_conn_since = 0;
    uint16_t scan_retry_ticks = 0;
    uint8_t  scan_fail_count = 0;
#define MAX_SCAN_RETRIES 2

    /*
     * BOOT button gesture FSM (polled at 10 Hz). DS4Dongle semantics,
     * adapted to BL616 (no UF2 bootloader):
     *   single click  → switch to next bonded controller / fresh scan
     *   double click  → disconnect + fresh scan (pair a new controller)
     *   hold >= 3s    → clear all bonds + blacklist, then reset
     */
    enum { BTN_IDLE, BTN_PRESSING, BTN_HELD, BTN_WAIT } btn_fsm = BTN_IDLE;
    uint8_t  btn_press_samples = 0;
    uint8_t  btn_wait_samples  = 0;
    uint8_t  btn_click_count   = 0;
    uint64_t btn_last_poll_us  = 0;
    uint8_t  btn_debug_counter = 0;
    /* Finding 2/3 progress guarantee: the grace window is owned by the latest
     * APPLIED edge (generation + timestamp). usb_phy_main_connect_if_due()
     * snapshots, checks and publishes atomically, so no state is kept here. */

    prev_led_disabled = config_led_disabled();
    conn_led_start_us = bflb_mtimer_get_time_us();

    for (;;) {
        loop_count++;
        if ((loop_count % 50) == 1) {
            LOG_DBG("[BT_TASK] loop #%u state=%d conn=%d\n",
                   loop_count, bt_hid_host_get_state(), ds4_connected);
        }

        /* ---- BOOT button gesture FSM (10 Hz cadence) ---- */
        uint64_t btn_now = bflb_mtimer_get_time_us();
        if (btn_now - btn_last_poll_us >= 100000ULL) {
            btn_last_poll_us = btn_now;
            bool pressed = boot_button_pressed();

            if (++btn_debug_counter >= 50) {
                btn_debug_counter = 0;
                LOG_DBG("[BTN] GPIO%d raw=%d fsm=%d\n",
                       BOOT_BUTTON_PIN,
                       (int)bflb_gpio_read(btn_gpio, BOOT_BUTTON_PIN),
                       btn_fsm);
            }

            switch (btn_fsm) {
            case BTN_IDLE:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                }
                break;

            case BTN_PRESSING:
                if (pressed) {
                    btn_press_samples++;
                    if (btn_press_samples == 3) {
                        led_status_set(LED_RED_BLINK);
                    }
                    if (btn_press_samples >= BOOT_HOLD_TICKS) {
                        btn_click_count = 0;
                        btn_fsm = BTN_HELD;
                        LOG_INF("[BTN] held 3s — clearing all bonds\n");
                        bt_hid_host_clear_bonds();
                        led_status_set(LED_BLINK_TRIPLE);
                        idle_ticks = 0;
                    }
                } else {
                    btn_click_count++;
                    btn_fsm = BTN_WAIT;
                    btn_wait_samples = 0;
                    if (btn_press_samples >= 3) {
                        led_status_set(ds4_connected ? LED_GREEN_SOLID
                                                     : LED_PURPLE_BLINK_SLOW);
                    }
                }
                break;

            case BTN_HELD:
                if (!pressed) {
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;
                    LOG_INF("[BTN] released after hold — resetting\n");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    GLB_SW_System_Reset();
                }
                break;

            case BTN_WAIT:
                if (pressed) {
                    btn_fsm = BTN_PRESSING;
                    btn_press_samples = 1;
                } else if (++btn_wait_samples >= BOOT_CLICK_WINDOW) {
                    uint8_t clicks = btn_click_count;
                    btn_click_count = 0;
                    btn_fsm = BTN_IDLE;
                    btn_press_samples = 0;

                    if (clicks == 1) {
                        if (bt_hid_host_get_bonded_count() > 1) {
                            int next = bt_hid_host_switch_next();
                            LOG_INF("[BTN] Single click — switching to controller %d\n", next);
                        } else {
                            LOG_INF("[BTN] Single click — only 1 bonded, fresh scan\n");
                            scan_fail_count = 0;
                            if (ds4_connected) {
                                scan_after_disconnect = true;
                                bt_hid_host_disconnect();
                            } else {
                                bt_hid_host_scan_start(0);
                            }
                        }
                        idle_ticks = 0;
                    } else if (clicks >= 2) {
                        LOG_INF("[BTN] Double click — disconnect + fresh scan\n");
                        scan_fail_count = 0;
                        if (ds4_connected) {
                            scan_after_disconnect = true;
                            bt_hid_host_disconnect();
                        } else {
                            bt_hid_host_scan_start(0);
                        }
                        idle_ticks = 0;
                    }
                }
                break;
            }
        }

        bt_hid_host_persist_if_dirty();

        /* Periodic RSSI read (~every 5 s) */
        {
            static uint16_t rssi_tick = 0;
            if (++rssi_tick >= 500 && ds4_connected) {
                rssi_tick = 0;
                bt_hid_host_read_rssi(NULL);
            }
        }

        /* Fallback: if DISCONNECTING for >1.5 seconds, the remote is dead. */
        if (bt_hid_host_get_state() == BT_HID_STATE_DISCONNECTING) {
            if (disconnecting_since == 0)
                disconnecting_since = bflb_mtimer_get_time_us();
            else if ((bflb_mtimer_get_time_us() - disconnecting_since)
                     > 1500000ULL) {
                LOG_WRN("[BT_TASK] DISCONNECTING stuck >1.5s, forcing cleanup\n");
                bt_hid_host_force_disconnect();
                disconnecting_since = 0;
            }
        } else {
            disconnecting_since = 0;
        }

        /* Deferred USB soft-disconnect after BT loss. Published as an owned
         * request: usb_task is the only writer of the PHY, so a speaker
         * recovery cannot interleave with it. */
        if (usb_deferred_disc_us && !ds4_connected &&
            (bflb_mtimer_get_time_us() - usb_deferred_disc_us) > USB_DEFERRED_DISC_US) {
            usb_deferred_disc_us = 0;
            usb_phy_request(USB_PHY_REQ_DISCONNECT, USB_PHY_OWNER_MAIN);
            /* The CONNECTED handler runs in BT callback context and may be
             * racing this timeout. bt_hid_host updates its state before it
             * invokes that callback, so this read sees a reconnect that is
             * already in flight; re-asserting the edge after the disconnect
             * makes visibility the last publication. Same invariant as the
             * cancel path above: no timeout may leave USB hidden while the
             * controller is linked. */
            if (bt_hid_host_get_state() == BT_HID_STATE_CONNECTED)
                usb_phy_request(USB_PHY_REQ_CONNECT, USB_PHY_OWNER_MAIN);
            LOG_INF("[MAIN] Deferred USB disconnect\n");
        }

        /* Finding 1: idempotent visibility progress guarantee.
         *
         * The speaker recovery may latch (fail-closed) while BT stays
         * steadily connected; a latched recovery performs no reconnect and
         * main only published CONNECT from BT events, so USB could stay
         * hidden indefinitely. Every loop pass therefore checks the invariant
         * "BT linked => persona visible" through the shared policy rule: PHY
         * down and nothing in flight means publish a MAIN CONNECT.
         *
         * - No BT event is required; this runs unconditionally each pass.
         * - The rule includes "no request pending from any owner", so it
         *   cannot fight an in-flight DISCONNECT (deferred BT loss or the
         *   recovery's own bounce).
         * - Finding 2: the grace is tied to the latest applied PHY edge. Main
         *   publishes only when the last applied edge was a DISCONNECT and
         *   SPK_VISIBILITY_GRACE_MS have passed since THAT edge's timestamp.
         *   A new applied edge (generation change) restarts the window even
         *   if both edges happened between two loop passes, so polling can
         *   never publish CONNECT < 250 ms after the latest disconnect. The
         *   interval still leaves the recovery's planned 100 ms low period
         *   alone; it only covers a PHY that stays hidden because the
         *   recovery latched or abandoned the bounce.
         * - Finding 3: the snapshot, the direction/grace/pending checks and
         *   the publication are ONE IRQ-masked region inside
         *   usb_phy_main_connect_if_due(). A CONNECT/DISCONNECT pair applied
         *   after the snapshot can therefore no longer leave main publishing
         *   from an older disconnect timestamp; there is no separate
         *   snapshot/publish step here anymore.
         * - Publishing is idempotent: once a request is queued or the PHY is
         *   up, the helper does nothing, and usb_task applies at most one
         *   edge per drain. A no-op CONNECT (PHY already up) does not bump
         *   the edge identity either. */
        if (bt_hid_host_get_state() == BT_HID_STATE_CONNECTED) {
            (void)usb_phy_main_connect_if_due(
                (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL),
                SPK_VISIBILITY_GRACE_MS);
        }

        if (bt_hid_host_poll_scan_early()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_connect()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_security()) {
            idle_ticks = 0;
            continue;
        }

        if (bt_hid_host_poll_sdp()) {
            idle_ticks = 0;
            continue;
        }

        bt_hid_host_poll_security_watchdog();
        bt_hid_host_poll_incoming_l2cap_fallback();

        /* React to disable_led config toggle in real-time */
        bool cur_led_disabled = config_led_disabled();
        if (cur_led_disabled != prev_led_disabled) {
            prev_led_disabled = cur_led_disabled;
            if (cur_led_disabled) {
                conn_led_start_us = bflb_mtimer_get_time_us();
                conn_led_off = false;
            } else {
                conn_led_off = false;
                if (ds4_connected) {
                    led_status_set(battery_low ? LED_BLINK_BATTERY
                                               : LED_GREEN_SOLID);
                } else {
                    enum bt_hid_host_state s = bt_hid_host_get_state();
                    if (s == BT_HID_STATE_SCANNING ||
                        s == BT_HID_STATE_CONNECTING ||
                        s == BT_HID_STATE_SDP_QUERY ||
                        s == BT_HID_STATE_L2CAP_CONTROL ||
                        s == BT_HID_STATE_L2CAP_INTERRUPT)
                        led_status_set(LED_PURPLE_BLINK_FAST);
                    else
                        led_status_set(LED_PURPLE_BLINK_SLOW);
                }
            }
        }

        /* 1-min auto-off: applies to any pattern in the whitelist */
        if (config_led_disabled() && !conn_led_off &&
            led_status_can_auto_off()) {
            uint64_t now_us = bflb_mtimer_get_time_us();
            if (now_us - conn_led_start_us >= LED_DISABLE_OFF_US) {
                led_status_set(LED_OFF);
                conn_led_off = true;
            }
        }

        if (ds4_connected) {
            /* Output forwarding: USB report 0x05 → BT report 0x11.
             * Each USB output frame is independently forwarded (a real
             * DS4 output report carries the complete state every time). */
            if (xQueueReceive(output_queue, usb_out, pdMS_TO_TICKS(24)) == pdTRUE) {
                uint8_t payload[DS4_USB_OUTPUT_PAYLOAD_LEN] = {0};
                /* usb_out[0] = report id 0x05; payload follows */
                uint16_t plen = USB_OUTPUT_BUF_SZ - 1;
                memcpy(payload, usb_out + 1, plen);
                if (config_lock_volume()) {
                    payload[0] &= 0x0F;  /* strip volume-enable flag bits */
                }
                ds4_send_output_payload(payload, plen);
            }
            idle_ticks = 0;
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
            idle_ticks++;

            /* CONNECTING timeout: L2CAP channels didn't complete in time */
            {
                static TickType_t l2cap_timeout_start = 0;
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if ((s == BT_HID_STATE_CONNECTING ||
                     s == BT_HID_STATE_SDP_QUERY) &&
                    connecting_start_tick > 0) {
                    TickType_t elapsed = xTaskGetTickCount() - connecting_start_tick;
                    if (elapsed > CONNECTING_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect();
                    }
                } else if (s == BT_HID_STATE_L2CAP_CONTROL ||
                           s == BT_HID_STATE_L2CAP_INTERRUPT) {
                    if (l2cap_timeout_start == 0)
                        l2cap_timeout_start = xTaskGetTickCount();
                    TickType_t elapsed = xTaskGetTickCount() - l2cap_timeout_start;
                    if (elapsed > L2CAP_FALLBACK_TIMEOUT_TICKS) {
                        LOG_WRN("[MAIN] Connecting timeout (%d) — disconnecting\n", s);
                        connecting_start_tick = 0;
                        l2cap_timeout_start = 0;
                        bt_hid_host_disconnect();
                    }
                } else {
                    l2cap_timeout_start = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_HANDSHAKE) {
                bt_hid_host_handshake_tick();
                if (handshake_start_us > 0 &&
                    (bflb_mtimer_get_time_us() - handshake_start_us) >
                    HANDSHAKE_TIMEOUT_US) {
                    LOG_WRN("[MAIN] Handshake timeout — disconnecting\n");
                    handshake_start_us = 0;
                    bt_hid_host_disconnect();
                }
            }

            {
                enum bt_hid_host_state s = bt_hid_host_get_state();
                if (s != BT_HID_STATE_IDLE) {
                    stale_conn_since = 0;
                    scan_retry_ticks = 0;
                    if (s != BT_HID_STATE_SCANNING)
                        scan_fail_count = 0;
                }
            }

            if (bt_hid_host_get_state() == BT_HID_STATE_IDLE &&
                idle_ticks >= 20) {
                if (bt_hid_host_has_pending_conn()) {
                    if (stale_conn_since == 0)
                        stale_conn_since = bflb_mtimer_get_time_us();
                    else if ((bflb_mtimer_get_time_us() - stale_conn_since)
                             > 15ULL * 1000000ULL) {
                        LOG_WRN("[MAIN] conn_disconnected never fired, "
                               "dropping stale conn\n");
                        bt_hid_host_drop_stale_conn();
                        stale_conn_since = 0;
                    }
                    scan_retry_ticks = 0;
                } else {
                    stale_conn_since = 0;
                    scan_retry_ticks += idle_ticks;
                    uint16_t retry_interval = ever_connected ? 600 : 300;
                    if (scan_retry_ticks >= retry_interval) {
                        scan_retry_ticks = 0;
                        if (scan_fail_count < MAX_SCAN_RETRIES) {
                            scan_fail_count++;
                            bt_hid_host_scan_start(0);
                        }
                    }
                }
                idle_ticks = 0;
            }
        }
    }
}

static void usb_task(void *arg)
{
    (void)arg;

    /* Wait for BT init to complete — it reconfigures clocks and kills USB */
    LOG_INF("[USB_TASK] Waiting for BT init to complete...\n");
    while (!bt_init_done) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    LOG_INF("[USB_TASK] BT init done, starting USB init\n");

    usb_gamepad_init(on_usb_output);

    /* DS4Dongle production behavior: hide USB at boot. The device
     * enumerates only after a controller connects (CONNECTED state).
     * NOTE: debug builds keep this too — USB-up-from-boot wedges the
     * controller (device never enumerates at all), and pairing needs BT
     * without USB enumeration racing it. USB also comes up together with the
     * gamepad at CONNECTED, which is exactly when the interesting
     * enumeration happens. Logs go to the UART regardless of enumeration. */
    vTaskDelay(pdMS_TO_TICKS(150));
    usb_phy_request(USB_PHY_REQ_DISCONNECT, USB_PHY_OWNER_MAIN);
    usb_phy_process();

    uint8_t raw_report[DS4_BT_INPUT_PAYLOAD_SIZE];
    uint64_t last_activity_us = bflb_mtimer_get_time_us();
    uint64_t last_input_us    = bflb_mtimer_get_time_us();
    bool inactive_disconnected = false;

#define CONN_WATCHDOG_US (3ULL * 1000000ULL)  /* 3s without input → force disconnect */

    for (;;) {
        usb_gamepad_process_deferred();

        /* Single writer of the PHY UNPLUG bit: apply whichever edge main's BT
         * path or the speaker fault recovery published. */
        usb_phy_process();

        /* Connection watchdog: connected but nothing for 3s → the
         * controller is likely powered off. Force disconnect bypasses
         * the HCI ACK (a dead controller can't ACK). */
        if (ds4_connected && usb_fwd_count > 0 &&
            (bflb_mtimer_get_time_us() - last_input_us) > CONN_WATCHDOG_US) {
            LOG_WRN("[MAIN] Watchdog: no input for 3s, force disconnect\n");
            bt_hid_host_force_disconnect();
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count = 0;
        }

        TickType_t wait = pdMS_TO_TICKS(10);
        if (xQueueReceive(input_queue, raw_report, wait) == pdTRUE) {
            last_input_us = bflb_mtimer_get_time_us();
            usb_fwd_count++;
            if (usb_fwd_count == 1)
                LOG_DBG("[USB-FWD] First BT→USB input forwarded\n");
            else if ((usb_fwd_count % 5000) == 0)
                LOG_DBG("[USB-FWD] %lu reports forwarded\n",
                       (unsigned long)usb_fwd_count);

            /* Battery/ext byte: bit0-3 level, bit4 charging, bit5 headset */
            uint8_t ext = raw_report[DS4_PAYLOAD_BATT_EXT];
            uint8_t pct = ext & DS4_BATT_LEVEL_MASK;
            cached_battery_level = (pct >= 10) ? 100 : pct * 10;
            cached_battery_state = (ext & DS4_BATT_CHARGING_MASK) ? 1 : 0;

            /* Headset jack detection steers the 0x17 audio target */
            {
                static uint8_t last_ext = 0xFF;
                if (ext != last_ext) {
                    last_ext = ext;
                    ext_change_cnt++;   /* R18: counter instead of a log line */
                }
            }
#ifdef DS4_FEATURE_AUDIO
            ds4_audio_set_headset((ext & DS4_BATT_HEADSET_MASK) != 0);
#endif

            bool charging = (ext & DS4_BATT_CHARGING_MASK) != 0;
            bool is_low = !charging && (pct <= 1);
            if (ds4_connected) {
                if (is_low && !battery_low) {
                    battery_low = true;
                    led_status_set(LED_BLINK_BATTERY);
                    LOG_WRN("[MAIN] Battery low (%d/10)\n", pct);
                } else if (!is_low && battery_low) {
                    battery_low = false;
                    if (config_led_disabled() && conn_led_off)
                        led_status_set(LED_OFF);
                    else
                        led_status_set(LED_GREEN_SOLID);
                }
            }

            if (ds4_connected) {
                if (!ds4_is_neutral_input(raw_report, DS4_BT_INPUT_PAYLOAD_SIZE)) {
                    last_activity_us = bflb_mtimer_get_time_us();
                    inactive_disconnected = false;
                }

                uint8_t inact_min = config_inactive_minutes();
                if (inact_min > 0 && !inactive_disconnected) {
                    uint64_t elapsed_us = bflb_mtimer_get_time_us() - last_activity_us;
                    uint64_t timeout_us = (uint64_t)inact_min * 60ULL * 1000000ULL;
                    if (elapsed_us >= timeout_us) {
                        LOG_INF("[MAIN] Idle timeout (%d min) — disconnecting\n",
                               inact_min);
                        bt_hid_host_disconnect();
                        inactive_disconnected = true;
                    }
                }

                usb_gamepad_send_raw_input(raw_report);
            }
        }

        if (!ds4_connected) {
            last_activity_us = bflb_mtimer_get_time_us();
            inactive_disconnected = false;
        }
    }
}

static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_status_tick();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

uint8_t get_battery_level(void)  { return cached_battery_level; }
uint8_t get_battery_state(void)  { return cached_battery_state; }

/* ---- R11: task creation bookkeeping ---- */
#define MAX_TRACKED_TASKS 12
static TaskHandle_t s_task_handles[MAX_TRACKED_TASKS];
static const char *s_task_names[MAX_TRACKED_TASKS];
static int s_task_count = 0;
static volatile uint32_t s_task_create_fail = 0;

/* Never ignore a failed xTaskCreate again, and keep the handle so stack
 * high-water marks can be reported later. */
static void create_task_checked(TaskFunction_t fn, const char *name,
                                uint16_t stack_words, UBaseType_t prio)
{
    if (s_task_count >= MAX_TRACKED_TASKS) {
        s_task_create_fail++;
        return;
    }
    if (xTaskCreate(fn, name, stack_words, NULL, prio,
                    &s_task_handles[s_task_count]) != pdPASS) {
        s_task_create_fail++;
        return;
    }
    s_task_names[s_task_count] = name;
    s_task_count++;
}

/* Drop the current task's handle before it deletes itself, so the periodic
 * stack report never touches a freed TCB. */
static void task_unregister_current(void)
{
    TaskHandle_t self = xTaskGetCurrentTaskHandle();

    for (int i = 0; i < s_task_count; i++) {
        if (s_task_handles[i] != self)
            continue;
        for (int j = i; j + 1 < s_task_count; j++) {
            s_task_handles[j] = s_task_handles[j + 1];
            s_task_names[j] = s_task_names[j + 1];
        }
        s_task_count--;
        s_task_handles[s_task_count] = NULL;
        s_task_names[s_task_count] = NULL;
        return;
    }
}

/* One-shot stack report from task context. The numbers require hardware;
 * the call path itself is static. */
static void log_stack_watermarks(void)
{
    for (int i = 0; i < s_task_count; i++) {
        if (!s_task_handles[i] || !s_task_names[i])
            continue;
        LOG_INF("[STACK] %-7s high-water=%lu words\n", s_task_names[i],
                (unsigned long)uxTaskGetStackHighWaterMark(s_task_handles[i]));
    }

    /* R18: the counters that replaced per-frame log lines, reported once
     * instead of printing in the hot paths. */
    {
        extern volatile uint32_t g_sbc_tx_ok, g_sbc_tx_fail;
        extern volatile uint32_t g_mic_frames, g_mic_dec_ok, g_mic_dec_fail,
                                 g_mic_dec_reset_err;
        extern volatile uint32_t g_mic_ep_err_cnt;
        extern volatile uint32_t g_bt_intr_pkts, g_bt_intr_bad,
                                 g_bt_ctrl_pkts, g_bt_feature_pkts,
                                 g_bt_handshake_err;
        extern volatile uint32_t g_bt_tx_pool_empty, g_bt_tx_send_err;
        /* BT TX failure classes (bt_hid_host.c). Every one of these is a
         * reason a frame never reached the controller, so each must be
         * countable from the wire even when the log lines themselves were
         * dropped by the text ring. */
        extern volatile uint32_t g_bt_tx_submitted, g_bt_tx_stale_done,
                                 g_bt_tx_stale_frame, g_bt_tx_stale_meta,
                                 g_bt_tx_stale_conn, g_bt_tx_retry,
                                 g_bt_tx_retry_drop, g_bt_tx_would_block,
                                 g_bt_tx_drop_full, g_bt_tx_oversize,
                                 g_bt_tx_inflight_timeout, g_bt_ctrl_send_err;
        /* Audio faults (ds4_audio.c / ds4_usb_audio.c). */
        extern volatile uint32_t g_audio_mic_gain_zero, g_mic_publish_drop,
                                 g_audio_mic_fault, g_audio_spk_fault;
        /* Speaker fault recovery (ds4_usb_audio.c): episodes counts terminal
         * F0 faults, recovery the controlled reconnects, latched the
         * fail-closed state. */
        extern volatile uint32_t g_audio_spk_fault_episodes,
                                 g_audio_spk_recovery_cnt,
                                 g_audio_spk_recovery_latched;
        /* USB input-report arming outcome (usb_gamepad.c, ISR-counted). */
        extern volatile uint32_t g_usb_in_err, g_usb_in_sent;
        /* F0 safety-net outcome (bflb_usb_v2.c, ISR-published): idle host,
         * criterion hit, synthetic completion, entry into FAULT, late
         * completion dropped, cancel unproven past its deadline. */
        extern volatile uint32_t g_usb_f0_idle_cnt, g_usb_f0_safetynet_cnt,
                                 g_usb_f0_synth_cnt, g_usb_f0_fault_cnt,
                                 g_usb_f0_stale_drop_cnt,
                                 g_usb_f0_witness_timeout;
        extern volatile uint32_t g_usb_log_cnt, g_fault_reason;

        /* One snapshot, several correlated records: log_text_printf()
         * formats into a 384-byte buffer and truncates silently, so a single
         * line would drop exactly the tail fields (textq/fault/ctrl_err)
         * that this report exists for. All of them are enqueued back to back
         * from this one task-context call site, so they stay in order on the
         * wire even though the individual records may be dropped. The widest
         * line below expands to 156 bytes with ten-digit counters. */
        LOG_INF("[DIAG] tx taskfail=%lu bt_rid=%lu bt_in=%lu primer=%lu ext=%lu "
                "sbc=%lu/%lu mic_frames=%lu mic_ep_err=%lu "
                "dec=%lu/%lu/%lu bt_rx=%lu/%lu/%lu/%lu hs_err=%lu "
                "txpool=%lu txerr=%lu usblog=%lu textdrop=%lu textq=%lu "
                "fault=%lu\n",
                (unsigned long)s_task_create_fail,
                (unsigned long)bt_bad_rid_cnt,
                (unsigned long)bt_input_reports,
                (unsigned long)bt_primer_cnt,
                (unsigned long)ext_change_cnt,
                (unsigned long)g_sbc_tx_ok,
                (unsigned long)g_sbc_tx_fail,
                (unsigned long)g_mic_frames,
                (unsigned long)g_mic_ep_err_cnt,
                (unsigned long)g_mic_dec_ok,
                (unsigned long)g_mic_dec_fail,
                (unsigned long)g_mic_dec_reset_err,
                (unsigned long)g_bt_intr_pkts,
                (unsigned long)g_bt_intr_bad,
                (unsigned long)g_bt_ctrl_pkts,
                (unsigned long)g_bt_feature_pkts,
                (unsigned long)g_bt_handshake_err,
                (unsigned long)g_bt_tx_pool_empty,
                (unsigned long)g_bt_tx_send_err,
                (unsigned long)g_usb_log_cnt,
                (unsigned long)log_text_dropped(),
                (unsigned long)log_text_pending(),
                (unsigned long)g_fault_reason);

        LOG_INF("[DIAG] btx submitted=%lu stale_done=%lu stale_frame=%lu "
                "stale_meta=%lu stale_conn=%lu retry=%lu retry_drop=%lu "
                "would_block=%lu drop_full=%lu oversize=%lu "
                "inflight_to=%lu ctrl_err=%lu\n",
                (unsigned long)g_bt_tx_submitted,
                (unsigned long)g_bt_tx_stale_done,
                (unsigned long)g_bt_tx_stale_frame,
                (unsigned long)g_bt_tx_stale_meta,
                (unsigned long)g_bt_tx_stale_conn,
                (unsigned long)g_bt_tx_retry,
                (unsigned long)g_bt_tx_retry_drop,
                (unsigned long)g_bt_tx_would_block,
                (unsigned long)g_bt_tx_drop_full,
                (unsigned long)g_bt_tx_oversize,
                (unsigned long)g_bt_tx_inflight_timeout,
                (unsigned long)g_bt_ctrl_send_err);

        LOG_INF("[DIAG] audio gain_zero=%lu pub_drop=%lu mic_fault=%lu "
                "spk_fault=%lu episodes=%lu recovery=%lu latched=%lu\n",
                (unsigned long)g_audio_mic_gain_zero,
                (unsigned long)g_mic_publish_drop,
                (unsigned long)g_audio_mic_fault,
                (unsigned long)g_audio_spk_fault,
                (unsigned long)g_audio_spk_fault_episodes,
                (unsigned long)g_audio_spk_recovery_cnt,
                (unsigned long)g_audio_spk_recovery_latched);

        LOG_INF("[DIAG] usb in_err=%lu in_sent=%lu\n",
                (unsigned long)g_usb_in_err,
                (unsigned long)g_usb_in_sent);

        LOG_INF("[DIAG] f0 idle=%lu safetynet=%lu synth=%lu fault=%lu "
                "stale=%lu witness=%lu\n",
                (unsigned long)g_usb_f0_idle_cnt,
                (unsigned long)g_usb_f0_safetynet_cnt,
                (unsigned long)g_usb_f0_synth_cnt,
                (unsigned long)g_usb_f0_fault_cnt,
                (unsigned long)g_usb_f0_stale_drop_cnt,
                (unsigned long)g_usb_f0_witness_timeout);
    }
}

/* R01: the single, lowest-priority consumer for everything that ISR / USB
 * completion / Bluetooth callbacks queue. Formatting and UART output are
 * unbounded in time, so this must never run at a real-time priority. */
static void log_task(void *arg)
{
    (void)arg;
    uint32_t next_report_ms = 30000u;   /* first snapshot 30 s after boot */

    for (;;) {
        /* Periodic, repeatable snapshot: a one-shot report at boot cannot
         * observe a fault that only appears later. Queued before the drains
         * below so it leaves on this iteration. */
        {
            uint32_t now_ms = (uint32_t)(bflb_mtimer_get_time_us() / 1000ULL);
            if ((int32_t)(now_ms - next_report_ms) >= 0) {
                log_stack_watermarks();
                next_report_ms = now_ms + 60000u;
            }
        }

        log_event_drain_print();
        /* R01 residual: task-context LOG_INF/WRN/DBG are queued, not printed
         * by the caller. Flush them here, after the event drain has had a
         * chance to enqueue its formatted lines. */
        log_text_drain();

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

int main(void)
{
    board_init();

    printf("\n=== DS4Dongle BL618 ===\n");
    LOG_INF("DualShock 4 Wireless Bridge\n");

    /* Flash speed diagnostic */
    {
        volatile uint32_t *glb_sf_cfg0 = (volatile uint32_t *)0x20000170;
        uint32_t sf_val  = *glb_sf_cfg0;
        uint8_t clk_sel  = (sf_val >> 12) & 0x3;
        uint8_t clk_sel2 = (sf_val >> 14) & 0x3;
        uint8_t clk_div  = (sf_val >> 8) & 0x7;
        unsigned mhz = 0;
        const char *src = "?";
        if (clk_sel == 0) {
            if (clk_sel2 == 1)      { src = "XTAL";   mhz = 40; }
            else if (clk_sel2 == 0) { src = "PLL120"; mhz = 120; }
            else if (clk_sel2 == 2) { src = "PLL96";  mhz = 96; }
        } else if (clk_sel == 1) { src = "PLL80";  mhz = 80; }
        else if (clk_sel == 2)   { src = "BCLK";   mhz = 0; }
        else if (clk_sel == 3)   { src = "PLL160"; mhz = 160; }
        if (clk_div > 0 && mhz > 0) mhz /= (clk_div + 1);
        LOG_INF("[B] Flash: %s/%u = %uMHz\n", src, clk_div, mhz);
    }

    /* Configure Exchange Memory for BT controller (required for BR/EDR) */
    {
        extern uint8_t __LD_CONFIG_EM_SEL;
        volatile uint32_t em_size = (uint32_t)&__LD_CONFIG_EM_SEL;
        if (em_size == 0)
            GLB_Set_EM_Sel(GLB_WRAM160KB_EM0KB);
        else if (em_size == 32 * 1024)
            GLB_Set_EM_Sel(GLB_WRAM128KB_EM32KB);
        else
            GLB_Set_EM_Sel(GLB_WRAM96KB_EM64KB);
        LOG_INF("[B] EM: %lu KB\n", em_size / 1024);
    }

    bflb_mtd_init();
    easyflash_init();
    if (rfparam_init(0, NULL, 0) != 0)
        LOG_ERR("[B] RF init FAIL\n");
    bflb_mtimer_delay_ms(3000);

    config_load();
    led_status_init();
    led_status_set(LED_PURPLE_BLINK_SLOW);
    boot_button_init();

#ifdef DS4_FEATURE_AUDIO
    ds4_audio_init();
#endif

    input_queue = xQueueCreate(1, INPUT_QUEUE_ITEM_SZ);
    /* Depth-1: BT callback always delivers the latest report to usb_task.
     * The 10-deep app_tx_fifo in bt_hid_host.c provides the BT output
     * flow control (one BT packet in-flight at a time). */
    output_queue = xQueueCreate(5, USB_OUTPUT_BUF_SZ);
    if (!input_queue || !output_queue) {
        LOG_ERR("[B] queue alloc FAIL\n");
        while (1) {}
    }

    create_task_checked(bt_task,    "bt",    BT_TASK_STACK_SIZE,
                        BT_TASK_PRIORITY);
    create_task_checked(usb_task,   "usb",   USB_TASK_STACK_SIZE,
                        USB_TASK_PRIORITY);
    create_task_checked(led_task,   "led",   LED_TASK_STACK_SIZE,
                        LED_TASK_PRIORITY);
#ifdef DS4_FEATURE_AUDIO
    create_task_checked(ds4_audio_task, "sbc", AUDIO_TASK_STACK_SIZE,
                        AUDIO_TASK_PRIORITY);
    create_task_checked(ds4_audio_mic_task, "micdec", MICAUDIO_TASK_STACK_SIZE,
                        MICAUDIO_TASK_PRIORITY);
    create_task_checked(ds4_audio_keepalive_task, "kalive",
                        KEEPALIVE_TASK_STACK_SIZE, KEEPALIVE_TASK_PRIORITY);
    create_task_checked(ds4_audio_mic_feed_task, "micfeed",
                        MICFEED_TASK_STACK_SIZE, configMAX_PRIORITIES - 1);
#endif
#ifdef DEBUG_CONSOLE
    create_task_checked(debug_console_task, "console", STACK_WORDS(1024*2),
                        tskIDLE_PRIORITY + 1);
#endif
    /* Lowest application priority: log formatting must never delay the
     * BT/USB/audio tasks. 4 KB: this task owns the 256-byte format buffer,
     * log_event's text staging and the SDK vsnprintf frame. */
    create_task_checked(log_task, "log", STACK_WORDS(1024*4),
                        tskIDLE_PRIORITY + 1);
    /* R11/P2: never bring up a half-created system. */
    if (s_task_create_fail) {
        LOG_ERR("[B] task creation failed (%lu of %d), not starting scheduler\n",
                (unsigned long)s_task_create_fail, s_task_count);
        for (;;) { }
    }

    LOG_INF("[B] init done, starting scheduler\n");
    vTaskStartScheduler();

    while (1) {}
    return 0;
}

void vApplicationIdleHook(void)
{
    __asm volatile("wfi");
}
