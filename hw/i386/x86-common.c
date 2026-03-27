/*
 * Copyright (c) 2003-2004 Fabrice Bellard
 * Copyright (c) 2019, 2024 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qemu/datadir.h"
#include "qemu/bswap.h"
#include "qapi/error.h"
#include "system/numa.h"
#include "system/kvm.h"
#include "system/system.h"
#include "system/xen.h"
#include "trace.h"

#include "hw/i386/x86.h"
#include "hw/i386/acpi-build.h"
#include "hw/i386/microvm.h"
#include "hw/i386/pkvm-acpi-pm.h"
#include "hw/acpi/acpi.h"
#include "hw/acpi/piix4.h"
#include "target/i386/cpu.h"
#include "target/i386/pkvm.h"
#include "hw/pci/pci.h"
#include "hw/rtc/mc146818rtc.h"
#include "target/i386/sev.h"

#include "hw/acpi/cpu_hotplug.h"
#include "hw/acpi/vmgenid.h"
#include "hw/irq.h"
#include "hw/loader.h"
#include "hw/southbridge/ich9.h"
#include "e820_memory_layout.h"
#include "multiboot.h"
#include "elf.h"
#include "standard-headers/asm-x86/bootparam.h"
#include CONFIG_DEVICES
#include "kvm/kvm_i386.h"

#ifdef CONFIG_XEN_EMU
#include "hw/xen/xen.h"
#include "hw/i386/kvm/xen_evtchn.h"
#endif

/* Physical Address of PVH entry point read from kernel ELF NOTE */
static size_t pvh_start_addr;
static const hwaddr x86_pkvm_zero_page_addr = 0x7000;
static const hwaddr x86_pkvm_boot_stack_pointer = 0x8000;
static const hwaddr x86_pkvm_mpf_addr = 0x0009fff0;
static const hwaddr x86_pkvm_mptable_addr = 0x0009e800;
static const hwaddr x86_pkvm_acpi_rsdp_addr = 0x000e0000;
static const hwaddr x86_pkvm_acpi_tables_addr = 0x000e1000;
static const hwaddr x86_pkvm_shared_low_mem_size = 0x00100000;

#define X86_PKVM_ACPI_TABLES_MAX_SIZE         0x1f000
#define X86_PKVM_ICH9_PMBASE                  0xb000
#define X86_PKVM_ACPI_RESERVED_START          x86_pkvm_acpi_rsdp_addr
#define X86_PKVM_ACPI_RESERVED_END            (x86_pkvm_acpi_tables_addr + \
                                               X86_PKVM_ACPI_TABLES_MAX_SIZE - 1)
#define X86_PKVM_BOOTPARAM_ACPI_RSDP_ADDR_OFFSET 0x070
#define X86_PKVM_BOOTPARAM_EXT_RAMDISK_IMAGE_OFFSET 0x0c0
#define X86_PKVM_BOOTPARAM_EXT_RAMDISK_SIZE_OFFSET  0x0c4
#define X86_PKVM_BOOTPARAM_EXT_CMD_LINE_PTR_OFFSET  0x0c8
#define X86_PKVM_BOOTPARAM_E820_COUNT_OFFSET   0x1e8
#define X86_PKVM_BOOTPARAM_SENTINEL_OFFSET     0x1ef
#define X86_PKVM_BOOTPARAM_HDR_OFFSET          0x1f1
#define X86_PKVM_BOOTPARAM_E820_TABLE_OFFSET   0x2d0
#define X86_PKVM_E820_MAX_ENTRIES              128
#define X86_PKVM_ACPI_SIG_LEN                  4
#define X86_PKVM_ACPI_TABLE_HEADER_LEN         36
#define X86_PKVM_ACPI_HEADER_LENGTH_OFFSET     4
#define X86_PKVM_ACPI_HEADER_CHECKSUM_OFFSET   9
#define X86_PKVM_MPTABLE_MAX_CPUS              254

#define X86_PKVM_MPC_CPU_ENABLED               0x1
#define X86_PKVM_MPC_CPU_BOOTPROCESSOR         0x2
#define X86_PKVM_MPC_APIC_USABLE               0x1
#define X86_PKVM_MP_IRQDIR_DEFAULT             0
#define X86_PKVM_MP_INTSRC_TYPE_INT            0
#define X86_PKVM_MP_LINTSRC_TYPE_EXTINT        3
#define X86_PKVM_MP_LINTSRC_TYPE_NMI           1
#define X86_PKVM_APIC_DEFAULT_ADDRESS          0xfee00000
#define X86_PKVM_IOAPIC_DEFAULT_ADDRESS        0xfec00000
#define X86_PKVM_APIC_VERSION                  0x14

#define X86_PKVM_FADT_FIELD_FACS_ADDR32           36
#define X86_PKVM_FADT_FIELD_DSDT_ADDR32           40
#define X86_PKVM_FADT_FIELD_SCI_INTERRUPT         46
#define X86_PKVM_FADT_FIELD_SMI_COMMAND           48
#define X86_PKVM_FADT_FIELD_ACPI_ENABLE           52
#define X86_PKVM_FADT_FIELD_ACPI_DISABLE          53
#define X86_PKVM_FADT_FIELD_PM1A_EVENT_BLK_ADDR   56
#define X86_PKVM_FADT_FIELD_PM1B_EVENT_BLK_ADDR   60
#define X86_PKVM_FADT_FIELD_PM1A_CONTROL_BLK_ADDR 64
#define X86_PKVM_FADT_FIELD_PM1B_CONTROL_BLK_ADDR 68
#define X86_PKVM_FADT_FIELD_PM2_CONTROL_BLK_ADDR  72
#define X86_PKVM_FADT_FIELD_PM_TMR_BLK_ADDR       76
#define X86_PKVM_FADT_FIELD_GPE0_BLK_ADDR         80
#define X86_PKVM_FADT_FIELD_GPE1_BLK_ADDR         84
#define X86_PKVM_FADT_FIELD_PM1A_EVENT_BLK_LEN    88
#define X86_PKVM_FADT_FIELD_PM1A_CONTROL_BLK_LEN  89
#define X86_PKVM_FADT_FIELD_PM2_CONTROL_BLK_LEN   90
#define X86_PKVM_FADT_FIELD_PM_TMR_LEN            91
#define X86_PKVM_FADT_FIELD_GPE0_BLK_LEN          92
#define X86_PKVM_FADT_FIELD_GPE1_BLK_LEN          93
#define X86_PKVM_FADT_FIELD_FACS_ADDR64           132
#define X86_PKVM_FADT_FIELD_DSDT_ADDR64           140
#define X86_PKVM_FADT_FIELD_X_PM1A_EVENT_BLK_ADDR 148
#define X86_PKVM_FADT_FIELD_X_PM1B_EVENT_BLK_ADDR 160
#define X86_PKVM_FADT_FIELD_X_PM1A_CONTROL_BLK_ADDR 172
#define X86_PKVM_FADT_FIELD_X_PM1B_CONTROL_BLK_ADDR 184
#define X86_PKVM_FADT_FIELD_X_PM2_CONTROL_BLK_ADDR 196
#define X86_PKVM_FADT_FIELD_X_PM_TMR_BLK_ADDR      208
#define X86_PKVM_FADT_FIELD_X_GPE0_BLK_ADDR        220
#define X86_PKVM_FADT_FIELD_X_GPE1_BLK_ADDR        232
#define X86_PKVM_ACPI_GAS_SIZE                     12

struct x86_pkvm_setup_header {
    uint8_t setup_sects;
    uint16_t root_flags;
    uint32_t syssize;
    uint16_t ram_size;
    uint16_t vid_mode;
    uint16_t root_dev;
    uint16_t boot_flag;
    uint16_t jump;
    uint32_t header;
    uint16_t version;
    uint32_t realmode_swtch;
    uint16_t start_sys_seg;
    uint16_t kernel_version;
    uint8_t type_of_loader;
    uint8_t loadflags;
    uint16_t setup_move_size;
    uint32_t code32_start;
    uint32_t ramdisk_image;
    uint32_t ramdisk_size;
    uint32_t bootsect_kludge;
    uint16_t heap_end_ptr;
    uint8_t ext_loader_ver;
    uint8_t ext_loader_type;
    uint32_t cmd_line_ptr;
    uint32_t initrd_addr_max;
    uint32_t kernel_alignment;
    uint8_t relocatable_kernel;
    uint8_t min_alignment;
    uint16_t xloadflags;
    uint32_t cmdline_size;
    uint32_t hardware_subarch;
    uint64_t hardware_subarch_data;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint64_t setup_data;
    uint64_t pref_address;
    uint32_t init_size;
    uint32_t handover_offset;
    uint32_t kernel_info_offset;
} QEMU_PACKED;

struct x86_pkvm_bios_linker_loader_entry {
    uint32_t command;
    union {
        struct {
            char file[56];
            uint32_t align;
            uint8_t zone;
        } alloc;
        struct {
            char dest_file[56];
            char src_file[56];
            uint32_t offset;
            uint8_t size;
        } pointer;
        struct {
            char file[56];
            uint32_t offset;
            uint32_t start;
            uint32_t length;
        } cksum;
        struct {
            char dest_file[56];
            char src_file[56];
            uint32_t dst_offset;
            uint32_t src_offset;
            uint8_t size;
        } wr_pointer;
        char pad[124];
    };
} QEMU_PACKED;

struct x86_pkvm_mpf_intel {
    char signature[4];
    uint32_t physptr;
    uint8_t length;
    uint8_t specification;
    int8_t checksum;
    uint8_t feature1;
    uint8_t feature2;
    uint8_t feature3;
    uint8_t feature4;
    uint8_t feature5;
} QEMU_PACKED;

struct x86_pkvm_mpc_table {
    char signature[4];
    uint16_t length;
    uint8_t spec;
    int8_t checksum;
    char oem[8];
    char productid[12];
    uint32_t oemptr;
    uint16_t oemsize;
    uint16_t oemcount;
    uint32_t lapic;
    uint32_t reserved;
} QEMU_PACKED;

struct x86_pkvm_mpc_cpu {
    uint8_t type;
    uint8_t apicid;
    uint8_t apicver;
    uint8_t cpuflag;
    uint32_t cpufeature;
    uint32_t featureflag;
    uint32_t reserved[2];
} QEMU_PACKED;

struct x86_pkvm_mpc_bus {
    uint8_t type;
    uint8_t busid;
    char bustype[6];
} QEMU_PACKED;

struct x86_pkvm_mpc_ioapic {
    uint8_t type;
    uint8_t apicid;
    uint8_t apicver;
    uint8_t flags;
    uint32_t apicaddr;
} QEMU_PACKED;

struct x86_pkvm_mpc_intsrc {
    uint8_t type;
    uint8_t irqtype;
    uint16_t irqflag;
    uint8_t srcbus;
    uint8_t srcbusirq;
    uint8_t dstapic;
    uint8_t dstirq;
} QEMU_PACKED;

struct x86_pkvm_mpc_lintsrc {
    uint8_t type;
    uint8_t irqtype;
    uint16_t irqflag;
    uint8_t srcbusid;
    uint8_t srcbusirq;
    uint8_t destapic;
    uint8_t destapiclint;
} QEMU_PACKED;

enum {
    X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ALLOCATE = 0x1,
    X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ADD_POINTER = 0x2,
    X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ADD_CHECKSUM = 0x3,
    X86_PKVM_BIOS_LINKER_LOADER_COMMAND_WRITE_POINTER = 0x4,
};

typedef struct X86PkvmAcpiFile {
    const char *name;
    uint8_t *blob;
    size_t len;
    hwaddr addr;
} X86PkvmAcpiFile;

typedef struct X86PkvmAcpiPmInfo {
    uint32_t pm_io_base;
    uint32_t gpe0_blk;
    uint8_t gpe0_blk_len;
    uint16_t sci_int;
} X86PkvmAcpiPmInfo;

#define X86_PKVM_ACPI_HW_ERROR_FW_CFG_FILE "etc/hardware_errors"

static X86PkvmAcpiFile *x86_pkvm_acpi_find_file(X86PkvmAcpiFile *files,
                                                size_t nr_files,
                                                const char *name)
{
    size_t i;

    for (i = 0; i < nr_files; i++) {
        if (!strcmp(files[i].name, name)) {
            return &files[i];
        }
    }

    return NULL;
}

static void x86_pkvm_acpi_update_checksum(uint8_t *blob, size_t len)
{
    uint8_t sum = 0;
    size_t i;

    if (len <= X86_PKVM_ACPI_HEADER_CHECKSUM_OFFSET) {
        return;
    }

    blob[X86_PKVM_ACPI_HEADER_CHECKSUM_OFFSET] = 0;
    for (i = 0; i < len; i++) {
        sum += blob[i];
    }
    blob[X86_PKVM_ACPI_HEADER_CHECKSUM_OFFSET] = (uint8_t)(0 - sum);
}

static uint8_t *x86_pkvm_acpi_find_table(uint8_t *blob, size_t blob_len,
                                         const char *signature, uint32_t *len)
{
    size_t offset = 0;

    while (offset + X86_PKVM_ACPI_TABLE_HEADER_LEN <= blob_len) {
        uint8_t *table = blob + offset;
        uint32_t table_len = ldl_le_p(table + X86_PKVM_ACPI_HEADER_LENGTH_OFFSET);

        if (table_len < X86_PKVM_ACPI_TABLE_HEADER_LEN ||
            offset + table_len > blob_len) {
            return NULL;
        }
        if (!memcmp(table, signature, X86_PKVM_ACPI_SIG_LEN)) {
            *len = table_len;
            return table;
        }

        offset += table_len;
    }

    return NULL;
}

static bool x86_pkvm_acpi_get_pm_info(X86PkvmAcpiPmInfo *pm)
{
    Object *pmdev = object_resolve_type_unambiguous(TYPE_PIIX4_PM, NULL);

    if (!pmdev) {
        pmdev = object_resolve_type_unambiguous(TYPE_ICH9_LPC_DEVICE, NULL);
    }
    if (!pmdev) {
        pmdev = object_resolve_type_unambiguous(TYPE_PKVM_ACPI_PM, NULL);
    }
    if (!pmdev) {
        return false;
    }

    pm->pm_io_base = object_property_get_uint(pmdev, ACPI_PM_PROP_PM_IO_BASE, NULL);
    pm->gpe0_blk = object_property_get_uint(pmdev, ACPI_PM_PROP_GPE0_BLK, NULL);
    pm->gpe0_blk_len = object_property_get_uint(pmdev, ACPI_PM_PROP_GPE0_BLK_LEN,
                                                NULL);
    pm->sci_int = object_property_get_uint(pmdev, ACPI_PM_PROP_SCI_INT, NULL);
    return true;
}

static void x86_pkvm_configure_ich9_lpc_pm_base(void)
{
    Object *obj = object_resolve_type_unambiguous(TYPE_ICH9_LPC_DEVICE, NULL);
    PCIDevice *pdev;
    PCIDeviceClass *pc;

    if (!obj) {
        return;
    }

    pdev = PCI_DEVICE(obj);
    pc = PCI_DEVICE_GET_CLASS(pdev);
    pc->config_write(pdev, ICH9_LPC_PMBASE,
                     X86_PKVM_ICH9_PMBASE | ICH9_LPC_PMBASE_RTE, 4);
    pc->config_write(pdev, ICH9_LPC_ACPI_CTRL,
                     ICH9_LPC_ACPI_CTRL_ACPI_EN | ICH9_LPC_ACPI_CTRL_9, 1);
}

static void x86_pkvm_acpi_override_fadt(X86PkvmAcpiFile *tables_file)
{
    X86PkvmAcpiPmInfo pm;
    uint8_t *fadt;
    uint8_t *facs;
    uint8_t *dsdt;
    uint32_t fadt_len;
    uint32_t facs_len;
    uint32_t dsdt_len;
    uint64_t facs_addr;
    uint64_t dsdt_addr;

    if (!x86_pkvm_acpi_get_pm_info(&pm)) {
        error_report("pkvm direct boot could not resolve ACPI PM device");
        exit(1);
    }

    fadt = x86_pkvm_acpi_find_table(tables_file->blob, tables_file->len, "FACP",
                                    &fadt_len);
    if (!fadt) {
        error_report("pkvm direct boot could not locate FADT");
        exit(1);
    }

    facs = x86_pkvm_acpi_find_table(tables_file->blob, tables_file->len, "FACS",
                                    &facs_len);
    dsdt = x86_pkvm_acpi_find_table(tables_file->blob, tables_file->len, "DSDT",
                                    &dsdt_len);
    if (!dsdt) {
        error_report("pkvm direct boot could not locate DSDT");
        exit(1);
    }
    facs_addr = facs ? tables_file->addr + (facs - tables_file->blob) : 0;
    dsdt_addr = tables_file->addr + (dsdt - tables_file->blob);

    stl_le_p(fadt + X86_PKVM_FADT_FIELD_FACS_ADDR32, 0);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_DSDT_ADDR32, 0);
    stw_le_p(fadt + X86_PKVM_FADT_FIELD_SCI_INTERRUPT, pm.sci_int);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_SMI_COMMAND, 0);
    fadt[X86_PKVM_FADT_FIELD_ACPI_ENABLE] = 0;
    fadt[X86_PKVM_FADT_FIELD_ACPI_DISABLE] = 0;

    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM1A_EVENT_BLK_ADDR, pm.pm_io_base);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM1B_EVENT_BLK_ADDR, 0);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM1A_CONTROL_BLK_ADDR, pm.pm_io_base + 4);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM1B_CONTROL_BLK_ADDR, 0);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM2_CONTROL_BLK_ADDR, 0);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_PM_TMR_BLK_ADDR, 0);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_GPE0_BLK_ADDR, pm.gpe0_blk);
    stl_le_p(fadt + X86_PKVM_FADT_FIELD_GPE1_BLK_ADDR, 0);

    fadt[X86_PKVM_FADT_FIELD_PM1A_EVENT_BLK_LEN] = 4;
    fadt[X86_PKVM_FADT_FIELD_PM1A_CONTROL_BLK_LEN] = 2;
    fadt[X86_PKVM_FADT_FIELD_PM2_CONTROL_BLK_LEN] = 0;
    fadt[X86_PKVM_FADT_FIELD_PM_TMR_LEN] = 0;
    fadt[X86_PKVM_FADT_FIELD_GPE0_BLK_LEN] = pm.gpe0_blk_len;
    fadt[X86_PKVM_FADT_FIELD_GPE1_BLK_LEN] = 0;

    stq_le_p(fadt + X86_PKVM_FADT_FIELD_FACS_ADDR64, facs_addr);
    stq_le_p(fadt + X86_PKVM_FADT_FIELD_DSDT_ADDR64, dsdt_addr);

    memset(fadt + X86_PKVM_FADT_FIELD_X_PM1B_EVENT_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_PM1A_EVENT_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_PM1B_CONTROL_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_PM1A_CONTROL_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_PM2_CONTROL_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_PM_TMR_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_GPE0_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);
    memset(fadt + X86_PKVM_FADT_FIELD_X_GPE1_BLK_ADDR, 0,
           X86_PKVM_ACPI_GAS_SIZE);

    x86_pkvm_acpi_update_checksum(fadt, fadt_len);
}

static bool x86_pkvm_acpi_patch_pointer(uint8_t *blob, size_t blob_len,
                                        uint32_t offset, uint8_t size,
                                        hwaddr value)
{
    uint64_t le_value = cpu_to_le64(value);

    if (offset + size > blob_len) {
        return false;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return false;
    }

    memcpy(blob + offset, &le_value, size);
    return true;
}

static bool x86_pkvm_acpi_get_addend(uint8_t *blob, size_t blob_len,
                                     uint32_t offset, uint8_t size,
                                     uint64_t *addend)
{
    uint16_t value16;
    uint32_t value32;
    uint64_t value64;

    if (offset + size > blob_len) {
        return false;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        return false;
    }

    switch (size) {
    case 1:
        *addend = blob[offset];
        break;
    case 2:
        memcpy(&value16, blob + offset, sizeof(value16));
        *addend = le16_to_cpu(value16);
        break;
    case 4:
        memcpy(&value32, blob + offset, sizeof(value32));
        *addend = le32_to_cpu(value32);
        break;
    case 8:
        memcpy(&value64, blob + offset, sizeof(value64));
        *addend = le64_to_cpu(value64);
        break;
    default:
        return false;
    }

    return true;
}

static bool x86_pkvm_acpi_apply_linker(GArray *cmd_blob, X86PkvmAcpiFile *files,
                                       size_t nr_files)
{
    size_t i;
    hwaddr next_addr = QEMU_ALIGN_UP(x86_pkvm_acpi_tables_addr +
                                     files[0].len, 0x1000);

    if (cmd_blob->len % sizeof(struct x86_pkvm_bios_linker_loader_entry)) {
        return false;
    }

    for (i = 0; i < cmd_blob->len;
         i += sizeof(struct x86_pkvm_bios_linker_loader_entry)) {
        struct x86_pkvm_bios_linker_loader_entry entry;
        uint32_t command;

        memcpy(&entry, cmd_blob->data + i, sizeof(entry));
        command = le32_to_cpu(entry.command);

        switch (command) {
        case 0:
            break;
        case X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ALLOCATE:
        {
            X86PkvmAcpiFile *file = x86_pkvm_acpi_find_file(files, nr_files,
                                                            entry.alloc.file);
            uint32_t align = le32_to_cpu(entry.alloc.align);

            if (!file || !file->blob) {
                error_report("pkvm acpi unsupported alloc file '%s'",
                             entry.alloc.file);
                return false;
            }

            if (file->addr) {
                break;
            }

            next_addr = QEMU_ALIGN_UP(next_addr, MAX(align, 1u));
            if (next_addr < x86_pkvm_acpi_tables_addr ||
                next_addr + file->len <
                x86_pkvm_acpi_tables_addr + files[0].len ||
                next_addr + file->len >
                x86_pkvm_acpi_tables_addr + X86_PKVM_ACPI_TABLES_MAX_SIZE) {
                error_report("pkvm acpi allocation overflow for '%s'",
                             entry.alloc.file);
                return false;
            }

            file->addr = next_addr;
            next_addr += file->len;
            break;
        }
        case X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ADD_POINTER:
        {
            X86PkvmAcpiFile *dest_file;
            X86PkvmAcpiFile *src_file;
            uint32_t offset;
            uint64_t addend;

            dest_file = x86_pkvm_acpi_find_file(files, nr_files,
                                                entry.pointer.dest_file);
            src_file = x86_pkvm_acpi_find_file(files, nr_files,
                                               entry.pointer.src_file);
            if (!dest_file || !src_file || !dest_file->blob || !src_file->blob ||
                !dest_file->addr || !src_file->addr) {
                error_report("pkvm acpi unsupported pointer dest='%s' src='%s'",
                             entry.pointer.dest_file, entry.pointer.src_file);
                return false;
            }

            offset = le32_to_cpu(entry.pointer.offset);
            if (!x86_pkvm_acpi_get_addend(dest_file->blob, dest_file->len, offset,
                                          entry.pointer.size, &addend)) {
                return false;
            }
            if (addend >= src_file->len) {
                return false;
            }
            if (!x86_pkvm_acpi_patch_pointer(dest_file->blob, dest_file->len,
                                             offset,
                                             entry.pointer.size,
                                             src_file->addr + addend)) {
                return false;
            }
            break;
        }
        case X86_PKVM_BIOS_LINKER_LOADER_COMMAND_ADD_CHECKSUM:
        {
            X86PkvmAcpiFile *file;
            uint32_t start, offset, length;
            uint8_t sum = 0;
            size_t j;

            file = x86_pkvm_acpi_find_file(files, nr_files, entry.cksum.file);
            if (!file || !file->blob || !file->addr) {
                error_report("pkvm acpi unsupported checksum file '%s'",
                             entry.cksum.file);
                return false;
            }

            start = le32_to_cpu(entry.cksum.start);
            offset = le32_to_cpu(entry.cksum.offset);
            length = le32_to_cpu(entry.cksum.length);
            if (start + length > file->len || offset >= file->len ||
                offset < start || offset >= start + length) {
                return false;
            }

            file->blob[offset] = 0;
            for (j = start; j < start + length; j++) {
                sum = sum - file->blob[j];
            }
            file->blob[offset] = sum;
            break;
        }
        case X86_PKVM_BIOS_LINKER_LOADER_COMMAND_WRITE_POINTER:
            break;
        default:
            error_report("pkvm acpi unsupported linker command 0x%x", command);
            return false;
        }
    }

    return true;
}

void x86_pkvm_write_direct_acpi_tables(AcpiBuildTables *tables,
                                       bool override_fadt)
{
    uint8_t *table_blob;
    uint8_t *rsdp_blob;
    X86PkvmAcpiFile files[] = {
        { ACPI_BUILD_TABLE_FILE, NULL, 0, 0 },
        { ACPI_BUILD_RSDP_FILE, NULL, 0, x86_pkvm_acpi_rsdp_addr },
        { ACPI_BUILD_TPMLOG_FILE, NULL, 0, 0 },
        { VMGENID_GUID_FW_CFG_FILE, NULL, 0, 0 },
        { X86_PKVM_ACPI_HW_ERROR_FW_CFG_FILE, NULL, 0, 0 },
    };
    uint64_t rsdp_addr = cpu_to_le64(x86_pkvm_acpi_rsdp_addr);
    size_t table_len, rsdp_len;
    size_t i;

    table_len = acpi_data_len(tables->table_data);
    rsdp_len = acpi_data_len(tables->rsdp);
    if (table_len > X86_PKVM_ACPI_TABLES_MAX_SIZE ||
        x86_pkvm_acpi_rsdp_addr + rsdp_len > x86_pkvm_acpi_tables_addr) {
        error_report("pkvm direct boot ACPI table placement is invalid");
        exit(1);
    }

    table_blob = g_memdup2(tables->table_data->data, table_len);
    rsdp_blob = g_memdup2(tables->rsdp->data, rsdp_len);
    files[0].blob = table_blob;
    files[0].len = table_len;
    files[0].addr = x86_pkvm_acpi_tables_addr;
    files[1].blob = rsdp_blob;
    files[1].len = rsdp_len;
    if (tables->tcpalog && acpi_data_len(tables->tcpalog)) {
        files[2].blob = g_memdup2(tables->tcpalog->data,
                                  acpi_data_len(tables->tcpalog));
        files[2].len = acpi_data_len(tables->tcpalog);
    }
    if (tables->vmgenid && acpi_data_len(tables->vmgenid)) {
        files[3].blob = g_memdup2(tables->vmgenid->data,
                                  acpi_data_len(tables->vmgenid));
        files[3].len = acpi_data_len(tables->vmgenid);
    }
    if (tables->hardware_errors && acpi_data_len(tables->hardware_errors)) {
        files[4].blob = g_memdup2(tables->hardware_errors->data,
                                  acpi_data_len(tables->hardware_errors));
        files[4].len = acpi_data_len(tables->hardware_errors);
    }

    if (!x86_pkvm_acpi_apply_linker(tables->linker->cmd_blob, files,
                                    ARRAY_SIZE(files))) {
        error_report("pkvm direct boot ACPI linker contains unsupported commands");
        exit(1);
    }
    if (override_fadt) {
        x86_pkvm_acpi_override_fadt(&files[0]);
    }

    for (i = 0; i < ARRAY_SIZE(files); i++) {
        if (files[i].blob && files[i].addr) {
            cpu_physical_memory_write(files[i].addr, files[i].blob, files[i].len);
        }
    }
    cpu_physical_memory_write(x86_pkvm_zero_page_addr +
                              X86_PKVM_BOOTPARAM_ACPI_RSDP_ADDR_OFFSET,
                              &rsdp_addr, sizeof(rsdp_addr));

    for (i = 0; i < ARRAY_SIZE(files); i++) {
        g_free(files[i].blob);
    }
}

void x86_pkvm_share_direct_boot_low_memory(void)
{
    if (!kvm_enabled()) {
        return;
    }

    if (kvm_set_memory_attributes_shared(0, x86_pkvm_shared_low_mem_size)) {
        error_report("pKVM direct boot failed to share the guest low memory");
        exit(1);
    }
}

void x86_pkvm_post_acpi_init(void)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AcpiBuildTables tables;

    if (!pkvm_enabled() || pkvm_guest_uses_firmware()) {
        return;
    }

    x86_pkvm_configure_ich9_lpc_pm_base();
    acpi_build_tables_init(&tables);
    acpi_build_direct(&tables, machine);
    x86_pkvm_write_direct_acpi_tables(&tables, true);
    acpi_build_tables_cleanup(&tables, true);
    x86_pkvm_share_direct_boot_low_memory();
}

static uint64_t x86_pkvm_gdt_entry(uint16_t flags, uint32_t base, uint32_t limit)
{
    return (((uint64_t)base & 0xff000000ULL) << (56 - 24)) |
           (((uint64_t)flags & 0x0000f0ffULL) << 40) |
           (((uint64_t)limit & 0x000f0000ULL) << (48 - 16)) |
           (((uint64_t)base & 0x00ffffffULL) << 16) |
           ((uint64_t)limit & 0x0000ffffULL);
}

static void x86_pkvm_write_gdt(bool is_64bit)
{
    uint64_t gdt[6] = {
        0,
        0,
        x86_pkvm_gdt_entry(is_64bit ? 0xa09b : 0xc09b, 0, 0xfffff),
        x86_pkvm_gdt_entry(0xc093, 0, 0xfffff),
        x86_pkvm_gdt_entry(0x808b, 0, 0xfffff),
        0,
    };
    size_t size = (is_64bit ? 6 : 5) * sizeof(gdt[0]);
    uint64_t idt = 0;

    cpu_physical_memory_write(PKVM_BOOT_GDT_ADDR, gdt, size);
    cpu_physical_memory_write(PKVM_BOOT_IDT_ADDR, &idt, sizeof(idt));
}

static void x86_pkvm_write_page_tables(void)
{
    uint64_t pml4[512] = {};
    uint64_t pdpte[512] = {};
    uint64_t pde[512];
    int i, j;

    pml4[0] = PKVM_BOOT_PDPTE_ADDR | 0x3;
    cpu_physical_memory_write(PKVM_BOOT_PML4_ADDR, pml4, sizeof(pml4));

    for (i = 0; i < 4; i++) {
        pdpte[i] = (PKVM_BOOT_PDE_ADDR + i * 0x1000) | 0x3;

        for (j = 0; j < 512; j++) {
            pde[j] = ((uint64_t)i << 30) | ((uint64_t)j << 21) | 0x83;
        }
        cpu_physical_memory_write(PKVM_BOOT_PDE_ADDR + i * 0x1000,
                                  pde, sizeof(pde));
    }

    cpu_physical_memory_write(PKVM_BOOT_PDPTE_ADDR, pdpte, sizeof(pdpte));
}

static uint8_t x86_pkvm_checksum(const void *buf, size_t size)
{
    const uint8_t *p = buf;
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < size; i++) {
        sum = sum + p[i];
    }

    return sum;
}

static void x86_pkvm_write_mptable(MachineState *machine)
{
    struct x86_pkvm_mpf_intel mpf = {
        .signature = { '_', 'M', 'P', '_' },
        .physptr = x86_pkvm_mptable_addr,
        .length = 1,
        .specification = 4,
    };
    struct x86_pkvm_mpc_table table = {
        .signature = { 'P', 'C', 'M', 'P' },
        .spec = 4,
        .lapic = X86_PKVM_APIC_DEFAULT_ADDRESS,
    };
    const char bus_type_isa[6] = { 'I', 'S', 'A', ' ', ' ', ' ' };
    const char mpc_oem[8] = { 'Q', 'E', 'M', 'U', ' ', ' ', ' ', ' ' };
    const char mpc_product_id[12] = {
        'p', 'K', 'V', 'M', '-', 'x', '8', '6', '-', 'Q', 'E', 'M'
    };
    hwaddr addr = x86_pkvm_mptable_addr + sizeof(table);
    uint8_t checksum = 0;
    uint16_t length;
    uint8_t cpus = machine->smp.cpus;
    uint8_t ioapic_id;
    int i;

    if (cpus < 1) {
        cpus = 1;
    }
    if (cpus > X86_PKVM_MPTABLE_MAX_CPUS) {
        cpus = X86_PKVM_MPTABLE_MAX_CPUS;
    }
    ioapic_id = cpus + 1;

    memcpy(table.oem, mpc_oem, sizeof(mpc_oem));
    memcpy(table.productid, mpc_product_id, sizeof(mpc_product_id));

    for (i = 0; i < cpus; i++) {
        struct x86_pkvm_mpc_cpu cpu = {
            .type = 0,
            .apicid = i,
            .apicver = X86_PKVM_APIC_VERSION,
            .cpuflag = X86_PKVM_MPC_CPU_ENABLED |
                       (i == 0 ? X86_PKVM_MPC_CPU_BOOTPROCESSOR : 0),
            .cpufeature = 0x600,
            .featureflag = 0x201,
        };

        cpu_physical_memory_write(addr, &cpu, sizeof(cpu));
        checksum += x86_pkvm_checksum(&cpu, sizeof(cpu));
        addr += sizeof(cpu);
    }

    {
        struct x86_pkvm_mpc_bus bus = {
            .type = 1,
            .busid = 0,
        };

        memcpy(bus.bustype, bus_type_isa, sizeof(bus_type_isa));
        cpu_physical_memory_write(addr, &bus, sizeof(bus));
        checksum += x86_pkvm_checksum(&bus, sizeof(bus));
        addr += sizeof(bus);
    }

    {
        struct x86_pkvm_mpc_ioapic ioapic = {
            .type = 2,
            .apicid = ioapic_id,
            .apicver = X86_PKVM_APIC_VERSION,
            .flags = X86_PKVM_MPC_APIC_USABLE,
            .apicaddr = X86_PKVM_IOAPIC_DEFAULT_ADDRESS,
        };

        cpu_physical_memory_write(addr, &ioapic, sizeof(ioapic));
        checksum += x86_pkvm_checksum(&ioapic, sizeof(ioapic));
        addr += sizeof(ioapic);
    }

    for (i = 0; i < 16; i++) {
        struct x86_pkvm_mpc_intsrc intsrc = {
            .type = 3,
            .irqtype = X86_PKVM_MP_INTSRC_TYPE_INT,
            .irqflag = X86_PKVM_MP_IRQDIR_DEFAULT,
            .srcbus = 0,
            .srcbusirq = i,
            .dstapic = ioapic_id,
            .dstirq = i,
        };

        if (i == 0) {
            intsrc.dstirq = 2;
        } else if (i == 2) {
            continue;
        }

        cpu_physical_memory_write(addr, &intsrc, sizeof(intsrc));
        checksum += x86_pkvm_checksum(&intsrc, sizeof(intsrc));
        addr += sizeof(intsrc);
    }

    {
        struct x86_pkvm_mpc_lintsrc lintsrc = {
            .type = 4,
            .irqtype = X86_PKVM_MP_LINTSRC_TYPE_EXTINT,
            .irqflag = X86_PKVM_MP_IRQDIR_DEFAULT,
            .srcbusid = 0,
            .srcbusirq = 0,
            .destapic = 0,
            .destapiclint = 0,
        };

        cpu_physical_memory_write(addr, &lintsrc, sizeof(lintsrc));
        checksum += x86_pkvm_checksum(&lintsrc, sizeof(lintsrc));
        addr += sizeof(lintsrc);

        lintsrc.irqtype = X86_PKVM_MP_LINTSRC_TYPE_NMI;
        lintsrc.destapic = 0xff;
        lintsrc.destapiclint = 1;
        cpu_physical_memory_write(addr, &lintsrc, sizeof(lintsrc));
        checksum += x86_pkvm_checksum(&lintsrc, sizeof(lintsrc));
        addr += sizeof(lintsrc);
    }

    length = addr - x86_pkvm_mptable_addr;
    table.length = length;
    checksum += x86_pkvm_checksum(&table, sizeof(table));
    table.checksum = 0 - checksum;
    cpu_physical_memory_write(x86_pkvm_mptable_addr, &table, sizeof(table));

    mpf.checksum = 0 - x86_pkvm_checksum(&mpf, sizeof(mpf));
    cpu_physical_memory_write(x86_pkvm_mpf_addr, &mpf, sizeof(mpf));
}

static void x86_pkvm_populate_e820(X86MachineState *x86ms, uint8_t *zero_page)
{
    MachineState *machine = MACHINE(x86ms);
    MicrovmMachineState *mms =
        object_dynamic_cast(OBJECT(machine), TYPE_MICROVM_MACHINE) ?
        MICROVM_MACHINE(machine) : NULL;
    struct e820_entry *table;
    int entries, i;

    entries = e820_get_table(&table);
    if (entries > X86_PKVM_E820_MAX_ENTRIES) {
        entries = X86_PKVM_E820_MAX_ENTRIES;
    }

    for (i = 0; i < entries; i++) {
        struct boot_e820_entry *entry =
            (struct boot_e820_entry *)(zero_page +
                                       X86_PKVM_BOOTPARAM_E820_TABLE_OFFSET +
                                       i * sizeof(struct boot_e820_entry));

        entry->addr = table[i].address;
        entry->size = table[i].length;
        entry->type = table[i].type;
    }

    if (pkvm_guest_is_direct_kernel_boot() &&
        x86_machine_is_acpi_enabled(x86ms) &&
        entries < X86_PKVM_E820_MAX_ENTRIES) {
        struct boot_e820_entry *entry =
            (struct boot_e820_entry *)(zero_page +
                                       X86_PKVM_BOOTPARAM_E820_TABLE_OFFSET +
                                       entries * sizeof(struct boot_e820_entry));

        entry->addr = X86_PKVM_ACPI_RESERVED_START;
        entry->size = X86_PKVM_ACPI_RESERVED_END -
                      X86_PKVM_ACPI_RESERVED_START + 1;
        entry->type = E820_RESERVED;
        entries++;
    }

    if (pkvm_guest_is_direct_kernel_boot() && mms &&
        mms->pcie == ON_OFF_AUTO_ON && mms->gpex.ecam.size &&
        entries < X86_PKVM_E820_MAX_ENTRIES) {
        struct boot_e820_entry *entry =
            (struct boot_e820_entry *)(zero_page +
                                       X86_PKVM_BOOTPARAM_E820_TABLE_OFFSET +
                                       entries * sizeof(struct boot_e820_entry));

        entry->addr = mms->gpex.ecam.base;
        entry->size = mms->gpex.ecam.size;
        entry->type = E820_RESERVED;
        entries++;
    }

    zero_page[X86_PKVM_BOOTPARAM_E820_COUNT_OFFSET] = entries;
}

static hwaddr x86_pkvm_initrd_addr(uint32_t initrd_max, uint64_t initrd_size)
{
    hwaddr addr = (initrd_max - initrd_size) & ~4095ULL;

    if (addr < PKVM_FW_END && addr + initrd_size > PKVM_FW_START) {
        if (initrd_size > PKVM_FW_START) {
            return HWADDR_MAX;
        }
        addr = (PKVM_FW_START - initrd_size) & ~4095ULL;
    }

    return addr;
}

static void x86_load_linux_pkvm(X86MachineState *x86ms,
                                int setup_size,
                                uint8_t *setup,
                                int kernel_size,
                                uint8_t *kernel,
                                hwaddr cmdline_addr,
                                hwaddr initrd_addr,
                                uint64_t initrd_size,
                                hwaddr kernel_entry,
                                hwaddr dtb_addr,
                                bool is_64bit)
{
    MachineState *machine = MACHINE(x86ms);
    uint8_t zero_page[4096] = {};
    struct x86_pkvm_setup_header *hdr;
    hwaddr kernel_load_addr = PKVM_BZIMAGE_LOAD_ADDR - setup_size;
    size_t hdr_copy_len;
    size_t hdr_end;
    Error *local_err = NULL;

    hdr = (struct x86_pkvm_setup_header *)(zero_page + X86_PKVM_BOOTPARAM_HDR_OFFSET);
    hdr_end = (setup_size > 0x201) ? (0x202 + setup[0x201]) : X86_PKVM_BOOTPARAM_HDR_OFFSET;
    hdr_end = MIN(hdr_end, (size_t)setup_size);
    hdr_copy_len = hdr_end > X86_PKVM_BOOTPARAM_HDR_OFFSET ?
                   MIN(sizeof(*hdr), hdr_end - X86_PKVM_BOOTPARAM_HDR_OFFSET) : 0;
    if (hdr_copy_len) {
        memcpy(hdr, setup + X86_PKVM_BOOTPARAM_HDR_OFFSET, hdr_copy_len);
    }

    hdr->type_of_loader = 0xff;
    hdr->boot_flag = 0xaa55;
    hdr->header = 0x53726448;
    hdr->cmd_line_ptr = cmdline_addr;
    hdr->kernel_alignment = 0x1000000;
    stl_le_p(zero_page + X86_PKVM_BOOTPARAM_EXT_CMD_LINE_PTR_OFFSET,
             cmdline_addr >> 32);
    zero_page[X86_PKVM_BOOTPARAM_SENTINEL_OFFSET] = 0;
    if (initrd_size) {
        hdr->ramdisk_image = initrd_addr;
        hdr->ramdisk_size = initrd_size;
        stl_le_p(zero_page + X86_PKVM_BOOTPARAM_EXT_RAMDISK_IMAGE_OFFSET,
                 initrd_addr >> 32);
        stl_le_p(zero_page + X86_PKVM_BOOTPARAM_EXT_RAMDISK_SIZE_OFFSET, 0);
    }
    if (dtb_addr) {
        hdr->setup_data = dtb_addr - sizeof(struct setup_data);
    }

    x86_pkvm_populate_e820(x86ms, zero_page);
    x86_pkvm_write_gdt(is_64bit);
    x86_pkvm_write_mptable(machine);
    if (is_64bit) {
        x86_pkvm_write_page_tables();
    }

    cpu_physical_memory_write(x86_pkvm_zero_page_addr, zero_page, sizeof(zero_page));
    cpu_physical_memory_write(cmdline_addr, machine->kernel_cmdline,
                              strlen(machine->kernel_cmdline) + 1);
    cpu_physical_memory_write(kernel_load_addr, kernel, kernel_size);

    if (pkvm_guest_uses_firmware()) {
        if (pkvm_guest_set_fw_gpa(PKVM_FW_START, &local_err) < 0) {
            error_report_err(local_err);
            exit(1);
        }
        pkvm_guest_set_fw_boot_state(kernel_entry, dtb_addr);
    } else {
        pkvm_guest_set_kernel_boot_state(kernel_entry, x86_pkvm_zero_page_addr,
                                         x86_pkvm_boot_stack_pointer,
                                         is_64bit);
    }
}

static void x86_cpu_new(X86MachineState *x86ms, int64_t apic_id, Error **errp)
{
    Object *cpu = object_new(MACHINE(x86ms)->cpu_type);

    if (!object_property_set_uint(cpu, "apic-id", apic_id, errp)) {
        goto out;
    }
    qdev_realize(DEVICE(cpu), NULL, errp);

out:
    object_unref(cpu);
}

void x86_cpus_init(X86MachineState *x86ms, int default_cpu_version)
{
    int i;
    const CPUArchIdList *possible_cpus;
    MachineState *ms = MACHINE(x86ms);
    MachineClass *mc = MACHINE_GET_CLASS(x86ms);

    x86_cpu_set_default_version(default_cpu_version);

    /*
     * Calculates the limit to CPU APIC ID values
     *
     * Limit for the APIC ID value, so that all
     * CPU APIC IDs are < x86ms->apic_id_limit.
     *
     * This is used for FW_CFG_MAX_CPUS. See comments on fw_cfg_arch_create().
     */
    x86ms->apic_id_limit = x86_cpu_apic_id_from_index(x86ms,
                                                      ms->smp.max_cpus - 1) + 1;

    /*
     * Can we support APIC ID 255 or higher?  With KVM, that requires
     * both in-kernel lapic and X2APIC userspace API.
     *
     * kvm_enabled() must go first to ensure that kvm_* references are
     * not emitted for the linker to consume (kvm_enabled() is
     * a literal `0` in configurations where kvm_* aren't defined)
     */
    if (kvm_enabled() && x86ms->apic_id_limit > 255 &&
        kvm_irqchip_in_kernel() && !kvm_enable_x2apic()) {
        error_report("current -smp configuration requires kernel "
                     "irqchip and X2APIC API support.");
        exit(EXIT_FAILURE);
    }

    if (kvm_enabled()) {
        kvm_set_max_apic_id(x86ms->apic_id_limit);
    }

    if (!kvm_irqchip_in_kernel()) {
        apic_set_max_apic_id(x86ms->apic_id_limit);
    }

    possible_cpus = mc->possible_cpu_arch_ids(ms);
    for (i = 0; i < ms->smp.cpus; i++) {
        x86_cpu_new(x86ms, possible_cpus->cpus[i].arch_id, &error_fatal);
    }
}

void x86_rtc_set_cpus_count(ISADevice *s, uint16_t cpus_count)
{
    MC146818RtcState *rtc = MC146818_RTC(s);

    if (cpus_count > 0xff) {
        /*
         * If the number of CPUs can't be represented in 8 bits, the
         * BIOS must use "FW_CFG_NB_CPUS". Set RTC field to 0 just
         * to make old BIOSes fail more predictably.
         */
        mc146818rtc_set_cmos_data(rtc, 0x5f, 0);
    } else {
        mc146818rtc_set_cmos_data(rtc, 0x5f, cpus_count - 1);
    }
}

static int x86_apic_cmp(const void *a, const void *b)
{
   CPUArchId *apic_a = (CPUArchId *)a;
   CPUArchId *apic_b = (CPUArchId *)b;

   return apic_a->arch_id - apic_b->arch_id;
}

/*
 * returns pointer to CPUArchId descriptor that matches CPU's apic_id
 * in ms->possible_cpus->cpus, if ms->possible_cpus->cpus has no
 * entry corresponding to CPU's apic_id returns NULL.
 */
static CPUArchId *x86_find_cpu_slot(MachineState *ms, uint32_t id, int *idx)
{
    CPUArchId apic_id, *found_cpu;

    apic_id.arch_id = id;
    found_cpu = bsearch(&apic_id, ms->possible_cpus->cpus,
        ms->possible_cpus->len, sizeof(*ms->possible_cpus->cpus),
        x86_apic_cmp);
    if (found_cpu && idx) {
        *idx = found_cpu - ms->possible_cpus->cpus;
    }
    return found_cpu;
}

void x86_cpu_plug(HotplugHandler *hotplug_dev,
                  DeviceState *dev, Error **errp)
{
    CPUArchId *found_cpu;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    X86MachineState *x86ms = X86_MACHINE(hotplug_dev);

    if (x86ms->acpi_dev) {
        hotplug_handler_plug(x86ms->acpi_dev, dev, &local_err);
        if (local_err) {
            goto out;
        }
    }

    /* increment the number of CPUs */
    x86ms->boot_cpus++;
    if (x86ms->rtc) {
        x86_rtc_set_cpus_count(x86ms->rtc, x86ms->boot_cpus);
    }
    if (x86ms->fw_cfg) {
        fw_cfg_modify_i16(x86ms->fw_cfg, FW_CFG_NB_CPUS, x86ms->boot_cpus);
    }

    /*
     * Non-hotplugged CPUs get their SMM cpu address space initialized in
     * machine init done notifier: register_smram_listener().
     *
     * We need initialize the SMM cpu address space for the hotplugged CPU
     * specifically.
     */
    if (kvm_enabled() && dev->hotplugged && x86_machine_is_smm_enabled(x86ms)) {
        kvm_smm_cpu_address_space_init(cpu);
    }

    found_cpu = x86_find_cpu_slot(MACHINE(x86ms), cpu->apic_id, NULL);
    found_cpu->cpu = CPU(dev);
out:
    error_propagate(errp, local_err);
}

void x86_cpu_unplug_request_cb(HotplugHandler *hotplug_dev,
                               DeviceState *dev, Error **errp)
{
    int idx = -1;
    X86CPU *cpu = X86_CPU(dev);
    X86MachineState *x86ms = X86_MACHINE(hotplug_dev);

    if (!x86ms->acpi_dev) {
        error_setg(errp, "CPU hot unplug not supported without ACPI");
        return;
    }

    x86_find_cpu_slot(MACHINE(x86ms), cpu->apic_id, &idx);
    assert(idx != -1);
    if (idx == 0) {
        error_setg(errp, "Boot CPU is unpluggable");
        return;
    }

    hotplug_handler_unplug_request(x86ms->acpi_dev, dev,
                                   errp);
}

void x86_cpu_unplug_cb(HotplugHandler *hotplug_dev,
                       DeviceState *dev, Error **errp)
{
    CPUArchId *found_cpu;
    Error *local_err = NULL;
    X86CPU *cpu = X86_CPU(dev);
    X86MachineState *x86ms = X86_MACHINE(hotplug_dev);

    hotplug_handler_unplug(x86ms->acpi_dev, dev, &local_err);
    if (local_err) {
        goto out;
    }

    found_cpu = x86_find_cpu_slot(MACHINE(x86ms), cpu->apic_id, NULL);
    found_cpu->cpu = NULL;
    qdev_unrealize(dev);

    /* decrement the number of CPUs */
    x86ms->boot_cpus--;
    /* Update the number of CPUs in CMOS */
    x86_rtc_set_cpus_count(x86ms->rtc, x86ms->boot_cpus);
    fw_cfg_modify_i16(x86ms->fw_cfg, FW_CFG_NB_CPUS, x86ms->boot_cpus);
 out:
    error_propagate(errp, local_err);
}

void x86_cpu_pre_plug(HotplugHandler *hotplug_dev,
                      DeviceState *dev, Error **errp)
{
    int idx;
    CPUState *cs;
    CPUArchId *cpu_slot;
    X86CPUTopoIDs topo_ids;
    X86CPU *cpu = X86_CPU(dev);
    CPUX86State *env = &cpu->env;
    MachineState *ms = MACHINE(hotplug_dev);
    X86MachineState *x86ms = X86_MACHINE(hotplug_dev);
    X86CPUTopoInfo *topo_info = &env->topo_info;

    if (!object_dynamic_cast(OBJECT(cpu), ms->cpu_type)) {
        error_setg(errp, "Invalid CPU type, expected cpu type: '%s'",
                   ms->cpu_type);
        return;
    }

    if (x86ms->acpi_dev) {
        Error *local_err = NULL;

        hotplug_handler_pre_plug(HOTPLUG_HANDLER(x86ms->acpi_dev), dev,
                                 &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return;
        }
    }

    init_topo_info(topo_info, x86ms);

    if (ms->smp.modules > 1) {
        set_bit(CPU_TOPOLOGY_LEVEL_MODULE, env->avail_cpu_topo);
    }

    if (ms->smp.dies > 1) {
        set_bit(CPU_TOPOLOGY_LEVEL_DIE, env->avail_cpu_topo);
    }

    /*
     * If APIC ID is not set,
     * set it based on socket/die/module/core/thread properties.
     */
    if (cpu->apic_id == UNASSIGNED_APIC_ID) {
        /*
         * die-id was optional in QEMU 4.0 and older, so keep it optional
         * if there's only one die per socket.
         */
        if (cpu->die_id < 0 && ms->smp.dies == 1) {
            cpu->die_id = 0;
        }

        /*
         * module-id was optional in QEMU 9.0 and older, so keep it optional
         * if there's only one module per die.
         */
        if (cpu->module_id < 0 && ms->smp.modules == 1) {
            cpu->module_id = 0;
        }

        if (cpu->socket_id < 0) {
            error_setg(errp, "CPU socket-id is not set");
            return;
        } else if (cpu->socket_id > ms->smp.sockets - 1) {
            error_setg(errp, "Invalid CPU socket-id: %u must be in range 0:%u",
                       cpu->socket_id, ms->smp.sockets - 1);
            return;
        }
        if (cpu->die_id < 0) {
            error_setg(errp, "CPU die-id is not set");
            return;
        } else if (cpu->die_id > ms->smp.dies - 1) {
            error_setg(errp, "Invalid CPU die-id: %u must be in range 0:%u",
                       cpu->die_id, ms->smp.dies - 1);
            return;
        }
        if (cpu->module_id < 0) {
            error_setg(errp, "CPU module-id is not set");
            return;
        } else if (cpu->module_id > ms->smp.modules - 1) {
            error_setg(errp, "Invalid CPU module-id: %u must be in range 0:%u",
                       cpu->module_id, ms->smp.modules - 1);
            return;
        }
        if (cpu->core_id < 0) {
            error_setg(errp, "CPU core-id is not set");
            return;
        } else if (cpu->core_id > (ms->smp.cores - 1)) {
            error_setg(errp, "Invalid CPU core-id: %u must be in range 0:%u",
                       cpu->core_id, ms->smp.cores - 1);
            return;
        }
        if (cpu->thread_id < 0) {
            error_setg(errp, "CPU thread-id is not set");
            return;
        } else if (cpu->thread_id > (ms->smp.threads - 1)) {
            error_setg(errp, "Invalid CPU thread-id: %u must be in range 0:%u",
                       cpu->thread_id, ms->smp.threads - 1);
            return;
        }

        topo_ids.pkg_id = cpu->socket_id;
        topo_ids.die_id = cpu->die_id;
        topo_ids.module_id = cpu->module_id;
        topo_ids.core_id = cpu->core_id;
        topo_ids.smt_id = cpu->thread_id;
        cpu->apic_id = x86_apicid_from_topo_ids(topo_info, &topo_ids);
    }

    cpu_slot = x86_find_cpu_slot(MACHINE(x86ms), cpu->apic_id, &idx);
    if (!cpu_slot) {
        x86_topo_ids_from_apicid(cpu->apic_id, topo_info, &topo_ids);

        error_setg(errp,
            "Invalid CPU [socket: %u, die: %u, module: %u, core: %u, thread: %u]"
            " with APIC ID %" PRIu32 ", valid index range 0:%d",
            topo_ids.pkg_id, topo_ids.die_id, topo_ids.module_id,
            topo_ids.core_id, topo_ids.smt_id, cpu->apic_id,
            ms->possible_cpus->len - 1);
        return;
    }

    if (cpu_slot->cpu) {
        error_setg(errp, "CPU[%d] with APIC ID %" PRIu32 " exists",
                   idx, cpu->apic_id);
        return;
    }

    /* if 'address' properties socket-id/core-id/thread-id are not set, set them
     * so that machine_query_hotpluggable_cpus would show correct values
     */
    /* TODO: move socket_id/core_id/thread_id checks into x86_cpu_realizefn()
     * once -smp refactoring is complete and there will be CPU private
     * CPUState::nr_cores and CPUState::nr_threads fields instead of globals */
    x86_topo_ids_from_apicid(cpu->apic_id, topo_info, &topo_ids);
    if (cpu->socket_id != -1 && cpu->socket_id != topo_ids.pkg_id) {
        error_setg(errp, "property socket-id: %u doesn't match set apic-id:"
            " 0x%x (socket-id: %u)", cpu->socket_id, cpu->apic_id,
            topo_ids.pkg_id);
        return;
    }
    cpu->socket_id = topo_ids.pkg_id;

    if (cpu->die_id != -1 && cpu->die_id != topo_ids.die_id) {
        error_setg(errp, "property die-id: %u doesn't match set apic-id:"
            " 0x%x (die-id: %u)", cpu->die_id, cpu->apic_id, topo_ids.die_id);
        return;
    }
    cpu->die_id = topo_ids.die_id;

    if (cpu->module_id != -1 && cpu->module_id != topo_ids.module_id) {
        error_setg(errp, "property module-id: %u doesn't match set apic-id:"
            " 0x%x (module-id: %u)", cpu->module_id, cpu->apic_id,
            topo_ids.module_id);
        return;
    }
    cpu->module_id = topo_ids.module_id;

    if (cpu->core_id != -1 && cpu->core_id != topo_ids.core_id) {
        error_setg(errp, "property core-id: %u doesn't match set apic-id:"
            " 0x%x (core-id: %u)", cpu->core_id, cpu->apic_id,
            topo_ids.core_id);
        return;
    }
    cpu->core_id = topo_ids.core_id;

    if (cpu->thread_id != -1 && cpu->thread_id != topo_ids.smt_id) {
        error_setg(errp, "property thread-id: %u doesn't match set apic-id:"
            " 0x%x (thread-id: %u)", cpu->thread_id, cpu->apic_id,
            topo_ids.smt_id);
        return;
    }
    cpu->thread_id = topo_ids.smt_id;

    /*
    * kvm_enabled() must go first to ensure that kvm_* references are
    * not emitted for the linker to consume (kvm_enabled() is
    * a literal `0` in configurations where kvm_* aren't defined)
    */
    if (kvm_enabled() && hyperv_feat_enabled(cpu, HYPERV_FEAT_VPINDEX) &&
        !kvm_hv_vpindex_settable()) {
        error_setg(errp, "kernel doesn't allow setting HyperV VP_INDEX");
        return;
    }

    cs = CPU(cpu);
    cs->cpu_index = idx;

    numa_cpu_pre_plug(cpu_slot, dev, errp);
}

static long get_file_size(FILE *f)
{
    long where, size;

    /* XXX: on Unix systems, using fstat() probably makes more sense */

    where = ftell(f);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, where, SEEK_SET);

    return size;
}

void gsi_handler(void *opaque, int n, int level)
{
    GSIState *s = opaque;
    bool bypass_ioapic = false;

    trace_x86_gsi_interrupt(n, level);

#ifdef CONFIG_XEN_EMU
    /*
     * Xen delivers the GSI to the Legacy PIC (not that Legacy PIC
     * routing actually works properly under Xen). And then to
     * *either* the PIRQ handling or the I/OAPIC depending on whether
     * the former wants it.
     *
     * Additionally, this hook allows the Xen event channel GSI to
     * work around QEMU's lack of support for shared level interrupts,
     * by keeping track of the externally driven state of the pin and
     * implementing a logical OR with the state of the evtchn GSI.
     */
    if (xen_mode == XEN_EMULATE) {
        bypass_ioapic = xen_evtchn_set_gsi(n, &level);
    }
#endif

    switch (n) {
    case 0 ... ISA_NUM_IRQS - 1:
        if (s->i8259_irq[n]) {
            /* Under KVM, Kernel will forward to both PIC and IOAPIC */
            qemu_set_irq(s->i8259_irq[n], level);
        }
        /* fall through */
    case ISA_NUM_IRQS ... IOAPIC_NUM_PINS - 1:
        if (!bypass_ioapic) {
            qemu_set_irq(s->ioapic_irq[n], level);
        }
        break;
    case IO_APIC_SECONDARY_IRQBASE
        ... IO_APIC_SECONDARY_IRQBASE + IOAPIC_NUM_PINS - 1:
        qemu_set_irq(s->ioapic2_irq[n - IO_APIC_SECONDARY_IRQBASE], level);
        break;
    }
}

void ioapic_init_gsi(GSIState *gsi_state, Object *parent)
{
    DeviceState *dev;
    SysBusDevice *d;
    unsigned int i;

    assert(parent);
    if (kvm_ioapic_in_kernel()) {
        dev = qdev_new(TYPE_KVM_IOAPIC);
    } else {
        dev = qdev_new(TYPE_IOAPIC);
    }
    object_property_add_child(parent, "ioapic", OBJECT(dev));
    d = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(d, &error_fatal);
    sysbus_mmio_map(d, 0, IO_APIC_DEFAULT_ADDRESS);

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        gsi_state->ioapic_irq[i] = qdev_get_gpio_in(dev, i);
    }
}

DeviceState *ioapic_init_secondary(GSIState *gsi_state)
{
    DeviceState *dev;
    SysBusDevice *d;
    unsigned int i;

    dev = qdev_new(TYPE_IOAPIC);
    d = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(d, &error_fatal);
    sysbus_mmio_map(d, 0, IO_APIC_SECONDARY_ADDRESS);

    for (i = 0; i < IOAPIC_NUM_PINS; i++) {
        gsi_state->ioapic2_irq[i] = qdev_get_gpio_in(dev, i);
    }
    return dev;
}

/*
 * The entry point into the kernel for PVH boot is different from
 * the native entry point.  The PVH entry is defined by the x86/HVM
 * direct boot ABI and is available in an ELFNOTE in the kernel binary.
 *
 * This function is passed to load_elf() when it is called from
 * load_elfboot() which then additionally checks for an ELF Note of
 * type XEN_ELFNOTE_PHYS32_ENTRY and passes it to this function to
 * parse the PVH entry address from the ELF Note.
 *
 * Due to trickery in elf_opts.h, load_elf() is actually available as
 * load_elf32() or load_elf64() and this routine needs to be able
 * to deal with being called as 32 or 64 bit.
 *
 * The address of the PVH entry point is saved to the 'pvh_start_addr'
 * global variable.  (although the entry point is 32-bit, the kernel
 * binary can be either 32-bit or 64-bit).
 */
static uint64_t read_pvh_start_addr(void *arg1, void *arg2, bool is64)
{
    size_t *elf_note_data_addr;

    /* Check if ELF Note header passed in is valid */
    if (arg1 == NULL) {
        return 0;
    }

    if (is64) {
        struct elf64_note *nhdr64 = (struct elf64_note *)arg1;
        uint64_t nhdr_size64 = sizeof(struct elf64_note);
        uint64_t phdr_align = *(uint64_t *)arg2;
        uint64_t nhdr_namesz = nhdr64->n_namesz;

        elf_note_data_addr =
            ((void *)nhdr64) + nhdr_size64 +
            QEMU_ALIGN_UP(nhdr_namesz, phdr_align);

        pvh_start_addr = *elf_note_data_addr;
    } else {
        struct elf32_note *nhdr32 = (struct elf32_note *)arg1;
        uint32_t nhdr_size32 = sizeof(struct elf32_note);
        uint32_t phdr_align = *(uint32_t *)arg2;
        uint32_t nhdr_namesz = nhdr32->n_namesz;

        elf_note_data_addr =
            ((void *)nhdr32) + nhdr_size32 +
            QEMU_ALIGN_UP(nhdr_namesz, phdr_align);

        pvh_start_addr = *(uint32_t *)elf_note_data_addr;
    }

    return pvh_start_addr;
}

static bool load_elfboot(const char *kernel_filename,
                         int kernel_file_size,
                         uint8_t *header,
                         size_t pvh_xen_start_addr,
                         FWCfgState *fw_cfg)
{
    uint32_t flags = 0;
    uint32_t mh_load_addr = 0;
    uint32_t elf_kernel_size = 0;
    uint64_t elf_entry;
    uint64_t elf_low, elf_high;
    int kernel_size;

    if (ldl_le_p(header) != 0x464c457f) {
        return false; /* no elfboot */
    }

    bool elf_is64 = header[EI_CLASS] == ELFCLASS64;
    flags = elf_is64 ?
        ((Elf64_Ehdr *)header)->e_flags : ((Elf32_Ehdr *)header)->e_flags;

    if (flags & 0x00010004) { /* LOAD_ELF_HEADER_HAS_ADDR */
        error_report("elfboot unsupported flags = %x", flags);
        exit(1);
    }

    uint64_t elf_note_type = XEN_ELFNOTE_PHYS32_ENTRY;
    kernel_size = load_elf(kernel_filename, read_pvh_start_addr,
                           NULL, &elf_note_type, &elf_entry,
                           &elf_low, &elf_high, NULL,
                           ELFDATA2LSB, I386_ELF_MACHINE, 0, 0);

    if (kernel_size < 0) {
        error_report("Error while loading elf kernel");
        exit(1);
    }
    mh_load_addr = elf_low;
    elf_kernel_size = elf_high - elf_low;

    if (pvh_start_addr == 0) {
        error_report("Error loading uncompressed kernel without PVH ELF Note");
        exit(1);
    }
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ENTRY, pvh_start_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, mh_load_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, elf_kernel_size);

    return true;
}

void x86_load_linux(X86MachineState *x86ms,
                    FWCfgState *fw_cfg,
                    int acpi_data_size,
                    bool pvh_enabled)
{
    bool linuxboot_dma_enabled = X86_MACHINE_GET_CLASS(x86ms)->fwcfg_dma_enabled;
    uint16_t protocol;
    int setup_size, kernel_size, cmdline_size;
    int dtb_size, setup_data_offset;
    uint32_t initrd_max;
    uint8_t header[8192], *setup, *kernel;
    hwaddr real_addr, prot_addr, cmdline_addr, initrd_addr = 0;
    uint64_t initrd_size_total = 0;
    FILE *f;
    const char *vmode;
    MachineState *machine = MACHINE(x86ms);
    struct setup_data *setup_data;
    const char *kernel_filename = machine->kernel_filename;
    const char *initrd_filename = machine->initrd_filename;
    const char *dtb_filename = machine->dtb;
    const char *kernel_cmdline = machine->kernel_cmdline;
    SevKernelLoaderContext sev_load_ctx = {};
    bool pkvm = pkvm_enabled();
    hwaddr kernel_entry = 0;
    hwaddr dtb_addr = 0;
    bool kernel_is_64bit = false;

    /* Align to 16 bytes as a paranoia measure */
    cmdline_size = (strlen(kernel_cmdline) + 16) & ~15;

    /* load the kernel header */
    f = fopen(kernel_filename, "rb");
    if (!f) {
        fprintf(stderr, "qemu: could not open kernel file '%s': %s\n",
                kernel_filename, strerror(errno));
        exit(1);
    }

    kernel_size = get_file_size(f);
    if (!kernel_size ||
        fread(header, 1, MIN(ARRAY_SIZE(header), kernel_size), f) !=
        MIN(ARRAY_SIZE(header), kernel_size)) {
        fprintf(stderr, "qemu: could not load kernel '%s': %s\n",
                kernel_filename, strerror(errno));
        exit(1);
    }

    /*
     * kernel protocol version.
     * Please see https://www.kernel.org/doc/Documentation/x86/boot.txt
     */
    if (ldl_le_p(header + 0x202) == 0x53726448) /* Magic signature "HdrS" */ {
        protocol = lduw_le_p(header + 0x206);
    } else {
        if (pkvm) {
            error_report("pKVM direct boot requires a Linux bzImage kernel");
            exit(1);
        }
        /*
         * This could be a multiboot kernel. If it is, let's stop treating it
         * like a Linux kernel.
         * Note: some multiboot images could be in the ELF format (the same of
         * PVH), so we try multiboot first since we check the multiboot magic
         * header before to load it.
         */
        if (load_multiboot(x86ms, fw_cfg, f, kernel_filename, initrd_filename,
                           kernel_cmdline, kernel_size, header)) {
            return;
        }
        /*
         * Check if the file is an uncompressed kernel file (ELF) and load it,
         * saving the PVH entry point used by the x86/HVM direct boot ABI.
         * If load_elfboot() is successful, populate the fw_cfg info.
         */
        if (pvh_enabled &&
            load_elfboot(kernel_filename, kernel_size,
                         header, pvh_start_addr, fw_cfg)) {
            fclose(f);

            fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE,
                strlen(kernel_cmdline) + 1);
            fw_cfg_add_string(fw_cfg, FW_CFG_CMDLINE_DATA, kernel_cmdline);

            setup = g_memdup2(header, sizeof(header));

            fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, sizeof(header));
            fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA,
                             setup, sizeof(header));

            /* load initrd */
            if (initrd_filename) {
                GMappedFile *mapped_file;
                gsize initrd_size;
                gchar *initrd_data;
                GError *gerr = NULL;

                mapped_file = g_mapped_file_new(initrd_filename, false, &gerr);
                if (!mapped_file) {
                    fprintf(stderr, "qemu: error reading initrd %s: %s\n",
                            initrd_filename, gerr->message);
                    exit(1);
                }
                x86ms->initrd_mapped_file = mapped_file;

                initrd_data = g_mapped_file_get_contents(mapped_file);
                initrd_size = g_mapped_file_get_length(mapped_file);
                initrd_max = x86ms->below_4g_mem_size - acpi_data_size - 1;
                if (initrd_size >= initrd_max) {
                    fprintf(stderr, "qemu: initrd is too large, cannot support."
                            "(max: %"PRIu32", need %"PRId64")\n",
                            initrd_max, (uint64_t)initrd_size);
                    exit(1);
                }

                initrd_addr = (initrd_max - initrd_size) & ~4095;

                fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
                fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
                fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data,
                                 initrd_size);
            }

            option_rom[nb_option_roms].bootindex = 0;
            option_rom[nb_option_roms].name = "pvh.bin";
            nb_option_roms++;

            return;
        }
        protocol = 0;
    }

    if (protocol < 0x200 || !(header[0x211] & 0x01)) {
        /* Low kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x10000;
    } else if (protocol < 0x202) {
        /* High but ancient kernel */
        real_addr    = 0x90000;
        cmdline_addr = 0x9a000 - cmdline_size;
        prot_addr    = 0x100000;
    } else {
        /* High and recent kernel */
        real_addr    = 0x10000;
        cmdline_addr = 0x20000;
        prot_addr    = 0x100000;
    }

    if (pkvm) {
        prot_addr = PKVM_BZIMAGE_LOAD_ADDR;
    }

    /* highest address for loading the initrd */
    if (protocol >= 0x20c &&
        lduw_le_p(header + 0x236) & XLF_CAN_BE_LOADED_ABOVE_4G) {
        /*
         * Linux has supported initrd up to 4 GB for a very long time (2007,
         * long before XLF_CAN_BE_LOADED_ABOVE_4G which was added in 2013),
         * though it only sets initrd_max to 2 GB to "work around bootloader
         * bugs". Luckily, QEMU firmware(which does something like bootloader)
         * has supported this.
         *
         * It's believed that if XLF_CAN_BE_LOADED_ABOVE_4G is set, initrd can
         * be loaded into any address.
         *
         * In addition, initrd_max is uint32_t simply because QEMU doesn't
         * support the 64-bit boot protocol (specifically the ext_ramdisk_image
         * field).
         *
         * Therefore here just limit initrd_max to UINT32_MAX simply as well.
         */
        initrd_max = UINT32_MAX;
    } else if (protocol >= 0x203) {
        initrd_max = ldl_le_p(header + 0x22c);
    } else {
        initrd_max = 0x37ffffff;
    }

    if (initrd_max >= x86ms->below_4g_mem_size - acpi_data_size) {
        initrd_max = x86ms->below_4g_mem_size - acpi_data_size - 1;
    }

    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_ADDR, cmdline_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_CMDLINE_SIZE, strlen(kernel_cmdline) + 1);
    fw_cfg_add_string(fw_cfg, FW_CFG_CMDLINE_DATA, kernel_cmdline);
    sev_load_ctx.cmdline_data = (char *)kernel_cmdline;
    sev_load_ctx.cmdline_size = strlen(kernel_cmdline) + 1;

    if (protocol >= 0x202) {
        stl_le_p(header + 0x228, cmdline_addr);
    } else {
        stw_le_p(header + 0x20, 0xA33F);
        stw_le_p(header + 0x22, cmdline_addr - real_addr);
    }

    /* handle vga= parameter */
    vmode = strstr(kernel_cmdline, "vga=");
    if (vmode) {
        unsigned int video_mode;
        const char *end;
        int ret;
        /* skip "vga=" */
        vmode += 4;
        if (!strncmp(vmode, "normal", 6)) {
            video_mode = 0xffff;
        } else if (!strncmp(vmode, "ext", 3)) {
            video_mode = 0xfffe;
        } else if (!strncmp(vmode, "ask", 3)) {
            video_mode = 0xfffd;
        } else {
            ret = qemu_strtoui(vmode, &end, 0, &video_mode);
            if (ret != 0 || (*end && *end != ' ')) {
                fprintf(stderr, "qemu: invalid 'vga=' kernel parameter.\n");
                exit(1);
            }
        }
        stw_le_p(header + 0x1fa, video_mode);
    }

    /* loader type */
    /*
     * High nybble = B reserved for QEMU; low nybble is revision number.
     * If this code is substantially changed, you may want to consider
     * incrementing the revision.
     */
    if (protocol >= 0x200) {
        header[0x210] = 0xB0;
    }
    /* heap */
    if (protocol >= 0x201) {
        header[0x211] |= 0x80; /* CAN_USE_HEAP */
        stw_le_p(header + 0x224, cmdline_addr - real_addr - 0x200);
    }

    /* load initrd */
    if (initrd_filename) {
        GMappedFile *mapped_file;
        gsize initrd_size;
        gchar *initrd_data;
        GError *gerr = NULL;

        if (protocol < 0x200) {
            fprintf(stderr, "qemu: linux kernel too old to load a ram disk\n");
            exit(1);
        }

        mapped_file = g_mapped_file_new(initrd_filename, false, &gerr);
        if (!mapped_file) {
            fprintf(stderr, "qemu: error reading initrd %s: %s\n",
                    initrd_filename, gerr->message);
            exit(1);
        }
        x86ms->initrd_mapped_file = mapped_file;

        initrd_data = g_mapped_file_get_contents(mapped_file);
        initrd_size = g_mapped_file_get_length(mapped_file);
        initrd_size_total = initrd_size;
        if (initrd_size >= initrd_max) {
            fprintf(stderr, "qemu: initrd is too large, cannot support."
                    "(max: %"PRIu32", need %"PRId64")\n",
                    initrd_max, (uint64_t)initrd_size);
            exit(1);
        }

        initrd_addr = pkvm ? x86_pkvm_initrd_addr(initrd_max, initrd_size) :
                             ((initrd_max - initrd_size) & ~4095ULL);
        if (initrd_addr == HWADDR_MAX) {
            fprintf(stderr, "qemu: initrd does not fit around the pKVM firmware window\n");
            exit(1);
        }

        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_ADDR, initrd_addr);
        fw_cfg_add_i32(fw_cfg, FW_CFG_INITRD_SIZE, initrd_size);
        fw_cfg_add_bytes(fw_cfg, FW_CFG_INITRD_DATA, initrd_data, initrd_size);
        sev_load_ctx.initrd_data = initrd_data;
        sev_load_ctx.initrd_size = initrd_size;

        stl_le_p(header + 0x218, initrd_addr);
        stl_le_p(header + 0x21c, initrd_size);

        if (pkvm) {
            cpu_physical_memory_write(initrd_addr, initrd_data, initrd_size);
        }
    }

    /* load kernel and setup */
    setup_size = header[0x1f1];
    if (setup_size == 0) {
        setup_size = 4;
    }
    setup_size = (setup_size + 1) * 512;
    if (setup_size > kernel_size) {
        fprintf(stderr, "qemu: invalid kernel header\n");
        exit(1);
    }

    setup  = g_malloc(setup_size);
    kernel = g_malloc(kernel_size);
    fseek(f, 0, SEEK_SET);
    if (fread(setup, 1, setup_size, f) != setup_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    fseek(f, 0, SEEK_SET);
    if (fread(kernel, 1, kernel_size, f) != kernel_size) {
        fprintf(stderr, "fread() failed\n");
        exit(1);
    }
    fclose(f);

    /* append dtb to kernel */
    if (dtb_filename) {
        if (pkvm && !machine->dtb) {
            error_report("pKVM DTB handling requires a valid dtb file");
            exit(1);
        }
        if (protocol < 0x209) {
            fprintf(stderr, "qemu: Linux kernel too old to load a dtb\n");
            exit(1);
        }

        dtb_size = get_image_size(dtb_filename);
        if (dtb_size <= 0) {
            fprintf(stderr, "qemu: error reading dtb %s: %s\n",
                    dtb_filename, strerror(errno));
            exit(1);
        }

        setup_data_offset = QEMU_ALIGN_UP(kernel_size, 16);
        kernel_size = setup_data_offset + sizeof(struct setup_data) + dtb_size;
        kernel = g_realloc(kernel, kernel_size);

        stq_le_p(header + 0x250,
                 (pkvm ? prot_addr - setup_size : prot_addr) + setup_data_offset);

        setup_data = (struct setup_data *)(kernel + setup_data_offset);
        setup_data->next = 0;
        setup_data->type = cpu_to_le32(SETUP_DTB);
        setup_data->len = cpu_to_le32(dtb_size);

        load_image_size(dtb_filename, setup_data->data, dtb_size);
        dtb_addr = (prot_addr - setup_size) + setup_data_offset +
                   sizeof(struct setup_data);
    }

    /*
     * If we're starting an encrypted VM, it will be OVMF based, which uses the
     * efi stub for booting and doesn't require any values to be placed in the
     * kernel header.  We therefore don't update the header so the hash of the
     * kernel on the other side of the fw_cfg interface matches the hash of the
     * file the user passed in.
     */
    if (!sev_enabled() && protocol > 0) {
        memcpy(setup, header, MIN(sizeof(header), setup_size));
    }

    sev_load_ctx.kernel_data = (char *)kernel + setup_size;
    sev_load_ctx.kernel_size = kernel_size - setup_size;
    sev_load_ctx.setup_data = (char *)setup;
    sev_load_ctx.setup_size = setup_size;

    kernel_is_64bit = lduw_le_p(header + 0x236) & XLF_KERNEL_64;

    if (pkvm) {
        kernel_entry = prot_addr + (kernel_is_64bit ? 0x200 : 0);
    } else if (protocol >= 0x202 && ldl_le_p(header + 0x214)) {
        kernel_entry = ldl_le_p(header + 0x214);
    } else {
        kernel_entry = prot_addr;
    }

    if (pkvm) {
        if (machine->shim_filename) {
            error_report("pKVM direct boot does not support shim");
            exit(1);
        }
        x86_load_linux_pkvm(x86ms, setup_size, setup, kernel_size,
                            kernel, cmdline_addr,
                            initrd_addr, initrd_filename ? initrd_size_total : 0,
                            kernel_entry, dtb_addr, kernel_is_64bit);
        return;
    }

    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_ADDR, prot_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_KERNEL_SIZE, kernel_size - setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_KERNEL_DATA,
                     kernel + setup_size, kernel_size - setup_size);

    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_ADDR, real_addr);
    fw_cfg_add_i32(fw_cfg, FW_CFG_SETUP_SIZE, setup_size);
    fw_cfg_add_bytes(fw_cfg, FW_CFG_SETUP_DATA, setup, setup_size);

    /* kernel without setup header patches */
    fw_cfg_add_file(fw_cfg, "etc/boot/kernel", kernel, kernel_size);

    if (machine->shim_filename) {
        GMappedFile *mapped_file;
        GError *gerr = NULL;

        mapped_file = g_mapped_file_new(machine->shim_filename, false, &gerr);
        if (!mapped_file) {
            fprintf(stderr, "qemu: error reading shim %s: %s\n",
                    machine->shim_filename, gerr->message);
            exit(1);
        }

        fw_cfg_add_file(fw_cfg, "etc/boot/shim",
                        g_mapped_file_get_contents(mapped_file),
                        g_mapped_file_get_length(mapped_file));
    }

    if (sev_enabled()) {
        sev_add_kernel_loader_hashes(&sev_load_ctx, &error_fatal);
    }

    option_rom[nb_option_roms].bootindex = 0;
    option_rom[nb_option_roms].name = "linuxboot.bin";
    if (linuxboot_dma_enabled && fw_cfg_dma_enabled(fw_cfg)) {
        option_rom[nb_option_roms].name = "linuxboot_dma.bin";
    }
    nb_option_roms++;
}

void x86_isa_bios_init(MemoryRegion *isa_bios, MemoryRegion *isa_memory,
                       MemoryRegion *bios, bool read_only)
{
    uint64_t bios_size = memory_region_size(bios);
    uint64_t isa_bios_size = MIN(bios_size, 128 * KiB);

    memory_region_init_alias(isa_bios, NULL, "isa-bios", bios,
                             bios_size - isa_bios_size, isa_bios_size);
    memory_region_add_subregion_overlap(isa_memory, 1 * MiB - isa_bios_size,
                                        isa_bios, 1);
    memory_region_set_readonly(isa_bios, read_only);
}

void x86_bios_rom_init(X86MachineState *x86ms, const char *default_firmware,
                       MemoryRegion *rom_memory, bool isapc_ram_fw)
{
    const char *bios_name;
    char *filename;
    int bios_size;
    ssize_t ret;

    /* BIOS load */
    bios_name = MACHINE(x86ms)->firmware ?: default_firmware;
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, bios_name);
    if (filename) {
        bios_size = get_image_size(filename);
    } else {
        bios_size = -1;
    }
    if (bios_size <= 0 ||
        (bios_size % 65536) != 0) {
        goto bios_error;
    }
    if (machine_require_guest_memfd(MACHINE(x86ms))) {
        memory_region_init_ram_guest_memfd(&x86ms->bios, NULL, "pc.bios",
                                           bios_size, &error_fatal);
    } else {
        memory_region_init_ram(&x86ms->bios, NULL, "pc.bios",
                               bios_size, &error_fatal);
    }
    if (sev_enabled()) {
        /*
         * The concept of a "reset" simply doesn't exist for
         * confidential computing guests, we have to destroy and
         * re-launch them instead.  So there is no need to register
         * the firmware as rom to properly re-initialize on reset.
         * Just go for a straight file load instead.
         */
        void *ptr = memory_region_get_ram_ptr(&x86ms->bios);
        load_image_size(filename, ptr, bios_size);
        x86_firmware_configure(0x100000000ULL - bios_size, ptr, bios_size);
    } else {
        memory_region_set_readonly(&x86ms->bios, !isapc_ram_fw);
        ret = rom_add_file_fixed(bios_name, (uint32_t)(-bios_size), -1);
        if (ret != 0) {
            goto bios_error;
        }
    }
    g_free(filename);

    if (!machine_require_guest_memfd(MACHINE(x86ms))) {
        /* map the last 128KB of the BIOS in ISA space */
        x86_isa_bios_init(&x86ms->isa_bios, rom_memory, &x86ms->bios,
                          !isapc_ram_fw);
    }

    /* map all the bios at the top of memory */
    memory_region_add_subregion(rom_memory,
                                (uint32_t)(-bios_size),
                                &x86ms->bios);
    return;

bios_error:
    fprintf(stderr, "qemu: could not load PC BIOS '%s'\n", bios_name);
    exit(1);
}
