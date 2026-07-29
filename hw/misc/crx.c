/*
 * QEMU Cryptex (CRX) PCI XOR accelerator device
 *
 * A fictional hardware accelerator that performs XOR-based "encryption".
 * Useful as a driver-writing exercise: covers PCI BAR registration,
 * RAM-backed MMIO, DMA, MSI interrupts, and write-1-to-clear IRQ status.
 *
 * Spec:
 *   VID 0x1DEA, DID 0xC77E
 *   BAR0 (4KB)  - MMIO control registers
 *   BAR1 (64KB) - RAM-backed device buffer (mmap-able)
 *
 * Copyright (c) 2024 QEMU contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "system/memory.h"

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

#define TYPE_PCI_CRX_DEVICE "crx"
typedef struct CrxState CrxState;
DECLARE_INSTANCE_CHECKER(CrxState, CRX, TYPE_PCI_CRX_DEVICE)

#define CRX_BAR0_SIZE    (4 * KiB)
#define CRX_BAR1_SIZE    (64 * KiB)

/* BAR0 register offsets */
#define CRX_REG_ID          0x000   /* R   - device identity */
#define CRX_REG_STATUS      0x004   /* R   - status bits */
#define CRX_REG_CONTROL     0x008   /* W   - control/command */
#define CRX_REG_KEY         0x00C   /* R/W - 32-bit XOR key */
#define CRX_REG_DMA_SRC     0x010   /* R/W - DMA source addr [31:0] */
#define CRX_REG_DMA_SRC_HI  0x014   /* R/W - DMA source addr [63:32] */
#define CRX_REG_DMA_DST     0x018   /* R/W - DMA dest addr [31:0] */
#define CRX_REG_DMA_DST_HI  0x01C   /* R/W - DMA dest addr [63:32] */
#define CRX_REG_DMA_LEN     0x020   /* R/W - transfer length in bytes */
#define CRX_REG_IRQ_STATUS  0x024   /* R/W1C - interrupt status */
#define CRX_REG_IRQ_MASK    0x028   /* R/W - interrupt enable mask */
#define CRX_REG_PERF_COUNT  0x02C   /* R   - completed operation count */
#define CRX_REG_RESET       0x030   /* W   - soft reset (magic 0xDEADBEEF) */
#define CRX_REG_HW_ERR_COUNT 0x034  /* R   - hardware error counter */
#define CRX_REG_TEMP        0x038   /* R   - die temperature in Fahrenheit */

#define CRX_ID_VALUE        0xC0DEC4FEU
#define CRX_RESET_MAGIC     0xDEADBEEFU

/* CRX_STATUS bits */
#define STATUS_IDLE         (1U << 0)
#define STATUS_BUSY         (1U << 1)
#define STATUS_DMA_DONE     (1U << 2)
#define STATUS_ERROR        (1U << 3)

/* CRX_CONTROL bits */
#define CTRL_START          (1U << 0)
#define CTRL_IRQ_ENABLE     (1U << 1)
#define CTRL_DMA_IN         (1U << 2)  /* DMA host→BAR1 before encrypt */
#define CTRL_DMA_OUT        (1U << 3)  /* DMA BAR1→host after encrypt */
#define CTRL_ENCRYPT        (1U << 4)  /* XOR BAR1 with CRX_KEY */

/* CRX_IRQ_STATUS / CRX_IRQ_MASK bits */
#define IRQ_DMA_DONE        (1U << 0)
#define IRQ_ERROR           (1U << 1)
#define IRQ_TEMP_HIGH       (1U << 2)  /* temperature exceeded threshold */

/* -------------------------------------------------------------------------
 * Device state
 * ---------------------------------------------------------------------- */

struct CrxState {
    PCIDevice pdev;
    MemoryRegion bar0;
    MemoryRegion bar1;   /* RAM-backed, guest-mmap-able */

    /* Shadow registers */
    uint32_t status;
    uint32_t control;    /* last written CONTROL value */
    uint32_t key;
    uint64_t dma_src;
    uint64_t dma_dst;
    uint32_t dma_len;
    uint32_t irq_status;
    uint32_t irq_mask;
    uint32_t perf_count;

    uint32_t hw_err_count;  /* incremented whenever STATUS_ERROR is set */
    uint32_t temperature;   /* simulated die temperature in Fahrenheit */

    GRand    *rng;          /* per-device PRNG for thermal + fault simulation */

    QEMUTimer dma_timer;
    QEMUTimer temp_timer;
    QEMUTimer fault_timer;
};

/* -------------------------------------------------------------------------
 * Interrupt helpers
 * ---------------------------------------------------------------------- */

/*
 * crx_update_irq: recompute the INTx line level.
 * Called when irq_status or irq_mask changes (W1C clear, mask write).
 * For MSI there is nothing to do — MSI is edge-triggered and was already
 * fired by crx_raise_irq at the moment the condition was set.
 */
static void crx_update_irq(CrxState *s)
{
    if (!msi_enabled(&s->pdev)) {
        pci_set_irq(&s->pdev, (s->irq_status & s->irq_mask) ? 1 : 0);
    }
}

/*
 * crx_raise_irq: record a new interrupt condition and notify the guest.
 * CTRL_IRQ_ENABLE is the sole per-operation gate; the mask register is not
 * checked here so that a driver using only CTRL_IRQ_ENABLE always gets its
 * interrupt regardless of whether it has written CRX_IRQ_MASK.
 */
static void crx_raise_irq(CrxState *s, uint32_t bits)
{
    s->irq_status |= bits;
    if (msi_enabled(&s->pdev)) {
        msi_notify(&s->pdev, 0);
    } else {
        pci_set_irq(&s->pdev, 1);
    }
}

/* -------------------------------------------------------------------------
 * Thermal simulation (Ornstein-Uhlenbeck random walk)
 *
 * Temperature is modelled as a mean-reverting random walk centred on 72 F.
 * Each tick adds uniform noise in [-8, +8] F plus a gentle pull back to the
 * equilibrium, so the device idles around 65-85 F but occasionally spikes
 * above the 100 F threshold.
 *
 * IRQ_TEMP_HIGH fires on the rising edge only (≤100 → >100) so the driver
 * sees exactly one interrupt per thermal event, not a continuous storm.
 *
 * Fault injection (crx_fault_timer)
 *
 * A separate timer fires at a random interval [15 s, 45 s].  Each time it
 * fires it has a 15 % chance of injecting a spontaneous hardware error when
 * the device is idle.  This exercises the driver's error-recovery path
 * without any workload running.
 * ---------------------------------------------------------------------- */

#define CRX_TEMP_EQUILIBRIUM    72U    /* F — long-run mean temperature  */
#define CRX_TEMP_THRESHOLD     100U    /* F — high-temp interrupt trigger */
#define CRX_TEMP_NOISE_RANGE     8     /* ± F of random noise per tick    */
#define CRX_TEMP_MIN            45U
#define CRX_TEMP_MAX           115U
#define CRX_TEMP_INTERVAL_MS  3000     /* ms between temperature samples  */

#define CRX_FAULT_INTERVAL_MIN_MS  15000   /* ms — min between fault checks */
#define CRX_FAULT_INTERVAL_MAX_MS  45000   /* ms — max between fault checks */
#define CRX_FAULT_PROBABILITY         15   /* % chance of error per check   */

static void crx_temp_timer(void *opaque)
{
    CrxState *s = opaque;
    uint32_t prev = s->temperature;
    int32_t  temp = (int32_t)s->temperature;

    /* Ornstein-Uhlenbeck step: noise + pull toward equilibrium */
    int32_t noise = g_rand_int_range(s->rng,
                                     -CRX_TEMP_NOISE_RANGE,
                                     CRX_TEMP_NOISE_RANGE + 1);
    int32_t pull  = ((int32_t)CRX_TEMP_EQUILIBRIUM - temp) / 5;
    temp = temp + noise + pull;

    if (temp < (int32_t)CRX_TEMP_MIN) temp = (int32_t)CRX_TEMP_MIN;
    if (temp > (int32_t)CRX_TEMP_MAX) temp = (int32_t)CRX_TEMP_MAX;
    s->temperature = (uint32_t)temp;

    /* Rising-edge interrupt: one IRQ per thermal event, not per hot tick */
    if (s->temperature > CRX_TEMP_THRESHOLD && prev <= CRX_TEMP_THRESHOLD) {
        crx_raise_irq(s, IRQ_TEMP_HIGH);
    }

    timer_mod(&s->temp_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + CRX_TEMP_INTERVAL_MS);
}

static void crx_fault_timer(void *opaque)
{
    CrxState *s = opaque;

    /*
     * Only inject when idle — don't corrupt an in-progress DMA operation.
     * Roll the dice: CRX_FAULT_PROBABILITY % chance of a hardware error.
     */
    if ((s->status & STATUS_IDLE) &&
        g_rand_int_range(s->rng, 0, 100) < CRX_FAULT_PROBABILITY) {
        s->status = STATUS_IDLE | STATUS_ERROR;
        s->hw_err_count++;
        crx_raise_irq(s, IRQ_ERROR);
    }

    /* Reschedule at a new random interval */
    int64_t next = g_rand_int_range(s->rng,
                                    CRX_FAULT_INTERVAL_MIN_MS,
                                    CRX_FAULT_INTERVAL_MAX_MS);
    timer_mod(&s->fault_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + next);
}

/* -------------------------------------------------------------------------
 * Soft reset
 * ---------------------------------------------------------------------- */

static void crx_soft_reset(CrxState *s)
{
    uint8_t *buf = memory_region_get_ram_ptr(&s->bar1);

    timer_del(&s->dma_timer);

    s->status       = STATUS_IDLE;
    s->control      = 0;
    s->key          = 0;
    s->dma_src      = 0;
    s->dma_dst      = 0;
    s->dma_len      = 0;
    s->irq_status   = 0;
    s->irq_mask     = 0;
    s->perf_count   = 0;
    s->hw_err_count = 0;
    /* temperature and temp_timer are physical — not reset by software */

    memset(buf, 0, CRX_BAR1_SIZE);
    memory_region_set_dirty(&s->bar1, 0, CRX_BAR1_SIZE);

    /* Drop any pending INTx line */
    if (!msi_enabled(&s->pdev)) {
        pci_set_irq(&s->pdev, 0);
    }
}

/* -------------------------------------------------------------------------
 * DMA + XOR engine (fires 100 ms after CTRL_START, simulating HW latency)
 * ---------------------------------------------------------------------- */

static void crx_dma_timer(void *opaque)
{
    CrxState *s = opaque;
    uint8_t *buf = memory_region_get_ram_ptr(&s->bar1);
    uint32_t len = s->dma_len;
    bool dma_in  = !!(s->control & CTRL_DMA_IN);
    bool dma_out = !!(s->control & CTRL_DMA_OUT);
    bool encrypt = !!(s->control & CTRL_ENCRYPT);
    bool irq_en  = !!(s->control & CTRL_IRQ_ENABLE);
    uint32_t key = s->key;

    if (len > CRX_BAR1_SIZE) {
        s->status = STATUS_IDLE | STATUS_ERROR;
        s->hw_err_count++;
        if (irq_en) {
            crx_raise_irq(s, IRQ_ERROR);
        }
        return;
    }

    if (dma_in && len > 0) {
        pci_dma_read(&s->pdev, s->dma_src, buf, len);
    }

    if (encrypt) {
        /* XOR in-place; key bytes repeat LSB-first across the buffer */
        for (uint32_t i = 0; i < len; i++) {
            buf[i] ^= (key >> ((i % 4) * 8)) & 0xff;
        }
    }

    if (dma_out && len > 0) {
        pci_dma_write(&s->pdev, s->dma_dst, buf, len);
    }

    if (dma_in || encrypt) {
        memory_region_set_dirty(&s->bar1, 0, len ? len : CRX_BAR1_SIZE);
    }

    s->status = STATUS_IDLE | STATUS_DMA_DONE;
    s->perf_count++;

    if (irq_en) {
        crx_raise_irq(s, IRQ_DMA_DONE);
    }
}

/* -------------------------------------------------------------------------
 * BAR0 MMIO
 * ---------------------------------------------------------------------- */

static uint64_t crx_bar0_read(void *opaque, hwaddr addr, unsigned size)
{
    CrxState *s = opaque;

    switch (addr) {
    case CRX_REG_ID:
        return CRX_ID_VALUE;
    case CRX_REG_STATUS:
        return s->status;
    case CRX_REG_KEY:
        return s->key;
    case CRX_REG_DMA_SRC:
        return (uint32_t)s->dma_src;
    case CRX_REG_DMA_SRC_HI:
        return (uint32_t)(s->dma_src >> 32);
    case CRX_REG_DMA_DST:
        return (uint32_t)s->dma_dst;
    case CRX_REG_DMA_DST_HI:
        return (uint32_t)(s->dma_dst >> 32);
    case CRX_REG_DMA_LEN:
        return s->dma_len;
    case CRX_REG_IRQ_STATUS:
        return s->irq_status;
    case CRX_REG_IRQ_MASK:
        return s->irq_mask;
    case CRX_REG_PERF_COUNT:
        return s->perf_count;
    case CRX_REG_HW_ERR_COUNT:
        return s->hw_err_count;
    case CRX_REG_TEMP:
        return s->temperature;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "crx: read from unknown register 0x%"HWADDR_PRIx"\n",
                      addr);
        return 0;
    }
}

static void crx_bar0_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    CrxState *s = opaque;
    uint32_t v32 = (uint32_t)val;

    switch (addr) {
    case CRX_REG_CONTROL:
        if (!(v32 & CTRL_START)) {
            break;
        }
        if (s->status & STATUS_BUSY) {
            break; /* spec: CTRL_START while BUSY is ignored */
        }
        if (s->dma_len > CRX_BAR1_SIZE) {
            s->status = STATUS_IDLE | STATUS_ERROR;
            s->hw_err_count++;
            if (v32 & CTRL_IRQ_ENABLE) {
                crx_raise_irq(s, IRQ_ERROR);
            }
            break;
        }
        s->control = v32;
        s->status  = STATUS_BUSY;
        timer_mod(&s->dma_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 100);
        break;

    case CRX_REG_KEY:
        s->key = v32;
        break;

    case CRX_REG_DMA_SRC:
        s->dma_src = (s->dma_src & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case CRX_REG_DMA_SRC_HI:
        s->dma_src = (s->dma_src & 0x00000000FFFFFFFFULL) |
                     ((uint64_t)v32 << 32);
        break;

    case CRX_REG_DMA_DST:
        s->dma_dst = (s->dma_dst & 0xFFFFFFFF00000000ULL) | v32;
        break;
    case CRX_REG_DMA_DST_HI:
        s->dma_dst = (s->dma_dst & 0x00000000FFFFFFFFULL) |
                     ((uint64_t)v32 << 32);
        break;

    case CRX_REG_DMA_LEN:
        s->dma_len = v32;
        break;

    case CRX_REG_IRQ_STATUS:
        /* W1C: clear bits where the guest writes 1 */
        s->irq_status &= ~v32;
        crx_update_irq(s);
        break;

    case CRX_REG_IRQ_MASK:
        s->irq_mask = v32;
        crx_update_irq(s);
        break;

    case CRX_REG_RESET:
        if (v32 == CRX_RESET_MAGIC) {
            crx_soft_reset(s);
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "crx: write to unknown register 0x%"HWADDR_PRIx
                      " val=0x%"PRIx32"\n", addr, v32);
        break;
    }
}

static const MemoryRegionOps crx_bar0_ops = {
    .read  = crx_bar0_read,
    .write = crx_bar0_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* -------------------------------------------------------------------------
 * PCI realize / unrealize
 * ---------------------------------------------------------------------- */

static void pci_crx_realize(PCIDevice *pdev, Error **errp)
{
    CrxState *s = CRX(pdev);

    pci_config_set_interrupt_pin(pdev->config, 1);

    if (msi_init(pdev, 0, 1, true, false, errp)) {
        return;
    }

    s->rng = g_rand_new();

    timer_init_ms(&s->dma_timer,   QEMU_CLOCK_VIRTUAL, crx_dma_timer,   s);
    timer_init_ms(&s->temp_timer,  QEMU_CLOCK_VIRTUAL, crx_temp_timer,  s);
    timer_init_ms(&s->fault_timer, QEMU_CLOCK_VIRTUAL, crx_fault_timer, s);

    memory_region_init_io(&s->bar0, OBJECT(s), &crx_bar0_ops, s,
                          "crx-mmio", CRX_BAR0_SIZE);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    if (!memory_region_init_ram(&s->bar1, OBJECT(s), "crx-devmem",
                                CRX_BAR1_SIZE, errp)) {
        return;
    }
    pci_register_bar(pdev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar1);

    s->status      = STATUS_IDLE;
    s->temperature = CRX_TEMP_EQUILIBRIUM;

    timer_mod(&s->temp_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + CRX_TEMP_INTERVAL_MS);
    /* Stagger the fault timer so it doesn't fire at the same time as temp */
    timer_mod(&s->fault_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + CRX_FAULT_INTERVAL_MIN_MS);
}

static void pci_crx_uninit(PCIDevice *pdev)
{
    CrxState *s = CRX(pdev);

    timer_del(&s->dma_timer);
    timer_del(&s->temp_timer);
    timer_del(&s->fault_timer);
    g_rand_free(s->rng);
    msi_uninit(pdev);
}

/* -------------------------------------------------------------------------
 * Type registration
 * ---------------------------------------------------------------------- */

static void crx_class_init(ObjectClass *class, const void *data)
{
    DeviceClass *dc   = DEVICE_CLASS(class);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(class);

    k->realize   = pci_crx_realize;
    k->exit      = pci_crx_uninit;
    k->vendor_id = 0x1DEA;
    k->device_id = 0xC77E;
    k->revision  = 0x01;
    k->class_id  = PCI_CLASS_OTHERS;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Cryptex XOR accelerator";
}

static const TypeInfo crx_types[] = {
    {
        .name          = TYPE_PCI_CRX_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(CrxState),
        .class_init    = crx_class_init,
        .interfaces    = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(crx_types)
