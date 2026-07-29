
Cryptex (CRX) device
====================

..
   SPDX-License-Identifier: MIT

Revision history
----------------

=======  ===========================================================
Version  Changes
=======  ===========================================================
v1       Initial specification: MMIO, DMA, MSI, char-device interface
v2       Decoupled CTRL bits (DMA_IN / DMA_OUT / ENCRYPT independent)
v3       Added CRX_HW_ERR_COUNT (0x034), CRX_TEMP (0x038),
         IRQ_TEMP_HIGH (bit 2), thermal monitoring, spontaneous fault
         injection, and updated crx_stats with error/thermal fields
=======  ===========================================================

Cryptex is a fictional PCI XOR-accelerator device intended as a
driver-writing exercise.  It is simple enough to implement in a day but
exposes every major driver pattern: MMIO, RAM-backed BAR, DMA, MSI
interrupts, write-1-to-clear IRQ status, and a char-device userspace
interface with ``read``/``write``/``mmap``/``ioctl``.

Command line
------------

``-device crx``

PCI identification
------------------

==========  ======
Vendor ID   0x1DEA
Device ID   0xC77E
Revision    0x01
Class       0xFF (Unclassified)
==========  ======

Memory BARs
-----------

BAR 0 — control registers
   MMIO, 4 KB.  All accesses must be 32-bit (``size == 4``).

BAR 1 — device buffer
   RAM-backed, 64 KB.  The host can read and write it directly via MMIO
   or map it into userspace with ``mmap``.  The DMA engine and encrypt
   engine also operate on this buffer.

BAR 0 register map
------------------

All registers are 32-bit.

=======  ================  =====  =================================================
Offset   Name              R/W    Description
=======  ================  =====  =================================================
0x000    CRX_ID            R      Identity: always reads ``0xC0DEC4FE``
0x004    CRX_STATUS        R      Status bits (see below)
0x008    CRX_CONTROL       W      Control / command register (see below)
0x00C    CRX_KEY           R/W    32-bit XOR key
0x010    CRX_DMA_SRC       R/W    DMA source physical address bits [31:0]
0x014    CRX_DMA_SRC_HI    R/W    DMA source physical address bits [63:32]
0x018    CRX_DMA_DST       R/W    DMA destination physical address bits [31:0]
0x01C    CRX_DMA_DST_HI    R/W    DMA destination physical address bits [63:32]
0x020    CRX_DMA_LEN       R/W    Transfer length in bytes (max 65536)
0x024    CRX_IRQ_STATUS    R/W1C  Interrupt status — write 1 to clear a bit
0x028    CRX_IRQ_MASK      R/W    Interrupt enable mask
0x02C    CRX_PERF_COUNT    R      Operations completed since last reset
0x030    CRX_RESET         W      Write ``0xDEADBEEF`` to trigger a soft reset
0x034    CRX_HW_ERR_COUNT  R      Hardware error counter (cleared by CRX_RESET)
0x038    CRX_TEMP          R      Die temperature in Fahrenheit (live reading)
=======  ================  =====  =================================================

CRX_STATUS bits
---------------

===  ===============  ====================================================
Bit  Name             Description
===  ===============  ====================================================
0    STATUS_IDLE      Device is idle and ready for a new command
1    STATUS_BUSY      Operation in progress; ``CTRL_START`` is ignored
2    STATUS_DMA_DONE  Last operation completed successfully
3    STATUS_ERROR     Error occurred; cleared by writing to ``CRX_RESET``
===  ===============  ====================================================

CRX_CONTROL bits
----------------

Writing ``CTRL_START`` (bit 0) together with any combination of the
pipeline-enable bits launches a single operation.  The bits are
independent and can be combined freely.

===  ==============  ========================================================
Bit  Name            Description
===  ==============  ========================================================
0    CTRL_START      Trigger the operation (ignored while STATUS_BUSY is set)
1    CTRL_IRQ_ENABLE Raise an interrupt on completion
2    CTRL_DMA_IN     DMA from ``CRX_DMA_SRC`` (host) → BAR 1
3    CTRL_DMA_OUT    DMA from BAR 1 → ``CRX_DMA_DST`` (host)
4    CTRL_ENCRYPT    XOR BAR 1 ``[0 .. CRX_DMA_LEN-1]`` with ``CRX_KEY``
===  ==============  ========================================================

The three pipeline stages always execute in the order
**DMA_IN → ENCRYPT → DMA_OUT**, regardless of which bits are set.

CRX_IRQ_STATUS / CRX_IRQ_MASK bits
-----------------------------------

===  ==============  ======================================================
Bit  Name            Description
===  ==============  ======================================================
0    IRQ_DMA_DONE    Operation completed (DMA and/or encrypt finished)
1    IRQ_ERROR       Error condition (e.g. ``CRX_DMA_LEN`` > 64 KB, or
                     spontaneous hardware fault)
2    IRQ_TEMP_HIGH   Die temperature exceeded 100 °F (rising-edge only;
                     see Thermal monitoring below)
===  ==============  ======================================================

``CRX_IRQ_STATUS`` is write-1-to-clear: writing a 1 to a bit clears it.
``CRX_IRQ_MASK`` gates which status bits actually raise the interrupt line.

.. note::

   ``IRQ_DMA_DONE`` and ``IRQ_TEMP_HIGH`` are independent and can be
   asserted simultaneously.  The driver ISR must inspect all asserted bits
   and dispatch each independently — a thermal interrupt must not be
   mistaken for a DMA completion.

Operation modes
---------------

Mode 1 — PIO (programmed I/O)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The host writes plaintext directly into BAR 1 via MMIO (or via
``mmap``), then triggers an in-place encrypt with no DMA movement::

  writel(CRX_KEY, key)
  memcpy_toio(bar1, plaintext, len)
  writel(CRX_DMA_LEN, len)
  writel(CRX_CONTROL, CTRL_START | CTRL_ENCRYPT | CTRL_IRQ_ENABLE)
  /* wait for IRQ_DMA_DONE */
  memcpy_fromio(ciphertext, bar1, len)

Mode 2 — full DMA pipeline
~~~~~~~~~~~~~~~~~~~~~~~~~~~

All three stages in one shot; no intermediate polling::

  writel(CRX_KEY, key)
  writel(CRX_DMA_SRC,    lower_32(dma_src_phys))
  writel(CRX_DMA_SRC_HI, upper_32(dma_src_phys))
  writel(CRX_DMA_DST,    lower_32(dma_dst_phys))
  writel(CRX_DMA_DST_HI, upper_32(dma_dst_phys))
  writel(CRX_DMA_LEN, len)
  writel(CRX_CONTROL, CTRL_START | CTRL_DMA_IN | CTRL_ENCRYPT |
                       CTRL_DMA_OUT | CTRL_IRQ_ENABLE)
  /* wait for IRQ_DMA_DONE */

Mode 3 — decoupled DMA (driver read/write paths)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Useful when the driver char-device ``read()`` and ``write()`` syscalls
need to move data independently of encryption::

  /* write(): DMA host buffer → BAR 1, no encrypt */
  writel(CRX_DMA_SRC, ...) ; writel(CRX_DMA_SRC_HI, ...)
  writel(CRX_DMA_LEN, len)
  writel(CRX_CONTROL, CTRL_START | CTRL_DMA_IN | CTRL_IRQ_ENABLE)

  /* ioctl(ENCRYPT): in-place XOR only */
  writel(CRX_KEY, key)
  writel(CRX_DMA_LEN, len)
  writel(CRX_CONTROL, CTRL_START | CTRL_ENCRYPT | CTRL_IRQ_ENABLE)

  /* read(): DMA BAR 1 → host buffer, no encrypt */
  writel(CRX_DMA_DST, ...) ; writel(CRX_DMA_DST_HI, ...)
  writel(CRX_DMA_LEN, len)
  writel(CRX_CONTROL, CTRL_START | CTRL_DMA_OUT | CTRL_IRQ_ENABLE)

Interrupt behaviour
-------------------

The device supports both MSI and INTx.  MSI is preferred; the driver
should call ``pci_alloc_irq_vectors`` and fall back to INTx if MSI is
unavailable.

When an interrupt condition occurs and the corresponding bit in
``CRX_IRQ_MASK`` is set, the device:

* asserts the MSI vector (or the INTx line if MSI is not enabled), and
* sets the corresponding bit in ``CRX_IRQ_STATUS``.

The driver must clear the interrupt in its ISR by writing 1 to the
asserted bit(s) in ``CRX_IRQ_STATUS`` (write-1-to-clear).  The driver
must write back exactly the bits it observed, not a fixed mask, so that
bits set by a concurrent event are not silently lost.  For INTx, failing
to clear the status register will cause the interrupt to re-fire
immediately.

Thermal monitoring
------------------

``CRX_TEMP`` reports the current die temperature in degrees Fahrenheit as
an unsigned 32-bit integer.  The normal operating range is **50 – 100 °F**.

When the temperature rises above 100 °F, the device asserts
``IRQ_TEMP_HIGH`` (bit 2 of ``CRX_IRQ_STATUS``).  This interrupt fires on
the **rising edge** of the threshold crossing: the device raises
``IRQ_TEMP_HIGH`` at most once per excursion above 100 °F, suppressing
further assertions until the temperature falls back to 100 °F or below
and then crosses 100 °F again.

``CRX_TEMP`` always reflects the live die temperature.  Drivers and
monitoring tools should read the register directly rather than caching the
value observed at interrupt time.

.. note::

   ``IRQ_TEMP_HIGH`` is independent of the DMA/encrypt pipeline.  It can
   fire while the device is idle, busy, or in an error state.  A high-
   temperature interrupt does not affect in-progress operations.

Spontaneous hardware faults
---------------------------

The device may assert ``IRQ_ERROR`` and set ``STATUS_ERROR`` at any time
while it is in the **IDLE** state, independently of any driver-initiated
operation.  This models transient hardware faults (e.g. ECC errors,
internal timeout) that are not caused by invalid driver input.

Each spontaneous fault increments ``CRX_HW_ERR_COUNT``.  Recovery is the
same as for any other error condition: write ``0xDEADBEEF`` to
``CRX_RESET``.

Error conditions
----------------

==========================================  ==========================================
Condition                                   Device response
==========================================  ==========================================
``CRX_DMA_LEN`` > 64 KB                    Sets STATUS_ERROR, raises IRQ_ERROR
``CTRL_START`` while STATUS_BUSY            Silently ignored, no state change
Invalid DMA physical address                Sets STATUS_ERROR (IOMMU fault)
Spontaneous hardware fault (idle state)     Sets STATUS_ERROR, raises IRQ_ERROR,
                                            increments CRX_HW_ERR_COUNT
==========================================  ==========================================

Recovery from any error: write ``0xDEADBEEF`` to ``CRX_RESET``.  This
clears all registers, zeroes BAR 1, and returns the device to the
IDLE state.

Soft reset
----------

Writing ``0xDEADBEEF`` to ``CRX_RESET``:

* Cancels any in-progress operation.
* Zeros all shadow registers (key, DMA addresses, length, IRQ state).
* Clears ``CRX_HW_ERR_COUNT`` to zero.
* Zeroes BAR 1.
* Sets ``CRX_STATUS`` to ``STATUS_IDLE``.
* Lowers the INTx line (or sends no further MSI).

.. note::

   ``CRX_TEMP`` is **not** affected by a soft reset.  Temperature is a
   physical property of the die and continues to be updated regardless of
   device state.

Suggested Linux driver interface
---------------------------------

The device is exposed as ``/dev/crx0``.

``ioctl`` commands
~~~~~~~~~~~~~~~~~~

=================  ======  ======================  ===========================
Command            Code    Argument                Description
=================  ======  ======================  ===========================
CRX_IOC_SET_KEY    0xC0    ``u32``                 Set XOR key
CRX_IOC_ENCRYPT    0xC1    ``struct crx_op *``     Submit encrypt operation
CRX_IOC_GET_STATS  0xC2    ``struct crx_stats *``  Read performance counters
CRX_IOC_RESET      0xC3    —                       Soft reset
=================  ======  ======================  ===========================

::

  struct crx_op {
      __u64 src;    /* userspace VA of input buffer  */
      __u64 dst;    /* userspace VA of output buffer */
      __u32 len;    /* bytes to process (max 65536)  */
      __u32 key;    /* XOR key (used if CRX_FLAG_KEY is set) */
      __u32 flags;  /* combination of CRX_FLAG_* values below */
  };

  /* crx_op.flags */
  #define CRX_FLAG_USE_DMA  (1 << 0)  /* use DMA engine (required) */
  #define CRX_FLAG_ASYNC    (1 << 1)  /* non-blocking (reserved, not yet supported) */
  #define CRX_FLAG_KEY      (1 << 2)  /* use crx_op.key instead of per-ctx key */

  struct crx_stats {
      __u64 ops_completed;    /* successful encrypt operations */
      __u64 bytes_processed;  /* total bytes encrypted         */
      __u64 irq_count;        /* total interrupts handled      */
      __u64 hw_errors;        /* IRQ_ERROR events seen by ISR  */
      __u64 temp_alerts;      /* IRQ_TEMP_HIGH events seen by ISR */
      __u32 temperature;      /* last temperature reading (°F) at IRQ_TEMP_HIGH */
      __u32 _pad;
  };

``read`` / ``write``
~~~~~~~~~~~~~~~~~~~~~

* ``write()`` — copies user data into the driver's DMA buffer, returns number of bytes written.
* ``read()``  — copies data from the DMA buffer to user, returns number of bytes read.
* Neither operation triggers encryption; call ``ioctl(CRX_IOC_ENCRYPT)``
  separately.

``mmap``
~~~~~~~~

Mapping ``/dev/crx0`` maps BAR 1 (64 KB) directly into the process
address space for zero-copy access.  The mapping is ``PROT_READ | PROT_WRITE``.
After populating the buffer, the process calls ``ioctl(CRX_IOC_ENCRYPT)``
and can then read the encrypted result back through the same mapping.

Sysfs attributes
~~~~~~~~~~~~~~~~

The driver exports the following read-only attributes under
``/sys/bus/pci/devices/<BDF>/crx/``:

=====================  ======================================================
Attribute              Description
=====================  ======================================================
``ops_completed``      Total successful encrypt operations (driver counter)
``bytes_processed``    Total bytes encrypted (driver counter)
``irq_count``          Total interrupts handled by the ISR (driver counter)
``hw_err_count``       Hardware error count read live from ``CRX_HW_ERR_COUNT``
``temperature``        Current die temperature read live from ``CRX_TEMP`` (°F)
``temp_alerts``        Number of ``IRQ_TEMP_HIGH`` events (driver counter)
=====================  ======================================================

``hw_err_count`` and ``temperature`` are read directly from device registers
on each sysfs read, so they always reflect the current hardware state.
