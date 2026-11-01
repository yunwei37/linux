Reusing the ModelOS source-deletion series
========================================

``tools/modelos/trim-series`` lists the original immutable source commits,
ordered on Linux v6.18.57 (2ff600d29ee478d0a8d46ef8a58afb5e038fe692).
It includes the C/Rust privileged entry, syscall/return removal, exec/user-MM/
VFS removal, optional-driver integration, user-copy/frame removal, user identity
and signal-queue removal, the normal-Linux clone-guard correction, and residual
FPU/prctl/fork-helper exclusion, kernel-only TCP socket/iterator specialization,
and exclusion of the unused syscall/UID-overflow kernel/sys.o unit. Kernel workers, page tables and kernel allocation
remain. This is an x86-64 kernel-only specialization, not a general Linux ABI.

Export and apply in a clean application-owned checkout::

  tools/modelos/export-trim-series.sh /tmp/modelos-series
  # Optional incoming KHO recovery support:
  # tools/modelos/export-trim-series.sh /tmp/modelos-kho-series --with-kho
  git switch -c my-kernel-only-image 2ff600d29ee478d0a8d46ef8a58afb5e038fe692
  git am --3way /tmp/modelos-series/[0-9]*.patch

Use at least 64 commits of branch history when fetching a shallow source pin.
The exporter checks parent availability and refuses a shallow-boundary root
patch. It writes patches outside the source tree; the commit list is the only
maintained series, not a second implementation. The optional KHO commit is
independent of the L1 application payload and does not enable a producer loader.
Both core-only and core-plus-KHO export/application are checked against the tag.

Select CONFIG_MODELOS and CONFIG_MODELOS_KERNEL_ONLY with the public minimal
CPU/Rust/NVIDIA config as a reference; adapt the privileged payload entry for
the application. A series-only tree intentionally excludes later application
commits and documentation/defconfigs; those can be read from the published
fork pin. Enabling the flag alone on an arbitrary defconfig is not sufficient:
IA32/X32/FRED/Xen, userspace loaders, modules and user filesystems must be off.
Rebuild and audit the actual final ELF, then run the real payload acceptance.
Do not satisfy a new consumer's link failure by silently restoring user MM.
The TCP specialization retains kernel socket ownership, kernel iterators,
checksum copy, static IP configuration and actual driver ethtool/RSS callbacks.
It excludes socket files/fds/sockfs, user poll/ioctl/splice, user netlink endpoints,
SCM transport, user zerocopy pinning and user buffer constructors. Other added
consumers still need an explicit kernel API port. The networking commit is an
independent parent retained by a normal merge with the concurrent GPU serving
port; the export contains source specialization only, not the serving payload.

The historical CPU-serving builder's first three trim patches produce exactly
the tree at 915f1376a (tree 7b458e25969df2d3b8f108a5f1762fb83a147165).
Do not reapply that prefix on top of itself. Prefer an application-only rebase
onto the complete published fork. Its application changes merge in three
places: init/Kconfig, init/Makefile and init/modelos.c. Preserve both the
application's MODELOS_SERVE entry/build/worker/network hooks and the new
kthread stop/return probe. The ModelOS repository's docs/kernel-trim report
contains the exact owner handoff and final-ELF reference evidence.

Serving ELF consumer/exclusion map
----------------------------------

* do_mmap: vm_mmap_pgoff calls it; mm/mmap.o and its user wrapper are excluded
  by 73ae10c61. No kernel socket or GPU VA API needs a CPU user VMA.
* handle_mm_fault: __get_user_pages and exc_page_fault call it in the legacy
  serving ELF; 73ae10c61 excludes user memory/GUP and the user-fault branch.
* start_thread: no direct call remains in that ELF, but export/addressable,
  alternatives and exception-table relocations retain it. 53f3f30bd excludes
  the user frame constructor and export instead of relying on garbage collection.
* user_mode_thread: no direct call remains; its IBT endbr-seal relocation
  retains it. 53f3f30bd excludes the user-clone wrapper. Kernel thread creation
  remains supported and is exercised by four workers and stop/return 37.

No GPU execution is implied by applying this series. The dedicated
modelos.nv_test=1 path requires independent hardware acceptance by its owner.

The final source cuts also exclude VFS pointer formatting (%pd/%pD) and user
VMA flag formatting (%pGv), its descriptor table and user VMA debug dump.
Those object domains do not exist in the kernel-only image. Physical page and
GFP flag diagnostics remain, as do the ordinary Linux formatting branches.
