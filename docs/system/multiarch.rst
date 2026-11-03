.. SPDX-License-Identifier: GPL-2.0-or-later

ARM/RISC-V multiarch machines
============================

The experimental ``multiarch-softmmu`` build combines ARM and RISC-V TCG
frontends in one process. It supports Linux application domains and smaller
firmware domains, each with a private memory view and interrupt controllers.
The machine is not a cache-coherent heterogeneous SMP system.

Build with::

  mkdir build-multiarch
  cd build-multiarch
  ../configure --target-list=multiarch-softmmu --disable-hvf --disable-rust
  ninja qemu-system-multiarch

The aggregate build is opt-in. Ordinary single-target builds retain their
normal target selection. It currently requires the AArch64 and RV64 system
targets, which also provide supported AArch32 and RV32 CPU models.

Configurable domains
--------------------

``-M multiarch-virt`` accepts repeated ``-domain``, ``-domain-loader`` and
``-domain-map`` arguments. For example, with an ARM64 Linux Image and
initramfs in the current directory::

  qemu-system-multiarch -M multiarch-virt -accel tcg,thread=multi \
    -display none -monitor none -chardev stdio,id=console \
    -domain id=linux0,platform=arm-virt,cpu=cortex-a55,cpus=2,ram=512M,chardev=console \
    -domain-loader 'domain=linux0,kernel=Image,initrd=rootfs.cpio.gz,append=console=ttyAMA0 rdinit=/init'

Supported profiles are ``arm-virt`` (Cortex-A53/A55), ``riscv-virt`` (RV64),
``arm-mcu`` (Cortex-M3/R5/R5F), and ``riscv-mcu`` (RV32). Application domains
allow multiple CPUs; MCU domains are single-core. Firmware must follow the
selected profile's address map and boot ABI. RV64 Linux requires an OpenSBI
``fw_dynamic.bin`` supplied through its domain loader's ``firmware=`` property.

Each domain owns its RAM and devices. A shared ``memory-backend-ram`` can be
mapped into several domains explicitly with ``-domain-map``. The guest must
provide its own synchronization protocol. Shared memory does not provide an
automatic mailbox driver or physical cache-coherence model.

``thread=multi`` gives each vCPU a host execution thread. ``thread=single``
provides a serialized baseline. Translators are initialized before threads
start, and translation-block flags retain both cluster identity and parallel
execution state. Migration is currently blocked.

Compatibility machines
----------------------

``multiarch`` provides two independently loaded firmware CPUs. The existing
``multiarch-virt-arm`` and ``multiarch-virt-riscv`` machines provide a primary
Linux CPU plus an independent MCU, using ordinary machine loader options and
``mcu-*`` properties. Historical ``hetero-*`` names remain as aliases.

Use the separate ``ruikoi-labs`` project for guest-image builds, firmware,
end-to-end acceptance and benchmarks. These are not required to build QEMU.
