ModelOS kernel-only build
=========================

This branch is based on upstream v6.18.57. CONFIG_MODELOS_KERNEL_ONLY removes
CPU user execution, exec, user address spaces and VFS from the compiled kernel.
The C/Rust entries start four bound busy kernel workers after initcalls.
There is no initramfs or user init. Kernel task scheduling, physical allocation,
page tables, IRQ/time/SMP and diagnostics retain kernel consumers.

Build in an isolated development environment; do not install on the host::

  make O=/workspaces/build/modelos LLVM=1 modelos_cpu_defconfig
  make O=/workspaces/build/modelos LLVM=1 -j4 bzImage
  qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 1024 -nodefaults \
    -display none -serial stdio -no-reboot \
    -kernel /workspaces/build/modelos/arch/x86/boot/bzImage \
    -append 'console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 keep_bootcon'

Use modelos_rust_defconfig with system Rust/bindgen compatible with this kernel.
The NVIDIA configuration requires the companion public fork branch
https://github.com/yunwei37/open-gpu-kernel-modules/tree/modelos/615.78.08.
Before building modelos_nvidia_defconfig, build its native RM core and link its
kernel-open directory at drivers/modelos-nvidia::

  make -C /workspaces/src/open-gpu-kernel-modules -j4 \
    kernel-open/nvidia/nv-kernel.o_binary
  ln -s /workspaces/src/open-gpu-kernel-modules/kernel-open drivers/modelos-nvidia

The symlink is local integration state and is not committed. The driver is
built in; no module loader, /dev/nvidia*, ioctl transport, procfs, libcuda or
CUDA user runtime is used. Matching GSP firmware must be built in for hardware.
The companion driver provides an opt-in built-in SM120 store test. With its
matching firmware built in, boot with ``modelos.nv_test=1 modelos.nv_gpu=0``.
It checks 1,025 GPU writes/completions, one GPFIFO ring wrap, and RM teardown.
See ``kernel-open/nvidia/README.modelos.md`` in the driver fork for toolchain,
firmware inputs, exact expected result and the dedicated serve-lane procedure.
No-GPU boots validate fixture parsing and the explicit SKIP path only; GPU
execution and Linux-first/kexec state handoff remain separate hardware tests.
