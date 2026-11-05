Coral V2 functional NPU (local experiment)
=========================================

The ``coral-npu`` PCI device connects an ARM64 or RISC-V64 Linux guest to
the official Coral V2 instruction simulator through a host shared library::

    -device coral-npu,backend=/absolute/path/to/libcoral_qemu.so

This is an experimental transport, with PCI ID ``1234:c0a1``. It is not the
Coral AXI hardware interface. It uses a worker thread and a completion bottom
half, and does not support migration. A shared library built for the host
architecture is required; the backend ABI is versioned and checked at realize.

BAR0 (4 KiB, little-endian 32-bit accesses):

* 0x00: signature 0x434f524c; 0x04: ABI version 1.
* 0x08: command, 1 START, 2 RESET.
* 0x0c: status, BUSY=1, DONE=2, ERROR=4.
* 0x10: entry point; 0x14: instruction limit, 1..1000000 (default 100000).
* 0x18: result, 1 success, 2 timeout, 3 trap, 4 invalid/backend error.
* 0x1c/0x20: instruction count low/high (not hardware cycles).
* 0x24: INTx enable bit 0; 0x28: pending, write 1 to acknowledge.
* 0x100..0x1ff: NUL-terminated backend diagnostic.

BAR1 (8 MiB): offset 0 maps 8 KiB ITCM at NPU address 0; offset 0x10000
maps 32 KiB DTCM at NPU address 0x10000; offset 0x400000 maps 4 MiB external
SRAM at NPU address 0x20000000. Holes read all ones and ignore writes.
While BUSY, all SRAM reads return all ones and writes are ignored.

START snapshots SRAM and ignores further ENTRY/LIMIT/START writes until
completion. The backend runs a fresh Coral simulator, initializes PC and SP
(0x18000), and stops on ebreak/mpause, a trap or the instruction budget.
The completion BH copies output back and publishes DONE (plus ERROR on
failure), with completion pending set irrespective of interrupt enable.

RESET waits for the bounded request, cancels pending completion, clears SRAM,
status, count, diagnostic and interrupt state. It does not preempt the worker.
An in-progress reset may therefore block the main loop until the request ends.

Source and reproducible tests are in the companion ``coralnpu`` checkout at
``integration/qemu``. Both Linux guests use a root-only sysfs BAR mmap runtime
and polling in this first version. INTx delivery to a guest ISR, DMA, persistent
NPU register state, official compiler/runtime compatibility and performance
simulation require further work.

This implementation was created with AI assistance for a local experiment.
It is not intended for an upstream QEMU contribution; see
``docs/devel/code-provenance.rst`` and ``AGENTS.md``.
