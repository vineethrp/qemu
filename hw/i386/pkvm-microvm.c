/*
 * x86 pKVM microvm machine
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"

#include "hw/i386/microvm.h"
#include "target/i386/cpu.h"
#include "target/i386/pkvm.h"

#define TYPE_PKVM_MICROVM_MACHINE MACHINE_TYPE_NAME("pkvm-microvm")

typedef struct PkvmMicrovmMachineState PkvmMicrovmMachineState;
typedef struct PkvmMicrovmMachineClass PkvmMicrovmMachineClass;

OBJECT_DECLARE_TYPE(PkvmMicrovmMachineState, PkvmMicrovmMachineClass,
                    PKVM_MICROVM_MACHINE)

struct PkvmMicrovmMachineState {
    MicrovmMachineState parent;
};

struct PkvmMicrovmMachineClass {
    MicrovmMachineClass parent;
    void (*parent_init)(MachineState *machine);
};

static bool pkvm_microvm_is_enabled(MachineState *machine)
{
    return machine->cgs &&
           object_dynamic_cast(OBJECT(machine->cgs), TYPE_PKVM_GUEST);
}

static void pkvm_microvm_machine_state_init(MachineState *machine)
{
    PkvmMicrovmMachineClass *pmc = PKVM_MICROVM_MACHINE_GET_CLASS(machine);

    if (!pkvm_microvm_is_enabled(machine)) {
        error_report("pkvm-microvm requires confidential-guest-support=pkvm-guest");
        exit(1);
    }
    if (!machine->kernel_filename) {
        error_report("pkvm-microvm requires -kernel");
        exit(1);
    }

    pmc->parent_init(machine);
}

static void pkvm_microvm_machine_initfn(Object *obj)
{
    MicrovmMachineState *mms = MICROVM_MACHINE(obj);
    X86MachineState *x86ms = X86_MACHINE(obj);

    mms->rtc = ON_OFF_AUTO_OFF;
    mms->pcie = ON_OFF_AUTO_OFF;
    mms->ioapic2 = ON_OFF_AUTO_OFF;
    mms->isa_serial = true;
    mms->option_roms = false;
    mms->auto_kernel_cmdline = true;

    x86ms->acpi = ON_OFF_AUTO_OFF;
}

static void pkvm_microvm_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    PkvmMicrovmMachineClass *pmc = PKVM_MICROVM_MACHINE_CLASS(oc);

    pmc->parent_init = mc->init;
    mc->init = pkvm_microvm_machine_state_init;
    mc->family = "pkvm_microvm_i386";
    mc->desc = "microvm for x86 pKVM firmwareless boot";
}

static const TypeInfo pkvm_microvm_machine_info = {
    .name          = TYPE_PKVM_MICROVM_MACHINE,
    .parent        = TYPE_MICROVM_MACHINE,
    .instance_size = sizeof(PkvmMicrovmMachineState),
    .instance_init = pkvm_microvm_machine_initfn,
    .class_size    = sizeof(PkvmMicrovmMachineClass),
    .class_init    = pkvm_microvm_class_init,
};

static void pkvm_microvm_machine_register_types(void)
{
    type_register_static(&pkvm_microvm_machine_info);
}

type_init(pkvm_microvm_machine_register_types);
