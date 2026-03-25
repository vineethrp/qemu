/*
 * QEMU pKVM (Protected KVM) x86 guest support
 *
 * Implements support for running protected VMs using pKVM (Protected KVM)
 * on x86. Memory isolation is enforced by the pKVM hypervisor running
 * below the host kernel; the host cannot access guest memory directly.
 *
 * Copyright (c) 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"

#include <linux/kvm.h>
#include <sys/ioctl.h>

#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/boards.h"
#include "system/kvm.h"
#include "kvm/kvm_i386.h"
#include "pkvm.h"
#include "confidential-guest.h"
#include "migration/blocker.h"
#include "cpu.h"

OBJECT_DECLARE_SIMPLE_TYPE(PkvmGuestState, PKVM_GUEST)

/*
 * Guest physical address layout for pKVM:
 *
 *   [swiotlb_gpa, swiotlb_gpa + swiotlb_size)   shared DMA bounce buffer
 *   [PKVM_FW_GPA, PKVM_FW_GPA + PKVM_FW_MAX_SIZE)  pVM firmware region
 *   [0x80000000, ...)                             normal guest RAM (2GB+)
 *
 * The firmware region sits just below 2GB, matching crosvm's layout.
 */
#define PKVM_FW_GPA          0x7FC00000ULL   /* 2GB - 4MB */
#define PKVM_FW_MAX_SIZE     (4 * MiB)
#define PKVM_SWIOTLB_DEFAULT_SIZE (64 * MiB)

/* Boot GDT/IDT offsets for flat32 protected mode entry */
#define PKVM_BOOT_GDT_OFFSET 0x500
#define PKVM_BOOT_IDT_OFFSET 0x520

struct PkvmGuestState {
    X86ConfidentialGuest parent_obj;

    /*
     * firmware: if true (default), the pKVM kernel loads its built-in
     * pVM firmware and the BSP vCPU enters in 32-bit flat protected mode.
     * If false (--protected-vm-without-firmware equivalent), the VM is
     * still memory-isolated but boots directly without pVM firmware.
     */
    bool firmware;

    /* Optional path to a custom pVM firmware blob to load from userspace. */
    char *fw_file;

    /* Firmware GPA (default: PKVM_FW_GPA). */
    uint64_t fw_gpa;

    /* Firmware size reported by the kernel (only valid when firmware=true). */
    uint64_t fw_size;

    /* swiotlb: shared DMA bounce buffer for virtio devices. */
    uint64_t swiotlb_size;
};

static Error *pkvm_mig_blocker;

bool pkvm_enabled(void)
{
    MachineState *ms;

    if (!current_machine) {
        return false;
    }
    ms = MACHINE(current_machine);
    return ms->cgs &&
           !!object_dynamic_cast(OBJECT(ms->cgs), TYPE_PKVM_GUEST);
}

bool pkvm_firmware_enabled(void)
{
    MachineState *ms;
    PkvmGuestState *pkvm;

    if (!pkvm_enabled()) {
        return false;
    }
    ms = MACHINE(current_machine);
    pkvm = PKVM_GUEST(ms->cgs);
    return pkvm->firmware;
}

/*
 * pkvm_kvm_type - return the KVM VM type for KVM_CREATE_VM.
 *
 * pKVM requires KVM_X86_PKVM_PROTECTED_VM regardless of whether firmware
 * is enabled; the VM type controls memory isolation, not firmware loading.
 */
static int pkvm_kvm_type(X86ConfidentialGuest *cg)
{
    if (!kvm_is_vm_type_supported(KVM_X86_PKVM_PROTECTED_VM)) {
        error_report("pkvm: KVM_X86_PKVM_PROTECTED_VM not supported by "
                     "host kernel (check that pKVM is loaded)");
        exit(1);
    }
    return KVM_X86_PKVM_PROTECTED_VM;
}

/*
 * pkvm_kvm_init - perform pKVM-specific VM initialization after KVM_CREATE_VM.
 *
 * When firmware is enabled, this queries the kernel for the pVM firmware
 * size and registers the firmware GPA with the hypervisor via KVM_ENABLE_CAP.
 * When firmware is disabled (ProtectedWithoutFirmware mode), the VM is
 * created as isolated but no firmware is loaded.
 */
static int pkvm_kvm_init(ConfidentialGuestSupport *cgs, Error **errp)
{
    PkvmGuestState *pkvm = PKVM_GUEST(cgs);
    KVMState *s = kvm_state;
    struct kvm_enable_cap cap = {};
    struct kvm_protected_vm_info info = {};
    int ret;

    if (!kvm_pkvm_protected_vm_supported()) {
        error_setg(errp, "pkvm: KVM_CAP_X86_PROTECTED_VM not supported by "
                   "host kernel");
        return -EINVAL;
    }

    if (pkvm->firmware) {
        /*
         * Step 1: query the firmware size embedded in the pKVM kernel module.
         * FLAGS_INFO writes the kvm_protected_vm_info struct via args[0].
         */
        cap.cap   = KVM_CAP_X86_PROTECTED_VM;
        cap.flags = KVM_CAP_X86_PROTECTED_VM_FLAGS_INFO;
        cap.args[0] = (uint64_t)(uintptr_t)&info;
        ret = kvm_vm_ioctl(s, KVM_ENABLE_CAP, &cap);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "pkvm: KVM_ENABLE_CAP FLAGS_INFO failed");
            return ret;
        }

        if (info.firmware_size == 0) {
            error_setg(errp,
                       "pkvm: kernel reports zero firmware size; "
                       "is a pKVM firmware image built into the module?");
            return -EINVAL;
        }
        if (info.firmware_size > PKVM_FW_MAX_SIZE) {
            error_setg(errp,
                       "pkvm: kernel firmware size %"PRIu64" exceeds "
                       "maximum %"PRIu64,
                       (uint64_t)info.firmware_size, (uint64_t)PKVM_FW_MAX_SIZE);
            return -EINVAL;
        }
        pkvm->fw_size = info.firmware_size;

        /*
         * Step 2: tell the hypervisor where in guest physical memory to
         * place the firmware.  The kernel maps it into the guest EPT.
         */
        memset(&cap, 0, sizeof(cap));
        cap.cap   = KVM_CAP_X86_PROTECTED_VM;
        cap.flags = KVM_CAP_X86_PROTECTED_VM_FLAGS_SET_FW_GPA;
        cap.args[0] = pkvm->fw_gpa;
        ret = kvm_vm_ioctl(s, KVM_ENABLE_CAP, &cap);
        if (ret < 0) {
            error_setg_errno(errp, -ret,
                             "pkvm: KVM_ENABLE_CAP FLAGS_SET_FW_GPA "
                             "(0x%"PRIx64") failed", pkvm->fw_gpa);
            return ret;
        }
    }

    /*
     * pKVM protects guest memory from the host; migration is not supported.
     */
    error_setg(&pkvm_mig_blocker,
               "Migration is not supported with pKVM protected guests");
    ret = migrate_add_blocker(&pkvm_mig_blocker, errp);
    if (ret < 0) {
        error_free(pkvm_mig_blocker);
        pkvm_mig_blocker = NULL;
        return ret;
    }

    cgs->ready = true;
    return 0;
}

/*
 * pkvm_configure_flat32_segments - set up vCPU for pVM firmware entry.
 *
 * The pKVM pVM firmware ABI requires the BSP vCPU to start in 32-bit flat
 * protected mode with paging disabled (CR0.PE=1, CR0.PG=0).  Segment
 * descriptors cover the full 4GB address space so the firmware can access
 * all of guest physical memory before enabling paging.
 *
 * This matches crosvm's configure_segments_and_sregs_flat32().
 */
void pkvm_configure_flat32_segments(CPUX86State *env)
{
    /* 32-bit code segment: execute/read, base=0, limit=4GB, DPL=0 */
    env->segs[R_CS].selector = 0x10;
    env->segs[R_CS].base     = 0;
    env->segs[R_CS].limit    = 0xffffffff;
    env->segs[R_CS].flags    = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                               DESC_R_MASK | DESC_G_MASK | DESC_B_MASK;

    /* 32-bit data segment: read/write, base=0, limit=4GB, DPL=0 */
    env->segs[R_DS].selector = 0x18;
    env->segs[R_DS].base     = 0;
    env->segs[R_DS].limit    = 0xffffffff;
    env->segs[R_DS].flags    = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
                               DESC_G_MASK | DESC_B_MASK;

    env->segs[R_ES] = env->segs[R_DS];
    env->segs[R_FS] = env->segs[R_DS];
    env->segs[R_GS] = env->segs[R_DS];
    env->segs[R_SS] = env->segs[R_DS];

    /* Minimal GDT: 5 descriptors (NULL, NULL, CS, DS, TSS), limit=39 */
    env->gdt.base  = PKVM_BOOT_GDT_OFFSET;
    env->gdt.limit = 39;

    /* Null IDT */
    env->idt.base  = PKVM_BOOT_IDT_OFFSET;
    env->idt.limit = 0xffff;

    /* CR0: set PE (protected mode), clear PG (paging disabled) */
    env->cr[0] = (env->cr[0] | CR0_PE_MASK) & ~CR0_PG_MASK;
}

/* --------------------------------------------------------------------
 * QOM type registration
 * -------------------------------------------------------------------- */

static void pkvm_guest_instance_init(Object *obj)
{
    PkvmGuestState *pkvm = PKVM_GUEST(obj);

    pkvm->firmware    = true;
    pkvm->fw_gpa      = PKVM_FW_GPA;
    pkvm->swiotlb_size = PKVM_SWIOTLB_DEFAULT_SIZE;
}

static void pkvm_guest_instance_finalize(Object *obj)
{
    PkvmGuestState *pkvm = PKVM_GUEST(obj);

    g_free(pkvm->fw_file);
}

static bool pkvm_get_firmware(Object *obj, Error **errp)
{
    return PKVM_GUEST(obj)->firmware;
}

static void pkvm_set_firmware(Object *obj, bool value, Error **errp)
{
    PKVM_GUEST(obj)->firmware = value;
}

static char *pkvm_get_fw_file(Object *obj, Error **errp)
{
    return g_strdup(PKVM_GUEST(obj)->fw_file);
}

static void pkvm_set_fw_file(Object *obj, const char *value, Error **errp)
{
    PkvmGuestState *pkvm = PKVM_GUEST(obj);

    g_free(pkvm->fw_file);
    pkvm->fw_file = g_strdup(value);
}

static void pkvm_guest_class_init(ObjectClass *oc, void *data)
{
    ConfidentialGuestSupportClass *cgs_klass =
        CONFIDENTIAL_GUEST_SUPPORT_CLASS(oc);
    X86ConfidentialGuestClass *x86_klass = X86_CONFIDENTIAL_GUEST_CLASS(oc);

    x86_klass->kvm_type  = pkvm_kvm_type;
    cgs_klass->kvm_init  = pkvm_kvm_init;

    object_class_property_add_bool(oc, "firmware",
                                   pkvm_get_firmware, pkvm_set_firmware);
    object_class_property_set_description(oc, "firmware",
        "If on (default), the pKVM kernel loads its built-in pVM firmware "
        "and the BSP vCPU starts in 32-bit flat protected mode. "
        "If off, the VM is memory-isolated but boots directly without "
        "pVM firmware (useful for testing isolation independently).");

    object_class_property_add_str(oc, "fw-file",
                                  pkvm_get_fw_file, pkvm_set_fw_file);
    object_class_property_set_description(oc, "fw-file",
        "Path to a custom pVM firmware binary to load at the firmware GPA "
        "(0x7FC00000). If unset, the pKVM kernel module provides the "
        "firmware. Requires firmware=on.");
}

static const TypeInfo pkvm_guest_info = {
    .parent        = TYPE_X86_CONFIDENTIAL_GUEST,
    .name          = TYPE_PKVM_GUEST,
    .instance_size = sizeof(PkvmGuestState),
    .instance_init = pkvm_guest_instance_init,
    .instance_finalize = pkvm_guest_instance_finalize,
    .class_init    = pkvm_guest_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void pkvm_register_types(void)
{
    type_register_static(&pkvm_guest_info);
}

type_init(pkvm_register_types);
