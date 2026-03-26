/*
 * x86 pKVM guest support.
 *
 * Copyright (c) 2026
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#ifndef TARGET_I386_PKVM_H
#define TARGET_I386_PKVM_H

#ifndef CONFIG_USER_ONLY
#include CONFIG_DEVICES
#endif

typedef struct Error Error;

#if !defined(CONFIG_SEV) || defined(CONFIG_USER_ONLY)
#define pkvm_enabled() 0
struct X86MachineState;
typedef struct X86CPU X86CPU;
static inline bool pkvm_guest_hole_enabled(void)
{
    return false;
}
static inline bool pkvm_guest_is_direct_kernel_boot(void)
{
    return false;
}
static inline bool pkvm_guest_uses_firmware(void)
{
    return false;
}
static inline void pkvm_guest_set_fw_boot_state(uint64_t kernel_entry,
                                                uint64_t dtb_addr)
{
}
static inline void pkvm_guest_set_kernel_boot_state(uint64_t kernel_entry,
                                                    uint64_t zero_page_addr,
                                                    uint64_t stack_ptr,
                                                    bool is_64bit)
{
}
static inline void pkvm_guest_apply_boot_state(X86CPU *cpu)
{
}
static inline int pkvm_guest_set_fw_gpa(uint64_t gpa, Error **errp)
{
    return -1;
}
static inline uint64_t pkvm_guest_fw_max_size(void)
{
    return 0;
}
#else
#include "qapi/error.h"

bool pkvm_enabled(void);
bool pkvm_guest_hole_enabled(void);
bool pkvm_guest_is_direct_kernel_boot(void);
bool pkvm_guest_uses_firmware(void);
void pkvm_guest_set_fw_boot_state(uint64_t kernel_entry, uint64_t dtb_addr);
void pkvm_guest_set_kernel_boot_state(uint64_t kernel_entry,
                                      uint64_t zero_page_addr,
                                      uint64_t stack_ptr,
                                      bool is_64bit);
void pkvm_guest_apply_boot_state(X86CPU *cpu);
int pkvm_guest_set_fw_gpa(uint64_t gpa, Error **errp);
uint64_t pkvm_guest_fw_max_size(void);
#endif

#define TYPE_PKVM_GUEST "pkvm-guest"

#define PKVM_FW_MAX_SIZE   (4 * 1024 * 1024ULL)
#define PKVM_FW_START      (0x80000000ULL - PKVM_FW_MAX_SIZE)
#define PKVM_FW_END        (PKVM_FW_START + PKVM_FW_MAX_SIZE)

#define PKVM_BZIMAGE_LOAD_ADDR   0x200000ULL
#define PKVM_BOOT_GDT_ADDR       0x1500ULL
#define PKVM_BOOT_IDT_ADDR       0x1528ULL
#define PKVM_BOOT_PML4_ADDR      0x9000ULL
#define PKVM_BOOT_PDPTE_ADDR     0xa000ULL
#define PKVM_BOOT_PDE_ADDR       0xb000ULL

#endif
