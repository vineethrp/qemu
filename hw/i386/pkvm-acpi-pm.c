/*
 * Minimal ACPI PM device for x86 pKVM microvm direct boot.
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/acpi_dev_interface.h"
#include "hw/boards.h"
#include "hw/i386/pkvm-acpi-pm.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"

OBJECT_DECLARE_SIMPLE_TYPE(PkvmAcpiPmState, PKVM_ACPI_PM)

struct PkvmAcpiPmState {
    SysBusDevice parent_obj;

    MemoryRegion io;
    MemoryRegion io_gpe;
    ACPIREGS ar;
    qemu_irq irq;
    uint32_t io_base;
    uint16_t sci_int;
    uint32_t gpe0_blk;
    uint32_t gpe0_blk_len;
};

static void pkvm_acpi_pm_update_sci_fn(ACPIREGS *regs)
{
    PkvmAcpiPmState *s = container_of(regs, PkvmAcpiPmState, ar);
    int sci_level;
    int pm1a_sts;

    pm1a_sts = acpi_pm1_evt_get_sts(&s->ar);
    sci_level = ((pm1a_sts &
                  s->ar.pm1.evt.en & ACPI_BITMASK_PM1_COMMON_ENABLED) != 0) ||
                ((s->ar.gpe.sts[0] & s->ar.gpe.en[0]) != 0);
    qemu_set_irq(s->irq, sci_level);
}

static uint64_t pkvm_acpi_pm_gpe_readb(void *opaque, hwaddr addr, unsigned width)
{
    PkvmAcpiPmState *s = opaque;

    return acpi_gpe_ioport_readb(&s->ar, addr);
}

static void pkvm_acpi_pm_gpe_writeb(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned width)
{
    PkvmAcpiPmState *s = opaque;

    acpi_gpe_ioport_writeb(&s->ar, addr, val);
    acpi_update_sci(&s->ar, s->irq);
}

static const MemoryRegionOps pkvm_acpi_pm_gpe_ops = {
    .read = pkvm_acpi_pm_gpe_readb,
    .write = pkvm_acpi_pm_gpe_writeb,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void pkvm_acpi_pm_reset(DeviceState *dev)
{
    PkvmAcpiPmState *s = PKVM_ACPI_PM(dev);

    acpi_pm_tmr_reset(&s->ar);
    acpi_pm1_evt_reset(&s->ar);
    acpi_pm1_cnt_reset(&s->ar);
    acpi_gpe_reset(&s->ar);
}

static void pkvm_acpi_pm_send_event(AcpiDeviceIf *adev, AcpiEventStatusBits ev)
{
    PkvmAcpiPmState *s = PKVM_ACPI_PM(adev);

    if (ev == ACPI_POWER_DOWN_STATUS) {
        acpi_pm1_evt_power_down(&s->ar);
        acpi_update_sci(&s->ar, s->irq);
        return;
    }

    acpi_send_gpe_event(&s->ar, s->irq, ev);
}

static void pkvm_acpi_pm_realize(DeviceState *dev, Error **errp)
{
    PkvmAcpiPmState *s = PKVM_ACPI_PM(dev);

    memory_region_init(&s->io, OBJECT(dev), "pkvm-acpi-pm", PKVM_ACPI_PM_IO_LEN);
    memory_region_add_subregion(get_system_io(), s->io_base, &s->io);

    acpi_pm_tmr_init(&s->ar, pkvm_acpi_pm_update_sci_fn, &s->io);
    acpi_pm1_evt_init(&s->ar, pkvm_acpi_pm_update_sci_fn, &s->io);
    acpi_pm1_cnt_init(&s->ar, &s->io, false, false, 2, false);
    acpi_gpe_init(&s->ar, s->gpe0_blk_len);

    memory_region_init_io(&s->io_gpe, OBJECT(dev), &pkvm_acpi_pm_gpe_ops, s,
                          "pkvm-acpi-gpe0", s->gpe0_blk_len);
    memory_region_add_subregion(&s->io, PKVM_ACPI_GPE0_BLK_OFFSET, &s->io_gpe);
}

static void pkvm_acpi_pm_init(Object *obj)
{
    PkvmAcpiPmState *s = PKVM_ACPI_PM(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->io_base = PKVM_ACPI_PM_IO_BASE;
    s->sci_int = PKVM_ACPI_SCI_IRQ;
    s->gpe0_blk = PKVM_ACPI_PM_IO_BASE + PKVM_ACPI_GPE0_BLK_OFFSET;
    s->gpe0_blk_len = PKVM_ACPI_GPE0_BLK_LEN;

    sysbus_init_irq(sbd, &s->irq);

    object_property_add_uint32_ptr(obj, ACPI_PM_PROP_PM_IO_BASE, &s->io_base,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint16_ptr(obj, ACPI_PM_PROP_SCI_INT, &s->sci_int,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, ACPI_PM_PROP_GPE0_BLK, &s->gpe0_blk,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, ACPI_PM_PROP_GPE0_BLK_LEN,
                                   &s->gpe0_blk_len, OBJ_PROP_FLAG_READ);
}

static void pkvm_acpi_pm_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AcpiDeviceIfClass *adevc = ACPI_DEVICE_IF_CLASS(klass);

    dc->realize = pkvm_acpi_pm_realize;
    dc->hotpluggable = false;
    dc->user_creatable = false;
    device_class_set_legacy_reset(dc, pkvm_acpi_pm_reset);

    adevc->send_event = pkvm_acpi_pm_send_event;
}

static const TypeInfo pkvm_acpi_pm_info = {
    .name          = TYPE_PKVM_ACPI_PM,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(PkvmAcpiPmState),
    .instance_init = pkvm_acpi_pm_init,
    .class_init    = pkvm_acpi_pm_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_ACPI_DEVICE_IF },
        { TYPE_HOTPLUG_HANDLER },
        { }
    }
};

void pkvm_microvm_acpi_pm_init(X86MachineState *x86ms)
{
    DeviceState *dev;

    dev = qdev_new(TYPE_PKVM_ACPI_PM);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, x86ms->gsi[PKVM_ACPI_SCI_IRQ]);
    x86ms->acpi_dev = HOTPLUG_HANDLER(dev);
}

static void pkvm_acpi_pm_register_types(void)
{
    type_register_static(&pkvm_acpi_pm_info);
}

type_init(pkvm_acpi_pm_register_types)
