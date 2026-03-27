x86 pKVM internals
==================

This document describes the current x86 pKVM architecture in QEMU and the
main implementation points for the current firmwareless protected boot flow.


Scope
-----

The current x86 pKVM support is centered on:

- the ``pkvm-guest`` confidential guest object for x86
- firmwareless Linux kernel boot
- the ``pkvm-microvm`` machine
- direct ACPI table placement in guest RAM
- no protected firmware handoff yet


High level flow
---------------

At a high level, QEMU performs the following steps for the current
firmwareless x86 pKVM boot flow:

1. Create the VM with the x86 pKVM KVM VM type
2. Configure the machine and guest memory layout for protected boot
3. Load the Linux kernel and zero page directly into guest memory
4. Initialize the bootstrap CPU with the required x86 protected boot state
5. Build ACPI tables in userspace and write the final tables directly into
   guest RAM
6. Start the vCPUs


Key components
--------------

``target/i386/pkvm.[ch]``
  Defines the x86 ``pkvm-guest`` object and the common x86 pKVM state used by
  the firmwareless boot flow today.

``target/i386/kvm/kvm.c``
  Selects the x86 pKVM VM type, applies x86-specific KVM setup, and aligns
  the direct boot register state with the required boot mode.

``hw/i386/pkvm-microvm.c``
  Defines the ``pkvm-microvm`` machine type and enforces the machine defaults
  used by the current x86 pKVM boot flow.

``hw/i386/microvm.c``
  Wires the machine devices, direct kernel loading order, ACPI setup, and
  PCIe host integration used by ``pkvm-microvm``.

``hw/i386/x86-common.c``
  Contains the x86 direct boot table generation, ACPI table placement,
  E820 handoff, low-memory sharing, and Linux boot parameter population used
  by the firmwareless x86 pKVM path.

``hw/i386/acpi-microvm.c``
  Builds the ``pkvm-microvm`` ACPI tables for direct boot, including FADT,
  MADT, DSDT, XSDT, and MCFG when PCIe is enabled.

``hw/i386/pkvm-acpi-pm.c``
  Implements the minimal ACPI PM device used by ``pkvm-microvm`` for the
  direct x86 pKVM ACPI path.

``hw/pci-host/gpex.c``
  Provides the PCIe host support used by ``pkvm-microvm``, including legacy
  PCI config I/O windows for guest config space access.


Why direct ACPI placement is used
---------------------------------

The normal x86 QEMU ACPI path is firmware-oriented: tables are built for
firmware consumption and finalized through the linker-loader path before the
guest firmware hands control to the OS.

For firmwareless x86 pKVM boot, QEMU bypasses that firmware stage.
Instead, it builds the final ACPI tables in userspace, applies the linker
fixups in QEMU, and writes the finished tables directly into guest memory.

This keeps the guest-visible ACPI handoff self-contained and avoids a
dependency on guest firmware for the current boot path.


Protected firmware
------------------

Protected firmware support is not implemented yet.

The current machine and guest object model already performs the boot-time
machine setup that pvmfw-style firmware would rely on, including:

- Linux boot parameter construction
- ACPI table generation and placement
- E820 handoff
- PCI and interrupt topology setup

When protected firmware support is added, the expected delta is the initial
control transfer contract: QEMU will need to load the protected firmware blob,
configure the kernel with its GPA, and enter that firmware instead of entering
the bzImage directly.


Memory layout notes
-------------------

The direct boot path uses a fixed low-memory handoff region for x86 Linux boot
artifacts such as the zero page, GDT, page tables, and MP/ACPI structures.

The ACPI RSDP and tables are placed in the legacy high BIOS area so the guest
can discover them during early boot without firmware assistance.

Low guest memory used for x86 boot handoff is shared with the host before
entering the guest, because the protected guest boot contract requires QEMU to
populate that region directly.


Interrupt and PM model
----------------------

``pkvm-microvm`` does not use the hardware-reduced ``GED`` path for the
current x86 pKVM boot flow. Instead it uses a conventional x86 ACPI PM model
with:

- an SCI interrupt
- PM1 event/control registers
- a GPE0 block
- MADT-based SMP/interrupt discovery

This matches the needs of Linux's x86 ACPI bring-up more closely than the
hardware-reduced microvm path.


PCIe integration
----------------

``pkvm-microvm`` enables PCIe and uses ``gpex`` as the host bridge. The guest
receives:

- PCI root AML in the DSDT
- an ``MCFG`` table for ECAM
- reserved E820 entries for ECAM space
- legacy config I/O at ``0xcf8``/``0xcfc`` in addition to ECAM

The legacy config I/O mapping is important because Linux still expects a
working config space access path very early during x86 boot.


Testing guidance
----------------

When changing x86 pKVM code, validate at least:

- direct boot with ``pkvm-microvm``
- serial console output through login prompt
- ACPI table discovery
- SMP bring-up
- block device discovery
- PCI enumeration when PCI devices are present

Host-side ``dmesg`` is useful when debugging pKVM failures, especially for
protected memory faults or KVM setup errors.
