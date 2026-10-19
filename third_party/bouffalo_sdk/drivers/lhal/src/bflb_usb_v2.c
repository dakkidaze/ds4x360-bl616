#include "bflb_core.h"
#include "usbd_core.h"
#include "usbh_core.h"
#include "hardware/usb_v2_reg.h"
#include "bflb_usb_v2_guard.h"

#ifndef CONFIG_USB_EHCI_HCOR_RESERVED_DISABLE
#error "usb host must enable CONFIG_USB_EHCI_HCOR_RESERVED_DISABLE"
#endif

/* R01: the F0 safety-net below runs inside the USB interrupt. Count the event
 * instead of printing from interrupt context.
 * P0 4.6: the whole guard is a pure decision table in bflb_usb_v2_guard.h;
 * these globals are the observable, ISR-only diagnostic counters. */
volatile uint32_t g_usb_f0_safetynet_cnt = 0;   /* criterion hit (deadlock)   */
volatile uint32_t g_usb_f0_synth_cnt = 0;       /* synthetic completions      */
volatile uint32_t g_usb_f0_stale_drop_cnt = 0;  /* dropped late completions   */
volatile uint32_t g_usb_f0_fault_cnt = 0;       /* entries into FAULT         */
volatile uint32_t g_usb_f0_witness_timeout = 0; /* cancel unproven past deadline (diag) */
volatile uint32_t g_usb_f0_misfire_cnt = 0;     /* blocked by criterion 3/4/6 */
volatile uint32_t g_usb_f0_idle_cnt = 0;        /* empty FIFO + no completion:
                                                 * idle host, deliberately not
                                                 * aborted (M1) */
/* P0: F0 safety-net guard endpoint. 0 disables the net entirely (a class
 * driver that double-arms F0 would otherwise see it fire spuriously). The
 * application sets this in usb_audio_register() to the ISO OUT endpoint it
 * owns. */
volatile uint8_t g_usb_f0_guard_ep = 0;

/* P0 4.6: weak fault notification. The default implementation is a no-op so
 * the driver stays usable without an audio class; the application overrides
 * it to count the fault, log it and fail the stream closed. */
__attribute__((weak)) void usbd_event_f0_fault_handler(uint8_t busid, uint8_t ep)
{
    (void)busid;
    (void)ep;
}

/* F0 guard instance: touched by the USB ISR, by usbd_ep_open() and by the
 * endpoint (re)arm path only. */
static bflb_f0_guard_t g_f0_guard;

/* Copy the guard's decision counters into the observable globals. The guard
 * only ever increments them, so the published values are monotonic. */
static inline void bflb_usb_f0_publish_counters(void)
{
    g_usb_f0_safetynet_cnt = g_f0_guard.cnt.safetynet;
    g_usb_f0_misfire_cnt = g_f0_guard.cnt.misfire;
    g_usb_f0_stale_drop_cnt = g_f0_guard.cnt.stale_drop;
    g_usb_f0_witness_timeout = g_f0_guard.cnt.witness_timeout;
    g_usb_f0_synth_cnt = g_f0_guard.cnt.synth;
    g_usb_f0_fault_cnt = g_f0_guard.cnt.fault;
    g_usb_f0_idle_cnt = g_f0_guard.cnt.idle;
}


/* select only one mode */
// #define CONFIG_USB_PINGPONG_ENABLE
// #define CONFIG_USB_TRIPLE_ENABLE

#if defined(BL616)
/* BL616 */
#include "bl616_irq.h"
#include "bl616_memorymap.h"

#define BFLB_USB_BASE                USB_BASE
#define BFLB_PDS_BASE                PDS_BASE
#define BFLB_USB_IRQ_NUM             BL616_IRQ_USB

/* 0x500 : usb_ctl */
#define PDS_USB_CTL_OFFSET           (0x500)
#define PDS_REG_USB_SW_RST_N         (1 << 0U)
#define PDS_REG_USB_EXT_SUSP_N       (1 << 1U)
#define PDS_REG_USB_WAKEUP           (1 << 2U)
#define PDS_REG_USB_L1_WAKEUP        (1 << 3U)
#define PDS_REG_USB_DRVBUS_POL       (1 << 4U)
#define PDS_REG_USB_IDDIG            (1 << 5U)

/* 0x504 : usb_phy_ctrl */
#define PDS_USB_PHY_CTRL_OFFSET      (0x504)
#define PDS_REG_USB_PHY_PONRST       (1 << 0U)
#define PDS_REG_USB_PHY_OSCOUTEN     (1 << 1U)
#define PDS_REG_USB_PHY_XTLSEL_SHIFT (2U)
#define PDS_REG_USB_PHY_XTLSEL_MASK  (0x3 << PDS_REG_USB_PHY_XTLSEL_SHIFT)
#define PDS_REG_USB_PHY_OUTCLKSEL    (1 << 4U)
#define PDS_REG_USB_PHY_PLLALIV      (1 << 5U)
#define PDS_REG_PU_USB20_PSW         (1 << 6U)

#elif defined(BL616CL)
/* BL616CL */
#include "bl616cl_irq.h"
#include "bl616cl_memorymap.h"

#define BFLB_USB_BASE                     USB_BASE
#define BFLB_PDS_BASE                     PDS_BASE
#define BFLB_USB_IRQ_NUM                  BL616CL_IRQ_USB

/* 0x500 : usb_ctl */
#define PDS_USB_CTL_OFFSET                (0x500)
#define PDS_REG_USB_SW_RST_N              (1 << 0U)
#define PDS_REG_USB_L1_WAKEUP             (1 << 2U)
#define PDS_REG_USB_WAKEUP                (1 << 3U)
#define PDS_REG_USB_VBUS_VALID_FORCE      (1 << 4U)
#define PDS_REG_USB_VBUS_VALID_FROM_PHY   (1 << 5U)
#define PDS_REG_USB_VBUS_VALID_FROM_GPIO  (1 << 6U)
#define PDS_REG_USB_LP_MODE_UCLK_DISABLE  (1 << 7U)
#define PDS_REG_USB_EXIT_SUSPEND_N        (1 << 8U)
#define PDS_REG_USB_IDDIG                 (1 << 16U)
#define PDS_REG_PU_USB20_PSW              (1 << 31U)

/* 0x504 : usb_phy0_ctrl */
#define PDS_USB_PHY0_CTRL_OFFSET          (0x504)

/* 0x508 : usb_phy1_ctrl */
#define PDS_USB_PHY1_CTRL_OFFSET          (0x508)
#define PDS_REG_USB_PHY_SUSPENDM0_USE_REG (1 << 0U)
#define PDS_REG_USB_PHY_SIDDQ             (1 << 28U)

/* 0x50C : usb_phy2_ctrl */
#define PDS_USB_PHY2_CTRL_OFFSET          (0x50C)
#define PDS_REG_USB_PHY_VBUS_VLD_EXT_SEL0 (1 << 18U)

/* 0x510 : usb_phy3_ctrl */
#define PDS_USB_PHY3_CTRL_OFFSET          (0x510)
#define PDS_REG_USB_PHY_POR               (1 << 31U)

#elif defined(BL618DG)
/* BL618DG */
#include "bl618dg_irq.h"
#include "bl618dg_memorymap.h"

#define BFLB_USB_BASE                     USB_BASE
#define BFLB_PDS_BASE                     PDS_BASE
#define BFLB_USB_IRQ_NUM                  BL618DG_IRQ_USB

/* 0x500 : usb_ctl */
#define PDS_USB_CTL_OFFSET                (0x500)
#define PDS_REG_USB_SW_RST_N              (1 << 0U)
#define PDS_REG_USB_IDDIG                 (1 << 1U)
#define PDS_REG_USB_L1_WAKEUP             (1 << 2U)
#define PDS_REG_USB_WAKEUP                (1 << 3U)
#define PDS_REG_USB_VBUS_VALID_FORCE      (1 << 4U)
#define PDS_REG_USB_VBUS_VALID_FROM_PHY   (1 << 5U)
#define PDS_REG_USB_VBUS_VALID_FROM_GPIO  (1 << 6U)
#define PDS_REG_USB_LP_MODE_UCLK_DISABLE  (1 << 7U)
#define PDS_REG_USB_EXIT_SUSPEND_N        (1 << 8U)
#define PDS_REG_PU_USB20_PSW              (1 << 31U)

/* 0x504 : usb_phy0_ctrl */
#define PDS_USB_PHY0_CTRL_OFFSET          (0x504)

/* 0x508 : usb_phy1_ctrl */
#define PDS_USB_PHY1_CTRL_OFFSET          (0x508)
#define PDS_REG_USB_PHY_SUSPENDM0_USE_REG (1 << 0U)
#define PDS_REG_USB_PHY_SIDDQ             (1 << 28U)

/* 0x50C : usb_phy2_ctrl */
#define PDS_USB_PHY2_CTRL_OFFSET          (0x50C)
#define PDS_REG_USB_PHY_VBUS_VLD_EXT_SEL0 (1 << 18U)

/* 0x510 : usb_phy3_ctrl */
#define PDS_USB_PHY3_CTRL_OFFSET          (0x510)
#define PDS_REG_USB_PHY_POR               (1 << 31U)

#else
#error "No support USB_V2 for this chip"
#endif

#define USB_SOF_TIMER_MASK_AFTER_RESET_HS (0x44C)
#define USB_SOF_TIMER_MASK_AFTER_RESET_FS (0x2710)

extern void USBD_IRQHandler(uint8_t busid);

void USBD_IRQ(int irq, void *arg)
{
    USBD_IRQHandler(0);
}

static void bflb_usb_phy_init(void)
{
    uint32_t regval;

#if defined(BL616)
    /* USB_PHY_CTRL[3:2] reg_usb_phy_xtlsel=0                             */
    /* 2000e504 = 0x40; #100; USB_PHY_CTRL[6] reg_pu_usb20_psw=1 (VCC33A) */
    /* 2000e504 = 0x41; #500; USB_PHY_CTRL[0] reg_usb_phy_ponrst=1        */
    /* 2000e500 = 0x20; #100; USB_CTL[0] reg_usb_sw_rst_n=0               */
    /* 2000e500 = 0x22; #500; USB_CTL[1] reg_usb_ext_susp_n=1             */
    /* 2000e500 = 0x23; #100; USB_CTL[0] reg_usb_sw_rst_n=1               */
    /* #1.2ms; wait UCLK                                                  */
    /* wait(soc616_b0.usb_uclk);                                          */

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval &= ~PDS_REG_USB_PHY_XTLSEL_MASK;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval |= PDS_REG_PU_USB20_PSW;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval |= PDS_REG_USB_PHY_PONRST;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    /* greater than 5T */
    bflb_mtimer_delay_us(1);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_SW_RST_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    /* greater than 5T */
    bflb_mtimer_delay_us(1);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_USB_EXT_SUSP_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    /* wait UCLK 1.2ms */
    bflb_mtimer_delay_ms(3);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_USB_SW_RST_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    bflb_mtimer_delay_ms(2);

#elif defined(BL618DG) || defined(BL616CL)

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_PU_USB20_PSW;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    bflb_mtimer_delay_us(50);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_SW_RST_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    /* wait XXX */
    bflb_mtimer_delay_us(50);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_USB_SW_RST_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    bflb_mtimer_delay_us(50);

    /* power on usb phy analog blocks*/
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY1_CTRL_OFFSET);
    regval &= ~PDS_REG_USB_PHY_SIDDQ;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY1_CTRL_OFFSET);

    bflb_mtimer_delay_us(50);

    /* reset usb macro */
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY3_CTRL_OFFSET);
    regval |= PDS_REG_USB_PHY_POR;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY3_CTRL_OFFSET);

    bflb_mtimer_delay_us(50);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY3_CTRL_OFFSET);
    regval &= ~PDS_REG_USB_PHY_POR;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY3_CTRL_OFFSET);

    bflb_mtimer_delay_us(50);

    /* External VBUS Valid Select */
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY2_CTRL_OFFSET);
    regval |= PDS_REG_USB_PHY_VBUS_VLD_EXT_SEL0;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY2_CTRL_OFFSET);

    /* SW force mode to assert VBUS valid signal */
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_VBUS_VALID_FROM_PHY;
    regval &= ~PDS_REG_USB_VBUS_VALID_FROM_GPIO;
    regval |= PDS_REG_USB_VBUS_VALID_FORCE;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

#endif
}

extern void USBH_IRQHandler(uint8_t busid);

void USBH_IRQ(int irq, void *arg)
{
    USBH_IRQHandler(0);
}

void usb_hc_low_level_init(struct usbh_bus *bus)
{
    uint32_t regval;

    bflb_usb_phy_init();

    bflb_irq_attach(BFLB_USB_IRQ_NUM, USBH_IRQ, NULL);
    bflb_irq_enable(BFLB_USB_IRQ_NUM);

    /* enable device-A for host */
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_IDDIG;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_OTG_CSR_OFFSET);
    regval |= USB_A_BUS_DROP_HOV;
    regval &= ~USB_A_BUS_REQ_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_OTG_CSR_OFFSET);

    bflb_mtimer_delay_ms(10);

    /* enable vbus and bus control */
    regval = getreg32(BFLB_USB_BASE + USB_OTG_CSR_OFFSET);
    regval &= ~USB_A_BUS_DROP_HOV;
    regval |= USB_A_BUS_REQ_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_OTG_CSR_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_GLB_INT_OFFSET);
    regval |= USB_MDEV_INT;
    regval |= USB_MOTG_INT;
    regval &= ~USB_MHC_INT;
    putreg32(regval, BFLB_USB_BASE + USB_GLB_INT_OFFSET);
}

void usb_hc_low_level_deinit(struct usbh_bus *bus)
{
}

uint8_t usbh_get_port_speed(struct usbh_bus *bus, const uint8_t port)
{
    uint8_t speed = 3;

    speed = (getreg32(BFLB_USB_BASE + USB_OTG_CSR_OFFSET) & USB_SPD_TYP_HOV_POV_MASK) >> USB_SPD_TYP_HOV_POV_SHIFT;

    if (speed == 0) {
        return USB_SPEED_FULL;
    } else if (speed == 1) {
        return USB_SPEED_LOW;
    } else if (speed == 2) {
        return USB_SPEED_HIGH;
    }
    return USB_SPEED_HIGH;
}

#define USB_FIFO_F0 0
#define USB_FIFO_F1 1
#define USB_FIFO_F2 2
#define USB_FIFO_F3 3
#if defined(BL618DG) || defined(BL616CL)
#define USB_FIFO_F4 4
#define USB_FIFO_F5 5
#define USB_FIFO_F6 6
#define USB_FIFO_F7 7
#endif
#define USB_FIFO_CXF          0xff

#define USB_FIFO_DIR_OUT      0
#define USB_FIFO_DIR_IN       1
#define USB_FIFO_DIR_BID      2

#define USB_VDMA_DIR_FIFO2MEM 0
#define USB_VDMA_DIR_MEM2FIFO 1

#if defined(BL616)
#define USB_MAX_EP_WITH_EP0     5
#define USB_MAX_EP_EXCLUDE_EP0  4
#define USB_NUM_BIDIR_ENDPOINTS 5
#endif
#if defined(BL618DG) || defined(BL616CL)
#define USB_MAX_EP_WITH_EP0     9
#define USB_MAX_EP_EXCLUDE_EP0  8
#define USB_NUM_BIDIR_ENDPOINTS 9
#endif

/* bmAttributes transfer-type code (USB 2.0, 9.6.6, bits 1:0).  ep_type below
 * stores exactly this field, so ISO-ness needs no second copy that could
 * drift away from the descriptor the host opened. */
#define USB_EP_TYPE_ISO 1u

/* Endpoint state */
struct bl_ep_state {
    uint16_t ep_mps;    /* Endpoint max packet size */
    uint8_t ep_type;    /* Endpoint type (bmAttributes 1:0, written by usbd_ep_open) */
    uint8_t ep_stalled; /* Endpoint stall flag */
    uint8_t ep_enable;  /* Endpoint enable */
    bool ep_active;
    uint8_t *xfer_buf;
    uint32_t xfer_len;
    uint32_t actual_xfer_len;
    /* P0 4.4/4.6: timestamp of the last arm of this endpoint (S4.4) and the
     * cancel witness for it. Both live in the driver, never in the class. */
    uint64_t arm_us;
    bflb_ep_cancel_witness_t cancel;
};

struct bl_udc {
    struct bl_ep_state in_ep[USB_NUM_BIDIR_ENDPOINTS];  /*!< IN endpoint parameters             */
    struct bl_ep_state out_ep[USB_NUM_BIDIR_ENDPOINTS]; /*!< OUT endpoint parameters            */
} g_bl_udc;

/* P0 B3: is this endpoint isochronous?  usbd_ep_open() is the only writer of
 * ep_type, so this answers "what did the host open this endpoint as?". */
static inline bool bflb_usb_ep_is_iso(uint8_t ep_type)
{
    return ep_type == USB_EP_TYPE_ISO;
}

static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_setup_buffer[8];

#ifdef CONFIG_USBDEV_TEST_MODE
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t g_test_packet_buffer[53];
#endif

static void bflb_usb_reset_fifo(uint8_t fifo)
{
    uint32_t regval;

    if (fifo == USB_FIFO_CXF) {
        regval = getreg32(BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
        regval |= USB_CX_CLR;
        putreg32(regval, BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
    } else {
        if (fifo < 4) {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_FIBC0_OFFSET + 4 * fifo);
            regval |= USB_FFRST0_HOV;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_FIBC0_OFFSET + 4 * fifo);
        } else {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_FCFG4_OFFSET + 4 * (fifo - 4));
            regval |= USB_FFRST0_HOV;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_FCFG4_OFFSET + 4 * (fifo - 4));
        }
    }
}

void bflb_usb_get_setup_packet(uint32_t setup[2])
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DMA_TFN_OFFSET);
    regval |= USB_ACC_CXF_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DMA_TFN_OFFSET);

    setup[0] = getreg32(BFLB_USB_BASE + USB_DMA_CPS3_OFFSET);
    setup[1] = getreg32(BFLB_USB_BASE + USB_DMA_CPS3_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DMA_TFN_OFFSET);
    regval &= ~USB_ACC_CXF_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DMA_TFN_OFFSET);
}

static void bflb_usb_set_ep_fifomap(uint8_t ep_idx, uint8_t fifo)
{
    uint32_t regval;
    uint32_t regaddr;

    if (ep_idx < 5) {
        regaddr = BFLB_USB_BASE + USB_DEV_EPMAP0_OFFSET;
    } else {
        ep_idx = ep_idx - 4;
        regaddr = BFLB_USB_BASE + USB_DEV_EPMAP1_OFFSET;
    }

    regval = getreg32(regaddr);
    regval &= ~(0xff << ((ep_idx - 1) * 8));
    regval |= (fifo << ((ep_idx - 1) * 8));
    regval |= (fifo << ((ep_idx - 1) * 8 + 4));

    putreg32(regval, regaddr);
}

static void bflb_usb_set_fifo_epmap(uint8_t fifo, uint8_t ep_idx, uint8_t dir)
{
    uint32_t regval;
    uint32_t regaddr;

    if (fifo < 4) {
        regaddr = BFLB_USB_BASE + USB_DEV_FMAP_OFFSET;
    } else {
        fifo = fifo - 4;
        regaddr = BFLB_USB_BASE + USB_DEV_FMAP2_OFFSET;
    }

    regval = getreg32(regaddr);
    regval &= ~(0x3f << (fifo * 8));
    regval |= (ep_idx << (fifo * 8));
    regval |= (dir << (fifo * 8 + 4));

    putreg32(regval, regaddr);
}

static void bflb_usb_set_outep_mps(uint8_t ep_idx, uint16_t ep_mps)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
    regval &= ~USB_MAXPS_OEP1_MASK;
    regval |= ep_mps;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
}

int bflb_usb_get_inep_mps(uint8_t ep_idx)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);

    return regval &= USB_MAXPS_IEP1_MASK;
}

int bflb_usb_get_outep_mps(uint8_t ep_idx)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);

    return regval &= USB_MAXPS_OEP1_MASK;
}

static void bflb_usb_set_inep_mps(uint8_t ep_idx, uint16_t ep_mps)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
    regval &= ~USB_MAXPS_IEP1_MASK;
    regval |= ep_mps;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
}

static uint8_t bflb_usb_get_fifo_ep(uint8_t fifo)
{
    uint32_t regval;

    if (fifo < 4) {
        regval = (getreg32(BFLB_USB_BASE + USB_DEV_FMAP_OFFSET) & (0xf << (fifo * 8)));
        regval >>= (fifo * 8);
    } else {
        fifo = fifo - 4;
        regval = (getreg32(BFLB_USB_BASE + USB_DEV_FMAP2_OFFSET) & (0xf << (fifo * 8)));
        regval >>= (fifo * 8);
    }
    return regval;
}

static void bflb_usb_fifo_config(uint8_t fifo, uint8_t ep_type, uint16_t block_size, uint8_t block_num, bool fifo_en)
{
    uint32_t regval;
    uint32_t regaddr;

    if (fifo < 4) {
        regaddr = BFLB_USB_BASE + USB_DEV_FCFG_OFFSET;
    } else {
        fifo = fifo - 4;
        regaddr = BFLB_USB_BASE + USB_DEV_FCFG2_OFFSET;
    }

    regval = getreg32(regaddr);
    regval &= ~(0x3f << (fifo * 8));
    regval |= (ep_type << (fifo * 8 + 0));
    regval |= ((block_num - 1) << (fifo * 8 + 2));
    if (block_size == 1024) {
        regval |= (1 << (fifo * 8 + 4));
    }

    if (fifo_en) {
        regval |= (1 << (fifo * 8 + 5));
    }

    putreg32(regval, regaddr);
}

static void bflb_usb_vdma_start_write(uint8_t fifo, const uint8_t *data, uint32_t len)
{
    uint32_t regval;
    uint32_t regaddr;
    uint32_t mem_regaddr;

    if (fifo == USB_FIFO_CXF) {
        regval = getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
        regval &= ~USB_VDMA_LEN_CXF_MASK;
        regval &= ~USB_VDMA_IO_CXF;
        regval |= USB_VDMA_TYPE_CXF;
        regval |= (len << USB_VDMA_LEN_CXF_SHIFT);
        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);

        putreg32((uint32_t)data, BFLB_USB_BASE + USB_VDMA_CXFPS2_OFFSET);

        regval = getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
        regval &= ~USB_VDMA_START_CXF;
        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
        regval |= USB_VDMA_START_CXF;
        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
    } else {
        if (fifo < 4) {
            regaddr = BFLB_USB_BASE + USB_VDMA_F0PS1_OFFSET + fifo * 8;
            mem_regaddr = BFLB_USB_BASE + USB_VDMA_F0PS2_OFFSET + fifo * 8;
        } else {
            fifo = fifo - 4;
            regaddr = BFLB_USB_BASE + USB_VDMA_FNPS1_OFFSET + fifo * 8;
            mem_regaddr = BFLB_USB_BASE + USB_VDMA_FNPS2_OFFSET + fifo * 8;
        }
        regval = getreg32(regaddr);
        regval &= ~USB_VDMA_LEN_CXF_MASK;
        regval &= ~USB_VDMA_IO_CXF;
        regval |= USB_VDMA_TYPE_CXF;
        regval |= (len << USB_VDMA_LEN_CXF_SHIFT);
        putreg32(regval, regaddr);

        putreg32((uint32_t)data, mem_regaddr);

        regval = getreg32(regaddr);
        regval &= ~USB_VDMA_START_CXF;
        putreg32(regval, regaddr);
        regval |= USB_VDMA_START_CXF;
        putreg32(regval, regaddr);
    }
}

static void bflb_usb_vdma_start_read(uint8_t fifo, uint8_t *data, uint32_t len)
{
    uint32_t regval;
    uint32_t regaddr;
    uint32_t mem_regaddr;

    if (fifo == USB_FIFO_CXF) {
        regval = getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
        regval &= ~USB_VDMA_LEN_CXF_MASK;
        regval &= ~USB_VDMA_IO_CXF;
        regval &= ~USB_VDMA_TYPE_CXF;
        regval |= (len << USB_VDMA_LEN_CXF_SHIFT);
        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);

        putreg32((uint32_t)data, BFLB_USB_BASE + USB_VDMA_CXFPS2_OFFSET);

        regval = getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
        regval |= USB_VDMA_START_CXF;
        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
    } else {
        if (fifo < 4) {
            regaddr = BFLB_USB_BASE + USB_VDMA_F0PS1_OFFSET + fifo * 8;
            mem_regaddr = BFLB_USB_BASE + USB_VDMA_F0PS2_OFFSET + fifo * 8;
        } else {
            fifo = fifo - 4;
            regaddr = BFLB_USB_BASE + USB_VDMA_FNPS1_OFFSET + fifo * 8;
            mem_regaddr = BFLB_USB_BASE + USB_VDMA_FNPS2_OFFSET + fifo * 8;
        }
        regval = getreg32(regaddr);
        regval &= ~USB_VDMA_LEN_CXF_MASK;
        regval &= ~USB_VDMA_IO_CXF;
        regval &= ~USB_VDMA_TYPE_CXF;
        regval |= (len << USB_VDMA_LEN_CXF_SHIFT);
        putreg32(regval, regaddr);

        putreg32((uint32_t)data, mem_regaddr);

        regval = getreg32(regaddr);
        regval |= USB_VDMA_START_CXF;
        putreg32(regval, regaddr);
    }
}

static uint32_t bflb_usb_vdma_get_remain_size(uint8_t fifo)
{
    uint32_t regval;
    uint32_t regaddr;

    if (fifo == USB_FIFO_CXF) {
        regval = (getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET) & USB_VDMA_LEN_CXF_MASK);
        regval >>= USB_VDMA_LEN_CXF_SHIFT;

    } else {
        if (fifo < 4) {
            regaddr = BFLB_USB_BASE + USB_VDMA_F0PS1_OFFSET + fifo * 8;
        } else {
            fifo = fifo - 4;
            regaddr = BFLB_USB_BASE + USB_VDMA_FNPS1_OFFSET + fifo * 8;
        }
        regval = (getreg32(regaddr) & USB_VDMA_LEN_CXF_MASK);
        regval >>= USB_VDMA_LEN_CXF_SHIFT;
    }

    return regval;
}

static inline void bflb_usb_control_transfer_done(void)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
    regval |= USB_CX_DONE;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
}

static inline void bflb_usb_set_mult(uint8_t ep_idx, uint8_t mult)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
    regval &= ~USB_TX_NUM_HBW_IEP1_MASK;
    regval |= (mult << USB_TX_NUM_HBW_IEP1_SHIFT);
    putreg32(regval, BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
}

static inline void bflb_usb_send_zlp(uint8_t ep_idx)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
    regval |= USB_TX0BYTE_IEP1;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
}

static inline uint32_t bflb_usb_get_tx_zlp_intstatus(void)
{
    return getreg32(BFLB_USB_BASE + USB_DEV_TXZ_OFFSET);
}

static inline uint32_t bflb_usb_get_rx_zlp_intstatus(void)
{
    return getreg32(BFLB_USB_BASE + USB_DEV_RXZ_OFFSET);
}

static inline void bflb_usb_clear_tx_zlp_intstatus(uint8_t ep_idx)
{
    putreg32((1 << (ep_idx - 1)), BFLB_USB_BASE + USB_DEV_TXZ_OFFSET);
}

static inline void bflb_usb_clear_rx_zlp_intstatus(uint8_t ep_idx)
{
    putreg32((1 << (ep_idx - 1)), BFLB_USB_BASE + USB_DEV_RXZ_OFFSET);
}

static uint32_t bflb_usb_get_source_group_intstatus(uint8_t group)
{
    switch (group) {
        case 0:
            return (getreg32(BFLB_USB_BASE + USB_DEV_ISG0_OFFSET) & ~getreg32(BFLB_USB_BASE + USB_DEV_MISG0_OFFSET));
        case 1:
            return (getreg32(BFLB_USB_BASE + USB_DEV_ISG1_OFFSET) & ~getreg32(BFLB_USB_BASE + USB_DEV_MISG1_OFFSET));
        case 2:
            return (getreg32(BFLB_USB_BASE + USB_DEV_ISG2_OFFSET) & ~getreg32(BFLB_USB_BASE + USB_DEV_MISG2_OFFSET));
        case 3:
            return (getreg32(BFLB_USB_BASE + USB_DEV_ISG3_OFFSET) & ~getreg32(BFLB_USB_BASE + USB_DEV_MISG3_OFFSET));
        case 4:
            return (getreg32(BFLB_USB_BASE + USB_DEV_ISG4_OFFSET) & ~getreg32(BFLB_USB_BASE + USB_DEV_MISG4_OFFSET));
        default:
            break;
    }
    return 0;
}

static void bflb_usb_source_group_int_clear(uint8_t group, uint32_t int_clear)
{
    switch (group) {
        case 0:
            putreg32(int_clear, BFLB_USB_BASE + USB_DEV_ISG0_OFFSET);
            break;
        case 1:
            putreg32(int_clear, BFLB_USB_BASE + USB_DEV_ISG1_OFFSET);
            break;
        case 2:
            putreg32(int_clear, BFLB_USB_BASE + USB_DEV_ISG2_OFFSET);
            break;
        case 3:
            putreg32(int_clear, BFLB_USB_BASE + USB_DEV_ISG3_OFFSET);
            break;
        case 4:
            putreg32(int_clear, BFLB_USB_BASE + USB_DEV_ISG4_OFFSET);
            break;

        default:
            break;
    }
}

static uint8_t usb_get_transfer_fifo(uint8_t ep_idx)
{
    uint8_t target_fifo_id;

    if ((g_bl_udc.out_ep[ep_idx].ep_mps > 512) || (g_bl_udc.in_ep[ep_idx].ep_mps > 512)) {
#if defined(CONFIG_USB_PINGPONG_ENABLE)
        target_fifo_id = (ep_idx - 1) * 4;
#elif defined(CONFIG_USB_TRIPLE_ENABLE)
        target_fifo_id = USB_FIFO_F0;
#else
        target_fifo_id = (ep_idx - 1) * 2;
#endif
    } else {
#if defined(CONFIG_USB_PINGPONG_ENABLE)
        target_fifo_id = (ep_idx - 1) * 2;
#elif defined(CONFIG_USB_TRIPLE_ENABLE)
        target_fifo_id = (ep_idx - 1) * 3;
#else
        target_fifo_id = (ep_idx - 1);
#endif
    }

    return target_fifo_id;
}

int usb_dc_init(uint8_t busid)
{
    uint32_t regval;

    bflb_usb_phy_init();

#ifndef NOT_USE_BFLB_LHAL_IRQ_ATTACH
    bflb_irq_attach(BFLB_USB_IRQ_NUM, USBD_IRQ, NULL);
    bflb_irq_enable(BFLB_USB_IRQ_NUM);
#endif

    /* disable device-A for host */
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_USB_IDDIG;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    /* disable global irq */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval &= ~USB_GLINT_EN_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
    regval |= USB_UNPLUG;
    putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval &= ~USB_CAP_RMWAKUP;
    regval |= USB_CHIP_EN_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
#ifdef CONFIG_USB_HS
    regval &= ~USB_FORCE_FS;
#else
    regval |= USB_FORCE_FS;
#endif
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval |= USB_SFRST_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    while (getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET) & USB_SFRST_HOV) {
    }

    regval = getreg32(BFLB_USB_BASE + USB_DEV_ADR_OFFSET);
    regval &= ~USB_AFT_CONF;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_ADR_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DEV_SMT_OFFSET);
    regval &= ~USB_SOFMT_MASK;
#ifdef CONFIG_USB_HS
    regval |= USB_SOF_TIMER_MASK_AFTER_RESET_HS;
#else
    regval |= USB_SOF_TIMER_MASK_AFTER_RESET_FS;
#endif
    putreg32(regval, BFLB_USB_BASE + USB_DEV_SMT_OFFSET);

    /* enable setup irq in source group0 */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_MISG0_OFFSET);
    regval &= ~USB_MCX_SETUP_INT;
    regval |= USB_MCX_IN_INT;
    regval |= (1 << 3);
    regval |= USB_MCX_OUT_INT;
    regval |= USB_MCX_IN_INT;
    regval |= USB_MCX_COMFAIL_INT;
    regval |= USB_MCX_COMABORT_INT;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_MISG0_OFFSET);

    /* disable all fifo irq in source group1 */
    putreg32(0xffffffff, BFLB_USB_BASE + USB_DEV_MISG1_OFFSET);

    /* enable rst/tx0/rx0 irq in source group2 */
    regval = 0xffffffff;
    regval &= ~USB_MUSBRST_INT;
    regval &= ~USB_MSUSP_INT;
    regval &= ~USB_MRESM_INT;
    regval &= ~USB_MTX0BYTE_INT;
    regval &= ~USB_MRX0BYTE_INT;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_MISG2_OFFSET);

    /* enable vdma cmplt and error irq in source group3 */
    regval = 0xffffffff;
    regval &= ~(USB_MVDMA_CMPLT_CXF |
#if (defined(BL618DG) || defined(BL616CL))
                USB_MVDMA_CMPLT_F4 |
                USB_MVDMA_CMPLT_F5 |
                USB_MVDMA_CMPLT_F6 |
                USB_MVDMA_CMPLT_F7 |
#endif
                USB_MVDMA_CMPLT_F0 |
                USB_MVDMA_CMPLT_F1 |
                USB_MVDMA_CMPLT_F2 |
                USB_MVDMA_CMPLT_F3);
    // regval &= ~(USB_MVDMA_ERROR_CXF |
    //             USB_MVDMA_ERROR_F0 |
    //             USB_MVDMA_ERROR_F1 |
    //             USB_MVDMA_ERROR_F2 |
    //             USB_MVDMA_ERROR_F3);
    putreg32(regval, BFLB_USB_BASE + USB_DEV_MISG3_OFFSET);

    /* enable group irq */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_MIGR_OFFSET);
    regval &= ~USB_MINT_G0;
    regval &= ~USB_MINT_G1;
    regval &= ~USB_MINT_G2;
    regval &= ~USB_MINT_G3;
    regval &= ~USB_MINT_G4;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_MIGR_OFFSET);

    /* enable device irq */
    regval = getreg32(BFLB_USB_BASE + USB_GLB_INT_OFFSET);
    regval |= USB_MHC_INT;
    regval |= USB_MOTG_INT;
    regval &= ~USB_MDEV_INT;
    putreg32(regval, BFLB_USB_BASE + USB_GLB_INT_OFFSET);

    bflb_usb_source_group_int_clear(2, 0x3ff);
    bflb_usb_source_group_int_clear(3, 0xffffffff);

    for (uint8_t i = 1; i < 9; i++) {
        bflb_usb_set_ep_fifomap(i, 15);
        bflb_usb_set_fifo_epmap(i, 15, 0);
    }

    /* enable vdma */
    regval = getreg32(BFLB_USB_BASE + USB_VDMA_CTRL_OFFSET);
    regval |= USB_VDMA_EN;
    putreg32(regval, BFLB_USB_BASE + USB_VDMA_CTRL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
    regval &= ~USB_UNPLUG;
    putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);

    /* enable USB_HC_CONN_DET_EN and USB_HC_WKP_DET_EN */
    regval = getreg32(BFLB_USB_BASE + USB_HCMISC_OFFSET);
    regval |= USB_HC_WKP_DET_EN;
    regval |= USB_HC_CONN_DET_EN;
    putreg32(regval, BFLB_USB_BASE + USB_HCMISC_OFFSET);

    /* enable global irq */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval |= USB_GLINT_EN_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

#if 0
    /* enable test mode for DV */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_TST_OFFSET);
    regval |= USB_TST_MOD_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_TST_OFFSET);
#endif

    return 0;
}

int usb_dc_deinit(uint8_t busid)
{
    uint32_t regval;

    /* disable global irq */
    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval &= ~USB_GLINT_EN_HOV;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
    regval |= USB_UNPLUG;
    putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);

#if defined(BL616)
    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval &= ~PDS_REG_USB_PHY_XTLSEL_MASK;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval &= ~PDS_REG_PU_USB20_PSW;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);
    regval &= ~PDS_REG_USB_PHY_PONRST;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_PHY_CTRL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_EXT_SUSP_N;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
#endif

    return 0;
}

int usbd_set_address(uint8_t busid, const uint8_t addr)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_ADR_OFFSET);
    regval &= ~USB_DEVADR_MASK;
    regval |= addr;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_ADR_OFFSET);

    return 0;
}

int usbd_set_remote_wakeup(uint8_t busid)
{
    uint32_t regval;

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval |= USB_CAP_RMWAKUP;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval |= PDS_REG_USB_WAKEUP;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    bflb_mtimer_delay_ms(10);

    regval = getreg32(BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);
    regval &= ~PDS_REG_USB_WAKEUP;
    putreg32(regval, BFLB_PDS_BASE + PDS_USB_CTL_OFFSET);

    regval = getreg32(BFLB_USB_BASE + USB_DEV_CTL_OFFSET);
    regval &= ~USB_CAP_RMWAKUP;
    putreg32(regval, BFLB_USB_BASE + USB_DEV_CTL_OFFSET);

    return 0;
}

uint8_t usbd_get_port_speed(uint8_t busid)
{
    uint8_t speed = 3;

    speed = (getreg32(BFLB_USB_BASE + USB_OTG_CSR_OFFSET) & USB_SPD_TYP_HOV_POV_MASK) >> USB_SPD_TYP_HOV_POV_SHIFT;

    if (speed == 0) {
        return USB_SPEED_FULL;
    } else if (speed == 1) {
        return USB_SPEED_LOW;
    } else if (speed == 2) {
        return USB_SPEED_HIGH;
    }
    return USB_SPEED_HIGH;
}

int usbd_ep_open(uint8_t busid, const struct usb_endpoint_descriptor *ep)
{
    uint8_t ep_addr;
    uint32_t regval;

    ep_addr = ep->bEndpointAddress;

    uint8_t ep_idx = USB_EP_GET_IDX(ep_addr);

#if defined(BL616)
    if ((ep_idx > 4) && (ep_idx < 9)) {
        return 0;
    }
#elif defined(BL618DG) || defined(BL616CL)
    if ((ep_idx > 8) && (ep_idx < 16)) {
        return 0;
    }
#endif

    /* P0 4.4/4.6: opening the guarded endpoint resets the deadlock budget -
     * the history of the previous endpoint/bus instance must not leak into
     * this one. The cancel witness is deliberately NOT cleared here: the
     * protocol re-opens an endpoint while the settle witness of the preceding
     * cancel is still outstanding (spec 4.3, ep_dirty path), so only
     * usbd_ep_cancel() may start a new one.
     *
     * N01: the same rule is what keeps the submit barrier honest. Clearing a
     * PENDING witness on open would discard the only record that a completion
     * may still leak out of the aborted transfer, re-opening the window where
     * that completion is credited to the transfer opened here. So this reset
     * covers the guard budget (arm_us / F0) only - never st->cancel. */
    if (ep_idx < USB_NUM_BIDIR_ENDPOINTS) {
        g_bl_udc.in_ep[ep_idx].arm_us = 0;
        g_bl_udc.out_ep[ep_idx].arm_us = 0;
        if (ep_idx == g_usb_f0_guard_ep) {
            bflb_f0_guard_reset(&g_f0_guard);
        }
    }

    if (USB_EP_DIR_IS_OUT(ep_addr)) {
        g_bl_udc.out_ep[ep_idx].ep_mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
        /* B3: this is the per-OUT-endpoint record of bmAttributes.  The
         * RX0BYTE handler reads it to keep ISO endpoints off the zero-length
         * completion path, so it must be written for every opened endpoint. */
        g_bl_udc.out_ep[ep_idx].ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
        g_bl_udc.out_ep[ep_idx].ep_enable = 1U;
        bflb_usb_set_outep_mps(ep_idx, USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize));
    } else {
        g_bl_udc.in_ep[ep_idx].ep_mps = USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize);
        g_bl_udc.in_ep[ep_idx].ep_type = USB_GET_ENDPOINT_TYPE(ep->bmAttributes);
        g_bl_udc.in_ep[ep_idx].ep_enable = 1U;
        bflb_usb_set_inep_mps(ep_idx, USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize));
        bflb_usb_set_mult(ep_idx, 0);
    }

    if (ep_idx != 0) {
#if !defined(CONFIG_USB_PINGPONG_ENABLE) && !defined(CONFIG_USB_TRIPLE_ENABLE)
        if (USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) > 512) {
            bflb_usb_set_ep_fifomap(1, USB_FIFO_F0);
            bflb_usb_set_ep_fifomap(2, USB_FIFO_F2);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_ep_fifomap(3, USB_FIFO_F4);
            bflb_usb_set_ep_fifomap(4, USB_FIFO_F6);
#endif

            bflb_usb_set_fifo_epmap(USB_FIFO_F0, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F1, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F2, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F3, 2, USB_FIFO_DIR_BID);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_fifo_epmap(USB_FIFO_F4, 3, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F5, 3, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F6, 4, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F7, 4, USB_FIFO_DIR_BID);
#endif
            if (ep_idx == 1) {
                bflb_usb_fifo_config(USB_FIFO_F0, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, true);
                bflb_usb_fifo_config(USB_FIFO_F1, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, false);
            } else if (ep_idx == 2) {
                bflb_usb_fifo_config(USB_FIFO_F2, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, true);
                bflb_usb_fifo_config(USB_FIFO_F3, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, false);
#if defined(BL618DG) || defined(BL616CL)
            } else if (ep_idx == 3) {
                bflb_usb_fifo_config(USB_FIFO_F4, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, true);
                bflb_usb_fifo_config(USB_FIFO_F5, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, false);
            } else if (ep_idx == 4) {
                bflb_usb_fifo_config(USB_FIFO_F6, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, true);
                bflb_usb_fifo_config(USB_FIFO_F7, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 1, false);
#endif
            } else {
                return -1;
            }
        } else {
            bflb_usb_set_ep_fifomap(1, USB_FIFO_F0);
            bflb_usb_set_ep_fifomap(2, USB_FIFO_F1);
            bflb_usb_set_ep_fifomap(3, USB_FIFO_F2);
            bflb_usb_set_ep_fifomap(4, USB_FIFO_F3);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_ep_fifomap(5, USB_FIFO_F4);
            bflb_usb_set_ep_fifomap(6, USB_FIFO_F5);
            bflb_usb_set_ep_fifomap(7, USB_FIFO_F6);
            bflb_usb_set_ep_fifomap(8, USB_FIFO_F7);
#endif

            bflb_usb_set_fifo_epmap(USB_FIFO_F0, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F1, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F2, 3, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F3, 4, USB_FIFO_DIR_BID);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_fifo_epmap(USB_FIFO_F4, 5, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F5, 6, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F6, 7, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F7, 8, USB_FIFO_DIR_BID);
#endif
            if (ep_idx == 1) {
                bflb_usb_fifo_config(USB_FIFO_F0, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 2) {
                bflb_usb_fifo_config(USB_FIFO_F1, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 3) {
                bflb_usb_fifo_config(USB_FIFO_F2, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 4) {
                bflb_usb_fifo_config(USB_FIFO_F3, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
#if defined(BL618DG) || defined(BL616CL)
            } else if (ep_idx == 5) {
                bflb_usb_fifo_config(USB_FIFO_F4, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 6) {
                bflb_usb_fifo_config(USB_FIFO_F5, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 7) {
                bflb_usb_fifo_config(USB_FIFO_F6, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
            } else if (ep_idx == 8) {
                bflb_usb_fifo_config(USB_FIFO_F7, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 1, true);
#endif
            } else {
                return -1;
            }
        }
#elif defined(CONFIG_USB_PINGPONG_ENABLE)
        if (USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) > 512) {
            bflb_usb_set_ep_fifomap(1, USB_FIFO_F0);

            bflb_usb_set_fifo_epmap(USB_FIFO_F0, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F1, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F2, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F3, 1, USB_FIFO_DIR_BID);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_fifo_epmap(USB_FIFO_F4, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F5, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F6, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F7, 2, USB_FIFO_DIR_BID);
#endif
            if (ep_idx == 1) {
                bflb_usb_fifo_config(USB_FIFO_F0, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F1, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
                bflb_usb_fifo_config(USB_FIFO_F2, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
                bflb_usb_fifo_config(USB_FIFO_F3, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
#if defined(BL618DG) || defined(BL616CL)
            } else if (ep_idx == 2) {
                bflb_usb_fifo_config(USB_FIFO_F4, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F5, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
                bflb_usb_fifo_config(USB_FIFO_F6, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
                bflb_usb_fifo_config(USB_FIFO_F7, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 1024, 2, false);
#endif
            } else {
                return -1;
            }
        } else {
            bflb_usb_set_ep_fifomap(1, USB_FIFO_F0);
            bflb_usb_set_ep_fifomap(2, USB_FIFO_F2);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_ep_fifomap(3, USB_FIFO_F4);
            bflb_usb_set_ep_fifomap(4, USB_FIFO_F6);
#endif
            bflb_usb_set_fifo_epmap(USB_FIFO_F0, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F1, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F2, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F3, 2, USB_FIFO_DIR_BID);
#if defined(BL618DG) || defined(BL616CL)
            bflb_usb_set_fifo_epmap(USB_FIFO_F4, 3, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F5, 3, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F6, 4, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F7, 4, USB_FIFO_DIR_BID);
#endif
            if (ep_idx == 1) {
                bflb_usb_fifo_config(USB_FIFO_F0, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F1, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, false);
            } else if (ep_idx == 2) {
                bflb_usb_fifo_config(USB_FIFO_F2, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F3, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, false);
#if defined(BL618DG) || defined(BL616CL)
            } else if (ep_idx == 3) {
                bflb_usb_fifo_config(USB_FIFO_F4, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F5, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, false);
            } else if (ep_idx == 4) {
                bflb_usb_fifo_config(USB_FIFO_F6, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, true);
                bflb_usb_fifo_config(USB_FIFO_F7, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 2, false);
#endif
            } else {
                return -1;
            }
        }
#elif defined(CONFIG_USB_TRIPLE_ENABLE)
        if (USB_GET_MAXPACKETSIZE(ep->wMaxPacketSize) > 512) {
            return -1;
        } else {
            bflb_usb_set_ep_fifomap(1, USB_FIFO_F0);
            bflb_usb_set_ep_fifomap(2, USB_FIFO_F3);

            bflb_usb_set_fifo_epmap(USB_FIFO_F0, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F1, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F2, 1, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F3, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F4, 2, USB_FIFO_DIR_BID);
            bflb_usb_set_fifo_epmap(USB_FIFO_F5, 2, USB_FIFO_DIR_BID);

            if (ep_idx == 1) {
                bflb_usb_fifo_config(USB_FIFO_F0, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, true);
                bflb_usb_fifo_config(USB_FIFO_F1, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, false);
                bflb_usb_fifo_config(USB_FIFO_F2, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, false);
            } else if (ep_idx == 2) {
                bflb_usb_fifo_config(USB_FIFO_F3, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, true);
                bflb_usb_fifo_config(USB_FIFO_F4, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, true);
                bflb_usb_fifo_config(USB_FIFO_F5, USB_GET_ENDPOINT_TYPE(ep->bmAttributes), 512, 3, true);
            } else {
                return -1;
            }
        }
#endif
        regval = getreg32(BFLB_USB_BASE + USB_DEV_ADR_OFFSET);
        regval |= USB_AFT_CONF;
        putreg32(regval, BFLB_USB_BASE + USB_DEV_ADR_OFFSET);
    }
    return 0;
}

static uint8_t bflb_usb_get_ep_fifo(uint8_t ep_idx)
{
    uint32_t regval;
    uint32_t regaddr;

    if (ep_idx == 0 || ep_idx > USB_NUM_BIDIR_ENDPOINTS) {
        return 0xff;
    }

    if (ep_idx < 5) {
        regaddr = BFLB_USB_BASE + USB_DEV_EPMAP0_OFFSET;
    } else {
        ep_idx = ep_idx - 4;
        regaddr = BFLB_USB_BASE + USB_DEV_EPMAP1_OFFSET;
    }

    regval = getreg32(regaddr);
    return (regval >> ((ep_idx - 1) * 8)) & 0x0f;
}

int usbd_ep_close(uint8_t busid, const uint8_t ep)
{
    (void)busid;
    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (ep_idx == 0) {
        return 0;
    }

    uint8_t fifo = bflb_usb_get_ep_fifo(ep_idx);
    if (fifo == 0xff || fifo == 0x0f) {
        return 0;
    }

    /* Clear VDMA start bit for this FIFO */
    uint32_t regval;
    uint32_t regaddr;

    if (fifo < 4) {
        regaddr = BFLB_USB_BASE + USB_VDMA_F0PS1_OFFSET + fifo * 8;
    } else {
        regaddr = BFLB_USB_BASE + USB_VDMA_FNPS1_OFFSET + (fifo - 4) * 8;
    }
    regval = getreg32(regaddr);
    regval &= ~USB_VDMA_START_CXF;
    putreg32(regval, regaddr);

    /* Reset FIFO hardware */
    bflb_usb_reset_fifo(fifo);

    /* Clear endpoint software state */
    if (USB_EP_DIR_IS_IN(ep)) {
        g_bl_udc.in_ep[ep_idx].ep_active = false;
        g_bl_udc.in_ep[ep_idx].xfer_buf = NULL;
        g_bl_udc.in_ep[ep_idx].xfer_len = 0;
        g_bl_udc.in_ep[ep_idx].actual_xfer_len = 0;
    } else {
        g_bl_udc.out_ep[ep_idx].ep_active = false;
        g_bl_udc.out_ep[ep_idx].xfer_buf = NULL;
        g_bl_udc.out_ep[ep_idx].xfer_len = 0;
        g_bl_udc.out_ep[ep_idx].actual_xfer_len = 0;
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* P0 4.4 / 4.6: endpoint abort, cancel and cancel witness                   */
/* ------------------------------------------------------------------------ */

/* VDMA PS1 register of a data FIFO (never CX). */
static uint32_t bflb_usb_vdma_ps1_regaddr(uint8_t fifo)
{
    if (fifo < 4) {
        return BFLB_USB_BASE + USB_VDMA_F0PS1_OFFSET + fifo * 8;
    }
    return BFLB_USB_BASE + USB_VDMA_FNPS1_OFFSET + (fifo - 4) * 8;
}

/* Is the FIFO's VDMA still running? (F0 safety-net criterion 3.) */
static bool bflb_usb_fifo_start_bit(uint8_t fifo)
{
    return (getreg32(bflb_usb_vdma_ps1_regaddr(fifo)) & USB_VDMA_START_CXF) != 0;
}

/* Latched G3 completion bit of a data FIFO: F0 -> bit 1 ... F7 -> bit 8. */
static inline uint32_t bflb_usb_fifo_cmplt_bit(uint8_t fifo)
{
    return (uint32_t)1u << (fifo + 1u);
}

/* Clear the VDMA START bit and reset the FIFO hardware (abort steps 1-3). */
static void bflb_usb_fifo_stop(uint8_t fifo)
{
    uint32_t regaddr = bflb_usb_vdma_ps1_regaddr(fifo);
    uint32_t regval = getreg32(regaddr);

    regval &= ~USB_VDMA_START_CXF;
    putreg32(regval, regaddr);
    bflb_usb_reset_fifo(fifo);
}

/* P0 4.6 ep_abort(): stop the FIFO and drop the software state of the OUT
 * endpoint mapped on it. No cancel bookkeeping - this is the guard's own
 * recovery, not an application cancel. The guarded endpoint is an OUT
 * endpoint by contract (F0 is the speaker ISO OUT FIFO). */
static void bflb_usb_ep_abort(uint8_t fifo)
{
    uint8_t ep_idx;

    bflb_usb_fifo_stop(fifo);

    ep_idx = bflb_usb_get_fifo_ep(fifo);
    if (ep_idx < USB_NUM_BIDIR_ENDPOINTS) {
        g_bl_udc.out_ep[ep_idx].ep_active = false;
        g_bl_udc.out_ep[ep_idx].xfer_buf = NULL;
        g_bl_udc.out_ep[ep_idx].xfer_len = 0;
        g_bl_udc.out_ep[ep_idx].actual_xfer_len = 0;
    }
}

/*
 * P0 4.4: abort one endpoint's transfer and arm the cancel witness.
 *
 * Steps 1-7 of the spec, in one interrupt-masked region (the function saves
 * and restores the mask itself). Returns 0 once the abort has been issued,
 * -1 when the endpoint cannot be cancelled (EP0, or no FIFO mapped).
 *
 * The witness is advanced from two places: (a) the clean-pass step of
 * USBD_IRQHandler's G3 branch, and (b) bflb_ep_cancel_hw_poll(), the hardware
 * poll that usbd_ep_cancel_settled() and the data-endpoint submit paths
 * (usbd_ep_start_write() / usbd_ep_start_read()) call. The witness timeout
 * counter is diagnostic only: it never advances the state and never settles.
 * Until the witness is SETTLED no completion of this FIFO may be dispatched.
 */
int usbd_ep_cancel(uint8_t busid, const uint8_t ep)
{
    uintptr_t irq_flags;
    uint8_t ep_idx;
    uint8_t fifo;
    struct bl_ep_state *st;

    (void)busid;

    irq_flags = bflb_irq_save();

    ep_idx = USB_EP_GET_IDX(ep);
    if (ep_idx == 0 || ep_idx >= USB_NUM_BIDIR_ENDPOINTS) {
        bflb_irq_restore(irq_flags);
        return -1;
    }

    fifo = bflb_usb_get_ep_fifo(ep_idx);
    if (fifo == 0xff || fifo == 0x0f) {
        bflb_irq_restore(irq_flags);
        return -1;
    }

    /* 2 + 3: clear START, then reset the FIFO. */
    bflb_usb_fifo_stop(fifo);

    /* 4: drop a completion bit that was latched before the abort. */
    bflb_usb_source_group_int_clear(3, bflb_usb_fifo_cmplt_bit(fifo));

    /* 5: the transfer is dead - release the software slot. */
    st = USB_EP_DIR_IS_IN(ep) ? &g_bl_udc.in_ep[ep_idx] : &g_bl_udc.out_ep[ep_idx];
    st->ep_active = false;
    st->xfer_buf = NULL;
    st->xfer_len = 0;
    st->actual_xfer_len = 0;

    /* 6: wait for one clean G3 pass before anything is dispatched again. */
    bflb_ep_cancel_begin(&st->cancel, bflb_mtimer_get_time_us());

    bflb_irq_restore(irq_flags);
    return 0;
}

/*
 * N01 / P0 4.4: witness *counter*, never a settlement source.
 *
 * The ISR can only advance a witness when a G3 pass happens. With the
 * cancelled FIFO idle and no other endpoint completing, no pass arrives, so
 * the class layer's query is the only observer. This helper keeps
 * g_usb_f0_witness_timeout meaningful by counting a cancel that stayed
 * unproven past the deadline - but it deliberately does NOT touch
 * st->cancel.state. Elapsed time is not a hardware observation: declaring
 * SETTLED from a timer would let a stale completion be dispatched against the
 * transfer armed after the barrier opened (fail-open).
 *
 * Callers must hold the interrupt mask; the state is re-checked there and the
 * timer is not read unless the witness is still PENDING.
 */
static void bflb_ep_cancel_count_timeout(struct bl_ep_state *st)
{
    if (!st || st->cancel.state != BFLB_EP_CANCEL_PENDING) {
        return;
    }
    if ((bflb_mtimer_get_time_us() - st->cancel.since_us) <
            BFLB_EP_CANCEL_WITNESS_TIMEOUT_US) {
        return;
    }
    if (!st->cancel.timeout_counted) {
        st->cancel.timeout_counted = 1;
        g_f0_guard.cnt.witness_timeout++;
        bflb_usb_f0_publish_counters();
    }
}

/*
 * P0 4.4: result of one *hardware* witness sample of a cancelled FIFO.
 *
 *   SETTLED       - both bits clear, witness latched SETTLED by the poll.
 *   PENDING_DIRTY - START clear but a completion was latched: the stale bit has
 *                   been cleared and counted; the witness stays PENDING.
 *   PENDING_START - VDMA START still set (the abort did not take); stays
 *                   PENDING. Consumes nothing.
 *   UNMAPPED      - endpoint has no data FIFO, nothing can be proven.
 */
enum bflb_ep_cancel_poll {
    BFLB_EP_CANCEL_POLL_SETTLED = 0,
    BFLB_EP_CANCEL_POLL_PENDING_DIRTY,
    BFLB_EP_CANCEL_POLL_PENDING_START,
    BFLB_EP_CANCEL_POLL_UNMAPPED,
};

/*
 * P0 4.4: the single hardware witness poll.
 *
 * This is the whole settlement rule, in one place, so that the observer
 * (usbd_ep_cancel_settled()) and the submit path (usbd_ep_start_write() /
 * usbd_ep_start_read()) cannot drift apart. The proof is hardware state for
 * the FIFO the endpoint is currently mapped to:
 *
 *   - VDMA START bit clear             : the aborted transfer no longer drives
 *                                        DMA.
 *   - latched G3 completion bit clear  : no completion of the aborted transfer
 *                                        is still waiting to be dispatched.
 *
 * Both clear -> latch SETTLED and report it. This is what closes the "idle
 * bus" wedge without fabricating settlement: an idle bus produces no G3 pass
 * for the ISR witness to advance on, but the two bits can still be read as a
 * state proof.
 *
 * START still set (the abort did not take) or the completion bit set (a late
 * completion leaked out of the aborted transfer) -> stay PENDING. A dirty
 * completion is dropped and counted exactly as usbd_ep_cancel() drops a
 * pre-latched bit; it is never dispatched and never advances the witness.
 * Repeated dirty passes therefore stay PENDING, so the caller - not this
 * helper - decides what to do with an endpoint that never proves quiet.
 *
 * There is no elapsed-time settlement anywhere in this driver: the 20 ms
 * deadline only feeds g_usb_f0_witness_timeout (see
 * bflb_ep_cancel_count_timeout()).
 *
 * Callers must hold the interrupt mask and must have checked that the witness
 * is PENDING. Reading the two bits, clearing a stale completion and latching
 * SETTLED therefore happen atomically with respect to the USB ISR and
 * usbd_ep_cancel().
 */
static enum bflb_ep_cancel_poll bflb_ep_cancel_hw_poll(struct bl_ep_state *st,
                                                       uint8_t ep_idx)
{
    uint8_t fifo;
    uint32_t g3_status;
    uint32_t cmplt_bit;
    bool start;

    fifo = bflb_usb_get_ep_fifo(ep_idx);
    if (fifo == 0xff || fifo == 0x0f) {
        return BFLB_EP_CANCEL_POLL_UNMAPPED;
    }

    cmplt_bit = bflb_usb_fifo_cmplt_bit(fifo);

    /* Read the VDMA START bit first, then the latched completion bit: the
     * abort is only "witnessed" if the DMA engine is already off AND nothing
     * of the aborted transfer is still latched. */
    start = bflb_usb_fifo_start_bit(fifo);
    g3_status = bflb_usb_get_source_group_intstatus(3);

    if (!start && (g3_status & cmplt_bit) == 0) {
        st->cancel.state = BFLB_EP_CANCEL_SETTLED;
        return BFLB_EP_CANCEL_POLL_SETTLED;
    }

    if (g3_status & cmplt_bit) {
        /* Dirty witness: this completion belongs to the aborted transfer.
         * Drop it (never dispatch it), count it, stay PENDING. */
        bflb_usb_source_group_int_clear(3, cmplt_bit);
        g_f0_guard.cnt.stale_drop++;
        bflb_usb_f0_publish_counters();
        return BFLB_EP_CANCEL_POLL_PENDING_DIRTY;
    }

    return BFLB_EP_CANCEL_POLL_PENDING_START;
}

/*
 * P0 4.4: has the cancelled FIFO proven itself silent?
 *
 * Observer entry point: the timeout counter is only a diagnostic, the verdict
 * comes from bflb_ep_cancel_hw_poll() - the same poll the submit path uses.
 * The poll is deliberately not time-gated: no elapsed time may ever latch
 * SETTLED, or a stale completion could be dispatched against the transfer
 * armed after the barrier opened (fail-open).
 */
bool usbd_ep_cancel_settled(uint8_t busid, const uint8_t ep)
{
    uintptr_t irq_flags;
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    struct bl_ep_state *st;
    bool settled;

    (void)busid;

    if (ep_idx == 0 || ep_idx >= USB_NUM_BIDIR_ENDPOINTS) {
        return false;
    }

    st = USB_EP_DIR_IS_IN(ep) ? &g_bl_udc.in_ep[ep_idx] : &g_bl_udc.out_ep[ep_idx];

    irq_flags = bflb_irq_save();

    if (st->cancel.state != BFLB_EP_CANCEL_PENDING) {
        settled = (st->cancel.state == BFLB_EP_CANCEL_SETTLED);
        bflb_irq_restore(irq_flags);
        return settled;
    }

    bflb_ep_cancel_count_timeout(st);

    settled = (bflb_ep_cancel_hw_poll(st, ep_idx) == BFLB_EP_CANCEL_POLL_SETTLED);

    bflb_irq_restore(irq_flags);
    return settled;
}

int usbd_ep_set_stall(uint8_t busid, const uint8_t ep)
{
    uint32_t regval;

    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (ep_idx == 0) {
        regval = getreg32(BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
        regval |= USB_CX_STL;
        putreg32(regval, BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
    } else {
        if (USB_EP_DIR_IS_OUT(ep)) {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
            regval |= USB_STL_OEP1;
            //regval |= USB_RSTG_OEP1;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
        } else {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
            regval |= USB_STL_IEP1;
            //regval |= USB_RSTG_IEP1;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
        }
    }

    return 0;
}

int usbd_ep_clear_stall(uint8_t busid, const uint8_t ep)
{
    uint32_t regval;

    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (ep_idx == 0) {
    } else {
        if (USB_EP_DIR_IS_OUT(ep)) {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
            //regval &= ~USB_RSTG_OEP1;
            regval &= ~USB_STL_OEP1;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
        } else {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
            //regval &= ~USB_RSTG_IEP1;
            regval &= ~USB_STL_IEP1;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
        }
    }

    return 0;
}

int usbd_ep_is_stalled(uint8_t busid, const uint8_t ep, uint8_t *stalled)
{
    uint32_t regval;

    uint8_t ep_idx = USB_EP_GET_IDX(ep);

    if (ep_idx == 0) {
    } else {
        if (USB_EP_DIR_IS_OUT(ep)) {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_OUTMPS1_OFFSET + (ep_idx - 1) * 4);
            if (regval & USB_STL_OEP1) {
                *stalled = 1;
            } else {
                *stalled = 0;
            }
        } else {
            regval = getreg32(BFLB_USB_BASE + USB_DEV_INMPS1_OFFSET + (ep_idx - 1) * 4);
            if (regval & USB_STL_IEP1) {
                *stalled = 1;
            } else {
                *stalled = 0;
            }
        }
    }

    return 0;
}

int usbd_ep_start_write(uint8_t busid, const uint8_t ep, const uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uintptr_t irq_flags;

    if (!data && data_len) {
        return -1;
    }

    /* P0 4.4 (R2.1): for a data endpoint the PENDING check, the software-slot
     * publication and the VDMA arm are ONE serialization region with
     * usbd_ep_cancel().
     *
     * The mask is required, not cosmetic: without it the HID task can pass
     * the PENDING test, get preempted by EP0 SET_INTERFACE, let
     * usbd_ep_cancel() stop the FIFO and release the software slot, then
     * resume to publish xfer_buf/ep_active and program VDMA for a transfer
     * whose cancel is still PENDING. That revives the cancelled transfer: its
     * completion is dropped by dispatch and the class busy flag never clears.
     * usbd_ep_cancel() takes the same mask, so after this change either the
     * submit finishes first (the cancel then aborts it) or the cancel
     * finishes first and this path observes PENDING. Observing PENDING is not
     * by itself a rejection: the submit path polls the hardware first and
     * proceeds only if that poll settles the witness in this same masked
     * region; otherwise it returns -3.
     *
     * The region is short - two checks, four stores, the guard stamp and the
     * VDMA start register writes - with no callback, no allocation and no
     * loop, so it is also safe from the USB ISR and from tasks that already
     * hold the core lock (the mask simply nests).
     *
     * Returns -3 (same negative convention as above) and touches neither
     * VDMA nor the software transfer slot. EP0 has no cancel witness and
     * keeps the original path below.
     *
     * PENDING is not a rejection by itself: the submit path is also a witness.
     * In an AUDIO=0 / HID-only configuration the cancelled FIFO is the only
     * user of G3, so no ISR pass ever arrives to advance the witness and a
     * bare PENDING test would reject every later report forever, even though
     * the endpoint is physically quiet. So the same bflb_ep_cancel_hw_poll()
     * that usbd_ep_cancel_settled() uses runs *inside this masked region*:
     *   - quiet hardware -> the poll latches SETTLED and the submission
     *     continues below, still under the same mask (one atomic step).
     *   - dirty hardware -> the poll drops the stale completion, counts it and
     *     the witness stays PENDING: return -3 and retry later.
     *   - VDMA still running -> stays PENDING: return -3.
     * No elapsed time is consulted, so settlement stays a hardware fact. */
    if (ep_idx != 0 && ep_idx < USB_NUM_BIDIR_ENDPOINTS) {
        irq_flags = bflb_irq_save();

        bflb_ep_cancel_count_timeout(&g_bl_udc.in_ep[ep_idx]);

        if (g_bl_udc.in_ep[ep_idx].cancel.state == BFLB_EP_CANCEL_PENDING &&
            bflb_ep_cancel_hw_poll(&g_bl_udc.in_ep[ep_idx], ep_idx) !=
                BFLB_EP_CANCEL_POLL_SETTLED) {
            bflb_irq_restore(irq_flags);
            return -3;
        }

        if (!g_bl_udc.in_ep[ep_idx].ep_enable) {
            bflb_irq_restore(irq_flags);
            return -2;
        }

        g_bl_udc.in_ep[ep_idx].xfer_buf = (uint8_t *)data;
        g_bl_udc.in_ep[ep_idx].xfer_len = data_len;
        g_bl_udc.in_ep[ep_idx].actual_xfer_len = 0;
        g_bl_udc.in_ep[ep_idx].ep_active = true;

        /* P0 4.4 (S4.4): timestamp every non-EP0 arm; P0 4.6: a fresh arm
         * restarts the guard's deadlock window. */
        g_bl_udc.in_ep[ep_idx].arm_us = bflb_mtimer_get_time_us();
        bflb_f0_guard_arm(&g_f0_guard, g_usb_f0_guard_ep, ep_idx,
                          g_bl_udc.in_ep[ep_idx].arm_us);
        if (data_len == 0) {
            bflb_usb_send_zlp(ep_idx);
        } else {
            bflb_usb_vdma_start_write(usb_get_transfer_fifo(ep_idx), data, data_len);
        }

        bflb_irq_restore(irq_flags);
        return 0;
    }

    if (!g_bl_udc.in_ep[ep_idx].ep_enable) {
        return -2;
    }

    g_bl_udc.in_ep[ep_idx].xfer_buf = (uint8_t *)data;
    g_bl_udc.in_ep[ep_idx].xfer_len = data_len;
    g_bl_udc.in_ep[ep_idx].actual_xfer_len = 0;
    g_bl_udc.in_ep[ep_idx].ep_active = true;

    if (data_len == 0) {
        g_bl_udc.in_ep[ep_idx].ep_active = false;
        bflb_usb_control_transfer_done();
        usbd_event_ep_in_complete_handler(busid, 0x80, 0);
    } else {
        data_len = MIN(data_len, g_bl_udc.in_ep[ep_idx].ep_mps);
        g_bl_udc.in_ep[ep_idx].xfer_len = data_len;
        bflb_usb_vdma_start_write(USB_FIFO_CXF, data, data_len);
    }

    return 0;
}

int usbd_ep_start_read(uint8_t busid, const uint8_t ep, uint8_t *data, uint32_t data_len)
{
    uint8_t ep_idx = USB_EP_GET_IDX(ep);
    uintptr_t irq_flags;

    if (!data && data_len) {
        return -1;
    }

    /* P0 4.4 (R2.1): same single serialization region as the IN path - see
     * usbd_ep_start_write(). The PENDING test, the software-slot publication
     * and the VDMA arm must not be separable by usbd_ep_cancel(), or a
     * canceled OUT transfer could be revived (and its buffer overwritten by
     * another producer) after the cancel released the slot. EP0 is exempt
     * and keeps the original path below.
     *
     * PENDING is resolved by the same in-region hardware poll as the IN path
     * (see usbd_ep_start_write() and bflb_ep_cancel_hw_poll()): a quiet FIFO
     * settles and this submission proceeds under the same mask, a dirty one
     * returns -3 with the stale completion dropped and counted. */
    if (ep_idx != 0 && ep_idx < USB_NUM_BIDIR_ENDPOINTS) {
        irq_flags = bflb_irq_save();

        bflb_ep_cancel_count_timeout(&g_bl_udc.out_ep[ep_idx]);

        if (g_bl_udc.out_ep[ep_idx].cancel.state == BFLB_EP_CANCEL_PENDING &&
            bflb_ep_cancel_hw_poll(&g_bl_udc.out_ep[ep_idx], ep_idx) !=
                BFLB_EP_CANCEL_POLL_SETTLED) {
            bflb_irq_restore(irq_flags);
            return -3;
        }

        if (!g_bl_udc.out_ep[ep_idx].ep_enable) {
            bflb_irq_restore(irq_flags);
            return -2;
        }

        if (data_len == 0) {
            /* A zero-length OUT is a synthetic completion, not an armed
             * transfer: nothing is published, so it stays outside the mask
             * (the class path may call back into the driver). A cancel
             * arriving in between finds an empty slot and cannot revive it. */
            bflb_irq_restore(irq_flags);
            bflb_usb_control_transfer_done();
            usbd_event_ep_out_complete_handler(busid, 0x00, 0);
            return 0;
        }

        g_bl_udc.out_ep[ep_idx].xfer_buf = (uint8_t *)data;
        g_bl_udc.out_ep[ep_idx].xfer_len = data_len;
        g_bl_udc.out_ep[ep_idx].actual_xfer_len = 0;
        g_bl_udc.out_ep[ep_idx].ep_active = true;

        /* P0 4.4 (S4.4): timestamp every non-EP0 arm; P0 4.6: a fresh arm
         * restarts the guard's deadlock window. */
        g_bl_udc.out_ep[ep_idx].arm_us = bflb_mtimer_get_time_us();
        bflb_f0_guard_arm(&g_f0_guard, g_usb_f0_guard_ep, ep_idx,
                          g_bl_udc.out_ep[ep_idx].arm_us);
        bflb_usb_vdma_start_read(usb_get_transfer_fifo(ep_idx), data, data_len);

        bflb_irq_restore(irq_flags);
        return 0;
    }

    if (!g_bl_udc.out_ep[ep_idx].ep_enable) {
        return -2;
    }

    if (data_len == 0) {
        bflb_usb_control_transfer_done();
        usbd_event_ep_out_complete_handler(busid, 0x00, 0);
        return 0;
    }

    g_bl_udc.out_ep[ep_idx].xfer_buf = (uint8_t *)data;
    g_bl_udc.out_ep[ep_idx].xfer_len = data_len;
    g_bl_udc.out_ep[ep_idx].actual_xfer_len = 0;
    g_bl_udc.out_ep[ep_idx].ep_active = true;

    data_len = MIN(data_len, g_bl_udc.out_ep[ep_idx].ep_mps);
    g_bl_udc.out_ep[ep_idx].xfer_len = data_len;
    bflb_usb_vdma_start_read(USB_FIFO_CXF, data, data_len);
    return 0;
}

/* F0 deadlock bookkeeping lives in g_f0_guard (bflb_usb_v2_guard.h); the
 * driver only supplies the hardware observations once per G3 pass. */

void USBD_IRQHandler(uint8_t busid)
{
    uint32_t glb_intstatus;
    uint32_t dev_intstatus;
    uint32_t subgroup_intstatus;
    uint32_t regval;
    uint8_t ep_idx;

    glb_intstatus = getreg32(BFLB_USB_BASE + USB_GLB_ISR_OFFSET);

    if (glb_intstatus & USB_DEV_INT) {
        dev_intstatus = getreg32(BFLB_USB_BASE + USB_DEV_IGR_OFFSET);
        if (dev_intstatus & USB_INT_G0) {
            subgroup_intstatus = bflb_usb_get_source_group_intstatus(0);

            if (subgroup_intstatus & USB_CX_SETUP_INT) {
                bflb_usb_vdma_start_read(USB_FIFO_CXF, g_setup_buffer, 8);
                volatile uint32_t vdma_wait = 0;
                while (getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET) & USB_VDMA_START_CXF) {
                    if (++vdma_wait > 100000) {
                        regval = getreg32(BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
                        regval &= ~USB_VDMA_START_CXF;
                        putreg32(regval, BFLB_USB_BASE + USB_VDMA_CXFPS1_OFFSET);
                        bflb_usb_reset_fifo(USB_FIFO_CXF);
                        goto skip_setup;
                    }
                }

                bflb_usb_source_group_int_clear(3, USB_VDMA_CMPLT_CXF);

                usbd_event_ep0_setup_complete_handler(busid, g_setup_buffer);
skip_setup:;
            }
        }
        if (dev_intstatus & USB_INT_G1) {
        }
        if (dev_intstatus & USB_INT_G2) {
            subgroup_intstatus = bflb_usb_get_source_group_intstatus(2);

            if (subgroup_intstatus & USB_SUSP_INT) {
                bflb_usb_source_group_int_clear(2, USB_SUSP_INT);

                bflb_usb_reset_fifo(USB_FIFO_F0);
                bflb_usb_reset_fifo(USB_FIFO_F1);
                bflb_usb_reset_fifo(USB_FIFO_F2);
                bflb_usb_reset_fifo(USB_FIFO_F3);
#if defined(BL618DG) || defined(BL616CL)
                bflb_usb_reset_fifo(USB_FIFO_F4);
                bflb_usb_reset_fifo(USB_FIFO_F5);
                bflb_usb_reset_fifo(USB_FIFO_F6);
                bflb_usb_reset_fifo(USB_FIFO_F7);
#endif
                bflb_usb_reset_fifo(USB_FIFO_CXF);

                usbd_event_suspend_handler(busid);
            }
            if (subgroup_intstatus & USB_RESM_INT) {
                bflb_usb_source_group_int_clear(2, USB_RESM_INT);
                usbd_event_resume_handler(busid);
            }
            if (subgroup_intstatus & USB_TX0BYTE_INT) {
                for (uint8_t i = 1; i < USB_MAX_EP_WITH_EP0; i++) {
                    if (bflb_usb_get_tx_zlp_intstatus() & (1 << (i - 1))) {
                        bflb_usb_clear_tx_zlp_intstatus(i);
                        usbd_event_ep_in_complete_handler(busid, i | 0x80, 0);
                    }
                }
                bflb_usb_source_group_int_clear(2, USB_TX0BYTE_INT);
            }
            if (subgroup_intstatus & USB_RX0BYTE_INT) {
                /* B3: a zero-byte OUT and the VDMA completion of the same
                 * endpoint can be latched in the same interrupt.  G2 runs
                 * first, so without arbitration it would complete (and
                 * re-arm) the transfer that G3 is about to complete as well.
                 * The VDMA completion owns the transaction; the ZLP is
                 * dropped for that endpoint this pass.
                 *
                 * The snapshot below cannot close the window by itself: a
                 * completion that latches *after* it was read, but before the
                 * class callback of this pass runs, is not in it, and the
                 * callback's re-arm would hand that completion to the new
                 * transfer.  Therefore ISO OUT endpoints never take a class
                 * completion from a zero-length packet at all (see below). */
                uint32_t g3_pending = bflb_usb_get_source_group_intstatus(3);

                for (uint8_t i = 1; i < USB_MAX_EP_WITH_EP0; i++) {
                    if (bflb_usb_get_rx_zlp_intstatus() & (1 << (i - 1))) {
                        bflb_usb_clear_rx_zlp_intstatus(i);
                        /* B3: a zero-length packet carries no isochronous
                         * meaning - ISO has no ZLP-terminate handshake, every
                         * packet length is carried by the packet itself, and
                         * an ISO transfer is retired only by its VDMA
                         * completion.  Dispatching here would give one
                         * software transaction a second retirement source,
                         * which is the hardware TOCTOU this closes: the G3
                         * completion read above can miss a bit that latches
                         * during the callback, and that stale completion
                         * would then release the transfer the callback just
                         * re-armed.  Audio OUT is the only ISO OUT endpoint
                         * in every shipped configuration, so this removes
                         * the reachable double-retirement path while leaving
                         * the ownership gate below untouched for the rest. */
                        if (bflb_usb_ep_is_iso(g_bl_udc.out_ep[i].ep_type)) {
                            continue;
                        }
                        /* P0 4.4: a zero-byte OUT is a *completion* for the
                         * transfer that is programmed on this endpoint.  If
                         * it is not ours (idle, or cancelled and waiting for
                         * the witness) it must not reach the class layer: the
                         * callback would re-arm an endpoint the consumer has
                         * not applied yet. */
                        if (!g_bl_udc.out_ep[i].ep_active ||
                            g_bl_udc.out_ep[i].xfer_buf == NULL ||
                            g_bl_udc.out_ep[i].cancel.state == BFLB_EP_CANCEL_PENDING) {
                            continue;
                        }
                        {
                            uint8_t fifo = bflb_usb_get_ep_fifo(i);
                            if (fifo != 0xff && fifo != 0x0f &&
                                (g3_pending & bflb_usb_fifo_cmplt_bit(fifo))) {
                                continue;   /* G3 completes it below */
                            }
                        }
                        /* Residual, non-ISO only: bulk/interrupt OUT (the CDC
                         * data pair, the only non-ISO OUT endpoints we have)
                         * still has the narrow window where a completion
                         * latches after the snapshot and before this
                         * callback, so the G3 pass charges it to the
                         * re-armed transfer.  It cannot coexist with audio:
                         * CMakeLists.txt disables AUDIO in DEBUG_CDC builds.
                         * Closing it in general needs a task-context re-arm,
                         * and the resulting ~4 ms unarmed gap would break the
                         * 1 ms ISO pacing we do ship. */
                        usbd_event_ep_out_complete_handler(busid, i, 0);
                    }
                }

                bflb_usb_source_group_int_clear(2, USB_RX0BYTE_INT);
            }
            if (subgroup_intstatus & USBRST_INT) {
                bflb_usb_source_group_int_clear(2, USBRST_INT);

                /* P0 4.6: the safety-net budget describes the previous bus
                 * session and must not leak into the next one. */
                bflb_f0_guard_reset(&g_f0_guard);

                bflb_usb_reset_fifo(USB_FIFO_F0);
                bflb_usb_reset_fifo(USB_FIFO_F1);
                bflb_usb_reset_fifo(USB_FIFO_F2);
                bflb_usb_reset_fifo(USB_FIFO_F3);
#if defined(BL618DG) || defined(BL616CL)
                bflb_usb_reset_fifo(USB_FIFO_F4);
                bflb_usb_reset_fifo(USB_FIFO_F5);
                bflb_usb_reset_fifo(USB_FIFO_F6);
                bflb_usb_reset_fifo(USB_FIFO_F7);
#endif
                bflb_usb_reset_fifo(USB_FIFO_CXF);

                regval = getreg32(BFLB_USB_BASE + USB_DEV_SMT_OFFSET);
                regval &= ~USB_SOFMT_MASK;
#ifdef CONFIG_USB_HS
                regval |= USB_SOF_TIMER_MASK_AFTER_RESET_HS;
#else
                regval |= USB_SOF_TIMER_MASK_AFTER_RESET_FS;
#endif
                putreg32(regval, BFLB_USB_BASE + USB_DEV_SMT_OFFSET);

                arch_memset(&g_bl_udc, 0, sizeof(g_bl_udc));
                usbd_event_reset_handler(busid);
            }
        }
        if (dev_intstatus & USB_INT_G3) {
            /* P0 4.4: snapshot the group status BEFORE clearing it. Every
             * consumer below (cancel witnesses, completion dispatch, the F0
             * guard) must reason about this pass only. */
            subgroup_intstatus = bflb_usb_get_source_group_intstatus(3);

            /* M4: FIFO0 occupancy (FIBC0, the same 11-bit used-width field as
             * the mic's FIBC1 read in ds4_usb_audio.c) belongs to the same
             * per-pass snapshot as the completion bit. Sampling it here,
             * before any handler below can drain or refill the FIFO, keeps
             * "completed in this pass" and "bytes queued in this pass"
             * describing one instant instead of two. The guard decides
             * whether the two samples disagree in a way that matters; it now
             * requires sustained evidence, so a single skewed pass can only
             * reset its window, never abort. */
            uint16_t f0_bytes_pass = 0;
            if (g_usb_f0_guard_ep != 0 &&
                g_usb_f0_guard_ep < USB_NUM_BIDIR_ENDPOINTS) {
                f0_bytes_pass = (uint16_t)(getreg32(BFLB_USB_BASE +
                                                    USB_DEV_FIBC0_OFFSET) &
                                           USB_BC_F0_MASK);
            }
            bflb_usb_source_group_int_clear(3, subgroup_intstatus);

            /* ---- cancel witnesses (P0 4.4) ------------------------------
             * A cancelled FIFO is only proven silent by a pass in which its
             * completion bit is NOT set. A completion that leaks out of an
             * aborted transfer is dropped here and never dispatched. */
            {
                uint64_t witness_now_us = bflb_mtimer_get_time_us();

                for (uint8_t dir = 0; dir < 2; dir++) {
                    for (uint8_t i = 1; i < USB_NUM_BIDIR_ENDPOINTS; i++) {
                        struct bl_ep_state *st = dir ? &g_bl_udc.in_ep[i] : &g_bl_udc.out_ep[i];
                        uint8_t fifo;

                        if (st->cancel.state != BFLB_EP_CANCEL_PENDING) {
                            continue;
                        }
                        fifo = bflb_usb_get_ep_fifo(i);
                        if (fifo == 0xff || fifo == 0x0f) {
                            continue;
                        }
                        (void)bflb_ep_cancel_witness_step(&st->cancel, &g_f0_guard.cnt,
                                                          (subgroup_intstatus &
                                                           bflb_usb_fifo_cmplt_bit(fifo)) != 0);
                        (void)bflb_ep_cancel_witness_timeout(&st->cancel, &g_f0_guard.cnt,
                                                             witness_now_us);
                    }
                }
            }

            /* ---- F0 deadlock guard (P0 4.6) -----------------------------
             * Runs BEFORE the completion dispatch on purpose: the completion
             * bit in this pass belongs to the transfer that was armed when
             * the pass started. Dispatching first would let the class
             * callback re-arm the FIFO (resetting progress) and then credit
             * this pass's completion to the *new* transfer, which would
             * permanently mask a later deadlock.
             * All policy lives in bflb_usb_v2_guard.h; this block only feeds
             * it the hardware state of this pass and executes the single
             * action it returns. With g_usb_f0_guard_ep == 0 nothing runs. */
            if (g_usb_f0_guard_ep != 0 && g_usb_f0_guard_ep < USB_NUM_BIDIR_ENDPOINTS) {
                bflb_f0_input_t f0_in;
                bflb_f0_action_t f0_act;

                f0_in.guard_ep = g_usb_f0_guard_ep;
                f0_in.fifo_ep = bflb_usb_get_fifo_ep(USB_FIFO_F0);
                f0_in.armed = g_bl_udc.out_ep[g_usb_f0_guard_ep].ep_active;
                f0_in.start = bflb_usb_fifo_start_bit(USB_FIFO_F0);
                f0_in.cmplt = (subgroup_intstatus & USB_VDMA_CMPLT_F0) != 0;
                /* M1/M4: occupancy from the same per-pass snapshot as cmplt.
                 * The guard needs it to tell a paused host (FIFO empty, VDMA
                 * nothing to drain -> idle) from a real stall (bytes queued
                 * and not moving). */
                f0_in.fifo_bytes = f0_bytes_pass;
                f0_in.now_us = bflb_mtimer_get_time_us();

                f0_act = bflb_f0_guard_step(&g_f0_guard, &f0_in);

                switch (f0_act) {
                    case BFLB_F0_ACT_ABORT:
                        /* Isolation: stop the FIFO and wait in RECOVERING for
                         * one clean pass. Never synthesise on the same pass. */
                        bflb_usb_ep_abort(USB_FIFO_F0);
                        break;

                    case BFLB_F0_ACT_SYNTH:
                        /* Exactly one synthetic zero-length completion so the
                         * class layer rebuilds its single in-flight transfer. */
                        usbd_event_ep_out_complete_handler(busid,
                                                           g_usb_f0_guard_ep & 0x7f, 0);
                        break;

                    case BFLB_F0_ACT_FAULT:
                        /* Terminal and visible: stop accepting transfers and
                         * notify the application (weak handler, called once). */
                        bflb_usb_ep_abort(USB_FIFO_F0);
                        g_bl_udc.out_ep[g_usb_f0_guard_ep].ep_active = false;
                        usbd_event_f0_fault_handler(busid, g_usb_f0_guard_ep & 0x7f);
                        break;

                    case BFLB_F0_ACT_NONE:
                    default:
                        break;
                }
            }

            if (subgroup_intstatus & USB_VDMA_CMPLT_CXF) {
                if (g_bl_udc.in_ep[0].ep_active) {
                    g_bl_udc.in_ep[0].ep_active = false;
                    g_bl_udc.in_ep[0].actual_xfer_len = g_bl_udc.in_ep[0].xfer_len - bflb_usb_vdma_get_remain_size(USB_FIFO_CXF);
                    usbd_event_ep_in_complete_handler(busid, 0x80, g_bl_udc.in_ep[0].actual_xfer_len);
                } else {
                    g_bl_udc.out_ep[0].ep_active = false;
                    g_bl_udc.out_ep[0].actual_xfer_len = g_bl_udc.out_ep[0].xfer_len - bflb_usb_vdma_get_remain_size(USB_FIFO_CXF);
                    usbd_event_ep_out_complete_handler(busid, 0x00, g_bl_udc.out_ep[0].actual_xfer_len);
                }
            }

            for (uint8_t i = 0; i < USB_MAX_EP_EXCLUDE_EP0; i++) {
                if (subgroup_intstatus & (1 << (i + 1))) {
                    ep_idx = bflb_usb_get_fifo_ep(i);
                    if (ep_idx >= USB_NUM_BIDIR_ENDPOINTS) {
                        /* Stale bit for an unmapped FIFO: nothing to report. */
                        continue;
                    }
                    /* P0 4.6: while the guard is recovering from an abort this
                     * FIFO's completions are noise - drop them, the guard
                     * counts them. */
                    if (g_usb_f0_guard_ep != 0 && ep_idx == g_usb_f0_guard_ep &&
                        g_f0_guard.state == BFLB_F0_RECOVERING) {
                        continue;
                    }
                    /* P0 4.4: only an armed transfer with a live buffer, and
                     * no cancel awaiting its witness, may be dispatched. */
                    if (g_bl_udc.in_ep[ep_idx].ep_active &&
                        g_bl_udc.in_ep[ep_idx].xfer_buf != NULL &&
                        g_bl_udc.in_ep[ep_idx].cancel.state != BFLB_EP_CANCEL_PENDING) {
                        g_bl_udc.in_ep[ep_idx].ep_active = 0;
                        g_bl_udc.in_ep[ep_idx].actual_xfer_len = g_bl_udc.in_ep[ep_idx].xfer_len - bflb_usb_vdma_get_remain_size(i);
                        usbd_event_ep_in_complete_handler(busid, ep_idx | 0x80, g_bl_udc.in_ep[ep_idx].actual_xfer_len);
                    } else if (g_bl_udc.out_ep[ep_idx].ep_active &&
                               g_bl_udc.out_ep[ep_idx].xfer_buf != NULL &&
                               g_bl_udc.out_ep[ep_idx].cancel.state != BFLB_EP_CANCEL_PENDING) {
                        g_bl_udc.out_ep[ep_idx].ep_active = 0;
                        g_bl_udc.out_ep[ep_idx].actual_xfer_len = g_bl_udc.out_ep[ep_idx].xfer_len - bflb_usb_vdma_get_remain_size(i);
                        usbd_event_ep_out_complete_handler(busid, ep_idx & 0x7f, g_bl_udc.out_ep[ep_idx].actual_xfer_len);
                    }
                }
            }


            /* Publish the ISR-only counters once per pass (witness counters
             * are updated above even when the guard is disabled). */
            bflb_usb_f0_publish_counters();
        }
        if (dev_intstatus & USB_INT_G4) {
        }
    }
}

#ifdef CONFIG_USBDEV_TEST_MODE
void usbd_execute_test_mode(uint8_t busid, uint8_t test_mode)
{
    uint32_t regval;

    switch (test_mode) {
        case 1: // Test_J
        {
            regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            regval |= USB_TST_JSTA;
            putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            USB_LOG_INFO("test_mode done: Test_J\r\n");
        } break;
        case 2: // Test_K
        {
            regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            regval |= USB_TST_KSTA;
            putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            USB_LOG_INFO("test_mode done: Test_K\r\n");
        } break;
        case 3: // TEST_SE0_NAK
        {
            regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            regval |= USB_TST_SE0NAK;
            putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            USB_LOG_INFO("test_mode done: TEST_SE0_NAK\r\n");
        } break;
        case 4: // Test_Packet
        {
            regval = getreg32(BFLB_USB_BASE + USB_PHY_TST_OFFSET);
            regval |= USB_TST_PKT;
            putreg32(regval, BFLB_USB_BASE + USB_PHY_TST_OFFSET);

            /* fill test packet */
            uint8_t *pp = g_test_packet_buffer;
            /* JKJKJKJK * 9 */
            for (uint8_t i = 0; i < 9; i++) {
                *pp++ = 0x00;
            }
            /* JJKKJJKK * 8 */
            for (uint8_t i = 0; i < 8; i++) {
                *pp++ = 0xAA;
            }
            /* JJJJKKKK * 8 */
            for (uint8_t i = 0; i < 8; i++) {
                *pp++ = 0xEE;
            }
            /* JJJJJJJKKKKKKK * 8, JJJJJJJK * 8 */
            *pp++ = 0xFE;
            for (uint8_t i = 0; i < 11; i++) {
                *pp++ = 0xFF;
            }
            /* {JKKKKKKK * 10}, JK*/
            *pp++ = 0x7F;
            *pp++ = 0xBF;
            *pp++ = 0xDF;
            *pp++ = 0xEF;
            *pp++ = 0xF7;
            *pp++ = 0xFB;
            *pp++ = 0xFD;
            *pp++ = 0xFC;
            *pp++ = 0x7E;
            *pp++ = 0xBF;
            *pp++ = 0xDF;
            *pp++ = 0xEF;
            *pp++ = 0xF7;
            *pp++ = 0xFB;
            *pp++ = 0xFD;
            *pp++ = 0x7E;

            /* write the test packet to CX FIFO */
            bflb_usb_vdma_start_write(USB_FIFO_CXF, g_test_packet_buffer, 53);

            /* wait for transfer complete */
            for (int i = 0; i < 10000; i++) {
                regval = getreg32(BFLB_USB_BASE + USB_DEV_ISG3_OFFSET);
                if (regval & USB_VDMA_CMPLT_CXF) {
                    putreg32(USB_VDMA_CMPLT_CXF, BFLB_USB_BASE + USB_DEV_ISG3_OFFSET);
                    break;
                }
                arch_delay_us(2);
            }
            if (!(regval & USB_VDMA_CMPLT_CXF)) {
                USB_LOG_ERR("test_mode Test_Packet timeout\r\n");
            }

            /* set PKDONE */
            regval = getreg32(BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);
            regval |= USB_TST_PKDONE;
            putreg32(regval, BFLB_USB_BASE + USB_DEV_CXCFE_OFFSET);

            USB_LOG_INFO("test_mode done: Test_Packet\r\n");
        } break;
        case 5: // Test_Force_Enable
            break;

        default:
            break;
    }
}
#endif
