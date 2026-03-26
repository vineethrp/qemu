/*
 * QEMU x86 pKVM support.
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"

#include <linux/kvm.h>

#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "system/kvm.h"
#include "system/system.h"
#include "hw/i386/x86.h"
#include "target/i386/cpu.h"
#include "target/i386/host-cpu.h"
#include "confidential-guest.h"
#include "pkvm.h"

OBJECT_DECLARE_TYPE(PkvmGuestState, PkvmGuestClass, PKVM_GUEST)

typedef enum PkvmBootMode {
    PKVM_BOOT_MODE_NONE,
    PKVM_BOOT_MODE_FIRMWARE,
    PKVM_BOOT_MODE_DIRECT_FLAT32,
    PKVM_BOOT_MODE_DIRECT_LONG64,
} PkvmBootMode;

struct PkvmGuestState {
    X86ConfidentialGuest parent_obj;

    uint64_t firmware_size;
    uint64_t boot_rip;
    uint64_t boot_rsi;
    uint64_t boot_rsp;
    uint64_t boot_rdi;
    uint64_t boot_rdx;
    bool boot_state_valid;
    PkvmBootMode boot_mode;
};

struct PkvmGuestClass {
    X86ConfidentialGuestClass parent_class;
};

static PkvmGuestState *pkvm_guest_get(void)
{
    ConfidentialGuestSupport *cgs = current_machine ? current_machine->cgs : NULL;

    return cgs ? PKVM_GUEST(object_dynamic_cast(OBJECT(cgs), TYPE_PKVM_GUEST))
               : NULL;
}

bool pkvm_enabled(void)
{
    return pkvm_guest_get() != NULL;
}

bool pkvm_guest_hole_enabled(void)
{
    return false;
}

bool pkvm_guest_is_direct_kernel_boot(void)
{
    return pkvm_enabled();
}

bool pkvm_guest_uses_firmware(void)
{
    return false;
}

uint64_t pkvm_guest_fw_max_size(void)
{
    return PKVM_FW_MAX_SIZE;
}

static int pkvm_kvm_init(ConfidentialGuestSupport *cgs, Error **errp)
{
    PkvmGuestState *pkvm = PKVM_GUEST(cgs);
    struct kvm_protected_vm_info info = {};
    char vendor[CPUID_VENDOR_SZ + 1] = { 0 };
    int ret;

    host_cpu_vendor_fms(vendor, NULL, NULL, NULL);
    if (!g_str_equal(vendor, CPUID_VENDOR_INTEL)) {
        error_setg(errp,
                   "x86 pKVM is currently supported only on Intel hosts "
                   "(detected host vendor: %s)", vendor[0] ? vendor : "unknown");
        return -EINVAL;
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_X86_PROTECTED_VM,
                            KVM_CAP_X86_PROTECTED_VM_FLAGS_INFO,
                            (uintptr_t)&info, 0, 0, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret,
                         "pKVM: failed to query protected VM firmware info");
        return ret;
    }

    pkvm->firmware_size = info.firmware_size;
    kvm_mark_guest_state_protected();
    cgs->ready = true;
    return 0;
}

int pkvm_guest_set_fw_gpa(uint64_t gpa, Error **errp)
{
    PkvmGuestState *pkvm = pkvm_guest_get();
    int ret;

    if (!pkvm) {
        error_setg(errp, "pKVM is not enabled");
        return -EINVAL;
    }

    if (!pkvm->firmware_size) {
        error_setg(errp, "pKVM firmware is not available");
        return -EINVAL;
    }

    if (pkvm->firmware_size > PKVM_FW_MAX_SIZE) {
        error_setg(errp,
                   "pKVM firmware size 0x%" PRIx64 " exceeds reserved window 0x%" PRIx64,
                   pkvm->firmware_size, (uint64_t)PKVM_FW_MAX_SIZE);
        return -ENOMEM;
    }

    ret = kvm_vm_enable_cap(kvm_state, KVM_CAP_X86_PROTECTED_VM,
                            KVM_CAP_X86_PROTECTED_VM_FLAGS_SET_FW_GPA,
                            gpa, 0, 0, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret,
                         "pKVM: failed to configure firmware GPA");
        return ret;
    }

    return 0;
}

void pkvm_guest_set_fw_boot_state(uint64_t kernel_entry, uint64_t dtb_addr)
{
    PkvmGuestState *pkvm = pkvm_guest_get();

    if (!pkvm) {
        return;
    }

    pkvm->boot_mode = PKVM_BOOT_MODE_FIRMWARE;
    pkvm->boot_rdi = kernel_entry;
    pkvm->boot_rdx = dtb_addr;
    pkvm->boot_rip = 0;
    pkvm->boot_rsi = 0;
    pkvm->boot_rsp = 0;
    pkvm->boot_state_valid = true;
}

void pkvm_guest_set_kernel_boot_state(uint64_t kernel_entry,
                                      uint64_t zero_page_addr,
                                      uint64_t stack_ptr,
                                      bool is_64bit)
{
    PkvmGuestState *pkvm = pkvm_guest_get();

    if (!pkvm) {
        return;
    }

    pkvm->boot_mode = is_64bit ? PKVM_BOOT_MODE_DIRECT_LONG64
                               : PKVM_BOOT_MODE_DIRECT_FLAT32;
    pkvm->boot_rip = kernel_entry;
    pkvm->boot_rsi = zero_page_addr;
    pkvm->boot_rsp = stack_ptr;
    pkvm->boot_rdi = 0;
    pkvm->boot_rdx = 0;
    pkvm->boot_state_valid = true;
}

void pkvm_guest_apply_boot_state(X86CPU *cpu)
{
    PkvmGuestState *pkvm = pkvm_guest_get();
    CPUX86State *env = &cpu->env;
    uint32_t data_seg = DESC_P_MASK | DESC_S_MASK | DESC_W_MASK |
                        DESC_G_MASK | DESC_B_MASK | DESC_A_MASK;
    uint32_t code_seg_flat32 = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                               DESC_R_MASK | DESC_G_MASK | DESC_B_MASK |
                               DESC_A_MASK;
    uint32_t code_seg_long64 = DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                               DESC_R_MASK | DESC_G_MASK | DESC_L_MASK |
                               DESC_A_MASK;
    uint32_t tss_seg = DESC_P_MASK | DESC_G_MASK | (11 << DESC_TYPE_SHIFT);

    if (!pkvm || !pkvm->boot_state_valid || !cpu_is_bsp(cpu)) {
        return;
    }

    env->eip = pkvm->boot_rip;
    env->regs[R_ESI] = pkvm->boot_rsi;
    env->regs[R_ESP] = pkvm->boot_rsp;
    env->regs[R_EDI] = pkvm->boot_rdi;
    env->regs[R_EDX] = pkvm->boot_rdx;
    env->cr[2] = 0;

    env->gdt.base = PKVM_BOOT_GDT_ADDR;
    env->gdt.limit = (pkvm->boot_mode == PKVM_BOOT_MODE_DIRECT_LONG64 ? 6 : 5) * 8 - 1;
    env->idt.base = PKVM_BOOT_IDT_ADDR;
    env->idt.limit = 8 - 1;

    if (pkvm->boot_mode == PKVM_BOOT_MODE_DIRECT_LONG64) {
        cpu_load_efer(env, MSR_EFER_LME);
        cpu_x86_update_cr3(env, PKVM_BOOT_PML4_ADDR);
        cpu_x86_update_cr4(env, CR4_PAE_MASK);
        cpu_x86_update_cr0(env, CR0_ET_MASK | CR0_NE_MASK |
                                CR0_PE_MASK | CR0_PG_MASK);
    } else {
        cpu_load_efer(env, 0);
        cpu_x86_update_cr3(env, 0);
        cpu_x86_update_cr4(env, 0);
        cpu_x86_update_cr0(env, CR0_ET_MASK | CR0_NE_MASK | CR0_PE_MASK);
    }

    cpu_x86_load_seg_cache(env, R_CS, 0x10, 0, 0xffffffff,
                           pkvm->boot_mode == PKVM_BOOT_MODE_DIRECT_LONG64 ?
                           code_seg_long64 : code_seg_flat32);
    cpu_x86_load_seg_cache(env, R_DS, 0x18, 0, 0xffffffff, data_seg);
    cpu_x86_load_seg_cache(env, R_ES, 0x18, 0, 0xffffffff, data_seg);
    cpu_x86_load_seg_cache(env, R_SS, 0x18, 0, 0xffffffff, data_seg);
    cpu_x86_load_seg_cache(env, R_FS, 0x18, 0, 0xffffffff, data_seg);
    cpu_x86_load_seg_cache(env, R_GS, 0x18, 0, 0xffffffff, data_seg);
    env->tr.selector = 0x20;
    env->tr.base = 0;
    env->tr.limit = 0xffffffff;
    env->tr.flags = tss_seg;
    env->ldt.selector = 0;
    env->ldt.base = 0;
    env->ldt.limit = 0;
    env->ldt.flags = 0;

    if (pkvm->boot_mode == PKVM_BOOT_MODE_FIRMWARE) {
        env->eip = 0xfff0;
    }
}

static int pkvm_kvm_type(X86ConfidentialGuest *cg)
{
    return KVM_X86_PKVM_PROTECTED_VM;
}

static uint32_t
pkvm_mask_cpuid_features(X86ConfidentialGuest *cg, uint32_t feature,
                         uint32_t index, int reg, uint32_t value)
{
    switch (feature) {
    case 1:
        if (reg == R_ECX) {
            return value & ~(CPUID_EXT_TSC_DEADLINE_TIMER |
                             CPUID_EXT_X2APIC);
        }
        break;
    case KVM_CPUID_FEATURES:
        if (reg == R_EAX || reg == R_EDX) {
            return 0;
        }
        break;
    case 7:
        if (index == 0 && reg == R_EBX) {
            return value & ~CPUID_7_0_EBX_TSC_ADJUST;
        }
        if (index == 0 && reg == R_EDX) {
            return value & ~(CPUID_7_0_EDX_SPEC_CTRL |
                             CPUID_7_0_EDX_STIBP |
                             CPUID_7_0_EDX_FLUSH_L1D |
                             CPUID_7_0_EDX_ARCH_CAPABILITIES |
                             CPUID_7_0_EDX_CORE_CAPABILITY |
                             CPUID_7_0_EDX_SPEC_CTRL_SSBD);
        }
        break;
    case 0x80000008:
        if (reg == R_EBX) {
            return value & ~CPUID_8000_0008_EBX_VIRT_SSBD;
        }
        break;
    }

    return value;
}

static void pkvm_guest_class_init(ObjectClass *oc, void *data)
{
    ConfidentialGuestSupportClass *klass = CONFIDENTIAL_GUEST_SUPPORT_CLASS(oc);
    X86ConfidentialGuestClass *x86_klass = X86_CONFIDENTIAL_GUEST_CLASS(oc);

    klass->kvm_init = pkvm_kvm_init;
    x86_klass->kvm_type = pkvm_kvm_type;
    x86_klass->mask_cpuid_features = pkvm_mask_cpuid_features;
}

static const TypeInfo pkvm_guest_info = {
    .parent = TYPE_X86_CONFIDENTIAL_GUEST,
    .name = TYPE_PKVM_GUEST,
    .instance_size = sizeof(PkvmGuestState),
    .class_init = pkvm_guest_class_init,
    .interfaces = (InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    },
};

static void pkvm_register_types(void)
{
    type_register_static(&pkvm_guest_info);
}

type_init(pkvm_register_types);
