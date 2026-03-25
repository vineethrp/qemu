/*
 * QEMU pKVM (Protected KVM) x86 guest support
 *
 * Copyright (c) 2025 Google LLC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef I386_PKVM_H
#define I386_PKVM_H

#ifndef CONFIG_USER_ONLY
# include CONFIG_DEVICES /* CONFIG_PKVM */
#endif

#if !defined(CONFIG_PKVM) || defined(CONFIG_USER_ONLY)
# define pkvm_enabled()          0
# define pkvm_firmware_enabled() 0
#else
bool pkvm_enabled(void);
bool pkvm_firmware_enabled(void);
#endif

#if !defined(CONFIG_USER_ONLY)

#define TYPE_PKVM_GUEST "pkvm-guest"

typedef struct CPUArchState CPUX86State;

void pkvm_configure_flat32_segments(CPUX86State *env);

#endif /* !CONFIG_USER_ONLY */

#endif /* I386_PKVM_H */
