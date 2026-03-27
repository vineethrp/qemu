#ifndef HW_I386_PKVM_ACPI_PM_H
#define HW_I386_PKVM_ACPI_PM_H

#include "hw/i386/x86.h"

#define TYPE_PKVM_ACPI_PM "pkvm-acpi-pm"

#define PKVM_ACPI_PM_IO_BASE 0x600
#define PKVM_ACPI_PM_EVT_LEN 4
#define PKVM_ACPI_PM_CNT_LEN 2
#define PKVM_ACPI_GPE0_BLK_LEN 64
#define PKVM_ACPI_GPE0_BLK_OFFSET 8
#define PKVM_ACPI_PM_IO_LEN (PKVM_ACPI_GPE0_BLK_OFFSET + PKVM_ACPI_GPE0_BLK_LEN)
#define PKVM_ACPI_SCI_IRQ 9

void pkvm_microvm_acpi_pm_init(X86MachineState *x86ms);

#endif
