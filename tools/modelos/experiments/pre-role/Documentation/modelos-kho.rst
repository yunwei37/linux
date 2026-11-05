ModelOS KHO receiver
===================

Build with ``make LLVM=1 modelos_kho_defconfig`` and ``make LLVM=1 -j4 bzImage``.
This is a kernel-only x86-64 successor, not an outgoing kexec producer.
The optional ``CONFIG_KEXEC_HANDOVER_RECEIVER`` keeps the existing v6.18.57
``kho-v1`` wire format, early scratch restrictions, preserved-page deserializer,
``kho_restore_{folio,pages,vmalloc}`` and named ``reserve_mem`` revival.
It selects relocatable loading but does not select KEXEC_FILE, CMA or migration.
KEXEC, KEXEC_FILE, KEXEC_HANDOVER_DEBUG/DEBUGFS must stay disabled in this profile.
An ordinary producer still uses CONFIG_KEXEC_HANDOVER and its usual dependencies.

The receiver releases the incoming scratch as MIGRATE_UNMOVABLE kernel memory.
It neither prepares new scratch for another handover nor serializes, finalizes,
loads a future kernel image or exposes debugfs. The producer must finalize KHO
before kexec_file_load. Use ``kho=on reserve_mem=2M:4096:modelos-image`` on both
sides when testing the named-region contract; only the producer needs
``kho_scratch=256M``. No initramfs or userspace executable belongs to the successor.

A RAM-only four-vCPU/4-GiB KVM test passed: ordinary Linux producer to this
trimmed receiver, is_kho_boot 0 to 1, the same physical 2-MiB named reservation,
every byte equal to (offset * 13 + 7) & 255, FNV32 6f9c9dc5, four busy
CPL-0/mm-none workers for 20 seconds and kthread return 37. The receiver's
verification initcall was linked only into the test image, not this defconfig's
production image. Reproducible guest-only probe/loader/harness and logs live in
the ModelOS repository under kernel/kho/ and docs/kernel-trim/.

At the same source/config/compiler checkpoint (Clang 19, ThinLTO, XZ), the
CPU bzImage increased from 644096 to 648192 bytes (+4096); complete vmlinux
from 11002312 to 11004560 (+2248); sized symbols from 9178 to 9223 (+45).
ELF text/data increased 7875/984 bytes; bss alignment decreased 4096 bytes.
These costs include RELOCATABLE, the libfdt reader and memblock KHO scratch
support. They do not include a GPU driver, firmware or the optional test probe.

This validates named RAM recovery, not GPU/GSP/RM-context adoption, vmalloc
recovery, outgoing receiver-to-third-kernel handover or first-token timing.
No old RM pointer/handle becomes valid solely because its numeric value survived.
