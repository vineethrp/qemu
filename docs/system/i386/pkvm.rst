x86 pKVM
========

QEMU supports running x86 protected KVM (pKVM) guests with KVM on Intel
hosts that provide the x86 pKVM ABI.

The current implementation focuses on firmwareless Linux boot using the
``pkvm-microvm`` machine type and the ``pkvm-guest`` confidential guest
object.


Requirements
------------

- An Intel x86 host kernel with x86 pKVM support
- KVM enabled for QEMU
- A guest kernel image that supports running as an x86 pKVM guest
- Direct kernel boot with ``-kernel``


Running an x86 pKVM guest
-------------------------

To run an x86 pKVM guest:

1. Create a ``pkvm-guest`` object
2. Set ``confidential-guest-support`` to that object
3. Use the ``pkvm-microvm`` machine type

Example::

  $ qemu-system-x86_64 \
      -accel kvm \
      -machine pkvm-microvm,confidential-guest-support=pkvm0 \
      -object pkvm-guest,id=pkvm0 \
      -cpu host -m 2048 -smp 2 \
      -kernel bzImage \
      -append "console=ttyS0 earlycon=uart,io,0x3f8,115200 root=/dev/vda rw" \
      -serial stdio -display none \
      -drive if=none,id=hd0,file=rootfs.img,format=raw \
      -device virtio-blk-device,drive=hd0


Machine model
-------------

``pkvm-microvm`` is a pKVM-oriented x86 machine derived from ``microvm``.
It configures the guest for firmwareless x86 pKVM boot and enables the
pieces needed by the current x86 pKVM flow:

- ACPI enabled
- RTC enabled
- PCIe enabled
- ISA serial enabled
- option ROM loading disabled

The machine is intended for the current firmwareless x86 pKVM flow and
requires::

  -object pkvm-guest,...


Boot flow
---------

For the current firmwareless x86 pKVM boot flow, QEMU:

- creates the guest as an x86 pKVM protected VM
- shares the low guest memory needed for x86 Linux boot handoff
- loads the Linux kernel and boot parameters directly
- initializes the bootstrap CPU with the protected boot register state
- generates ACPI tables directly in guest RAM


Devices
-------

The current implementation supports the ``microvm`` style device model used
by ``pkvm-microvm``:

- ``virtio-mmio`` devices
- PCIe devices behind ``gpex``
- ISA serial console
- ACPI power management and interrupt delivery needed for guest boot


Limitations
-----------

- x86 pKVM support currently targets ``pkvm-microvm``
- x86 pKVM support is currently Intel-only
- the current boot flow is firmwareless Linux boot
- protected firmware handoff is not supported yet
- guest support depends on the host kernel exposing the required x86 pKVM ABI


Related documentation
---------------------

- :doc:`microvm`
- :doc:`../confidential-guest-support`
