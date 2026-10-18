#ifndef USB_GAMEPAD_H
#define USB_GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>

/* DS4 v2 wired persona (054C:09CC). Output reports arrive via EP0
 * SET_REPORT (no interrupt OUT endpoint). P2: audio uses interfaces
 * 0/1/2 (byte-identical to a real DS4 v2), gamepad is interface 3. */
#define USB_GAMEPAD_EP_IN       0x84
#define USB_GAMEPAD_EP_MPS      64
#define USB_GAMEPAD_INTERVAL_MS 1

#define USB_GAMEPAD_VID         0x054C
#define USB_GAMEPAD_PID         0x09CC

#ifdef DS4_FEATURE_AUDIO
#define USB_INTF_GAMEPAD        3
#else
#define USB_INTF_GAMEPAD        0   /* gamepad-only build: no audio interfaces */
#endif

/* DS4 USB report layout */
#define DS4_USB_REPORT_ID_INPUT    0x01
#define DS4_USB_REPORT_ID_OUTPUT   0x05
#define DS4_USB_INPUT_PAYLOAD_LEN  63
#define DS4_USB_OUTPUT_PAYLOAD_LEN 31
#define FEATURE_DATA_MAX           256

typedef void (*usb_gamepad_output_cb_t)(const uint8_t *data, uint16_t len);

int usb_gamepad_init(usb_gamepad_output_cb_t output_cb);

int usb_gamepad_send_raw_input(const uint8_t *payload);
bool usb_gamepad_is_ready(void);

void usb_gamepad_set_suspend_hooks(void (*on_suspend)(void),
                                   void (*on_resume)(void),
                                   void (*on_configured)(void));

void usb_gamepad_set_polling_rate(uint8_t mode);

void usb_gamepad_process_deferred(void);

/* ---- PHY ownership (review finding 3) ----
 * The UNPLUG bit has exactly one writer: usb_task. Every other context
 * publishes a request; usb_task drains it via usb_phy_process(). */
typedef enum {
    USB_PHY_OWNER_NONE = 0,
    USB_PHY_OWNER_MAIN,       /* BT connect / BT-loss deferred disconnect */
    USB_PHY_OWNER_RECOVERY,   /* speaker F0 auto-recovery                  */
} usb_phy_owner_t;

typedef enum {
    USB_PHY_REQ_NONE = 0,
    USB_PHY_REQ_CONNECT,
    USB_PHY_REQ_DISCONNECT,
} usb_phy_req_t;

/* Any task/ISR context. The last request wins; applied in order. */
void usb_phy_request(usb_phy_req_t req, usb_phy_owner_t owner);
/* Is there an unapplied request from this owner? (0 = none) */
bool usb_phy_request_pending(usb_phy_owner_t owner);
/* Is there an unapplied request from ANY owner? Atomic read; used by main's
 * visibility progress guarantee so it never overwrites an in-flight edge. */
bool usb_phy_request_any_pending(void);

/* Queue req only when nothing is in flight and the applied-edge generation
 * still equals expect_gen (review finding 2). Atomic with usb_phy_process(),
 * so an external CONNECT applied after the caller's baseline can never be
 * overwritten by a stale recovery retry. Returns true when it was queued. */
bool usb_phy_request_if_gen(usb_phy_req_t req, usb_phy_owner_t owner,
                            uint32_t expect_gen);

/* ---- applied-edge identity (review findings 1 + 2) ----
 * Every APPLIED edge that actually changes the PHY direction (CONNECT or
 * DISCONNECT) bumps a generation and stamps a 32-bit millisecond timestamp.
 * "Applied" is decided by reading PHY_TST.UNPLUG back after the write: a lost
 * MMIO store is never recorded, the request stays pending and is retried on
 * the next pass (review finding 1). A publication whose direction equals the
 * direction the register reports is a NO-OP: it does not touch the register,
 * does not bump the generation and does not restamp the time. Redundant
 * CONNECT->CONNECT / DISCONNECT->DISCONNECT requests therefore cannot restart
 * main's grace window or falsely satisfy the recovery's takeover check.
 *
 * Consumers that must reason about "how long since the last edge" or "did
 * anyone else apply an edge since I asked" use ONE snapshot: direction,
 * generation and timestamp are read together under the IRQ mask. `connected`
 * is the PHY_TST.UNPLUG bit itself, not a software shadow. Reading the
 * direction and the generation separately is forbidden for decisions: an
 * edge applied between the two reads made our own DISCONNECT look like an
 * external CONNECT (review finding 2).
 *
 * The generation starts at 0, meaning "no edge applied yet". */
typedef struct {
    uint32_t gen;            /* applied-edge generation (0 = none yet)     */
    uint32_t last_ms;        /* ms timestamp of the last real direction
                              * change                                    */
    bool     connected;      /* PHY_TST.UNPLUG read at the same instant    */
} usb_phy_snapshot_t;

/* IRQ-masked snapshot of the applied edge: all three fields from one instant. */
void usb_phy_snapshot(usb_phy_snapshot_t *out);

/* Pure no-op rule, header-only and SDK-free so the host test drives the exact
 * rule usb_phy_process() applies. `cur` is the applied identity; applying a
 * direction equal to the current one returns it unchanged. */
typedef struct {
    bool     connected;
    uint32_t gen;
    uint32_t at_ms;
} usb_phy_identity_t;

static inline bool usb_phy_identity_changes(bool current_connected,
                                            bool requested_connect)
{
    return current_connected != requested_connect;
}

static inline usb_phy_identity_t
usb_phy_identity_apply(usb_phy_identity_t cur, bool requested_connect,
                       uint32_t now_ms)
{
    if (!usb_phy_identity_changes(cur.connected, requested_connect)) {
        return cur;                       /* no-op: identity unchanged */
    }
    cur.connected = requested_connect;
    cur.gen++;
    cur.at_ms = now_ms;
    return cur;
}

/* Main's visibility handoff (review finding 3). Under ONE IRQ-masked region:
 * take the applied-edge snapshot, require the last applied edge to be a
 * DISCONNECT at least grace_ms old, require no request pending from ANY
 * owner, and queue the MAIN CONNECT. The direction/grace/pending checks and
 * the publication are indivisible, so a CONNECT/DISCONNECT pair applied
 * between two loop passes can never leave main using an older disconnect
 * time. Returns true when the request was queued. */
bool usb_phy_main_connect_if_due(uint32_t now_ms, uint32_t grace_ms);

/* usb_task only: apply the pending request, if any. Final validation, the
 * UNPLUG write and its readback are done inside one bflb_irq_save() region,
 * so a host SET_INTERFACE/reset recorder cannot bump the recovery epoch
 * between the staleness check and the write, and a lost write is never
 * recorded as applied. It never sleeps, logs or calls the scheduler while
 * masked. */
void usb_phy_process(void);

/* Low-level primitives: usb_task / PHY-owner context only. */
void usb_soft_disconnect(void);
void usb_soft_connect(void);

#endif /* USB_GAMEPAD_H */
