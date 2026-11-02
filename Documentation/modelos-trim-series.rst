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


The series has 27 core commits, or 28 with the incoming-only KHO support.
The event-input cuts exclude synthetic-uevent sysfs attributes and parsing,
uevent Netlink receive/forward callbacks, and Netlink bind/connect/getname
and automatic-address helpers. The public AF_NETLINK family creation and
NETLINK_USERSOCK reservation are absent; internal netlink_kernel_create
still uses sock_create_lite and __netlink_create directly.

Thermal Netlink management and its three reverse selectors are unavailable
under MODELOS_KERNEL_ONLY. Thermal core, the configured governor and package
temperature driver remain. Run olddefconfig after applying this series;
do not restore the userspace management interface merely to retain a sensor.
Ordinary Linux keeps the original interfaces and reverse selections.


The final controller cut excludes Generic Netlink family/policy user queries,
their operation table and policy dump helpers, while retaining family
registration and duplicate-name checks and outgoing event construction.
The bus/driver cut excludes sysfs attribute dispatch, manual bind/unbind and
probe/autoprobe callbacks and their registration/removal roots. Automatic
hardware probing and object release remain. Ordinary Linux keeps all original
callbacks when CONFIG_MODELOS_KERNEL_ONLY is disabled. No panic or additional
configuration switch is introduced. The ModelOS report contains per-cut
six-profile size/symbol/input counts, KVM worker/service and warm KHO evidence.
These cuts do not claim removal of all rtnetlink or other sysfs control code.

The device/class cut excludes their sysfs read/write dispatch bodies and type
pointers while preserving object release. The Generic Netlink receive cut
excludes request parsing/dumps and socket group/private hooks; notification
registration APIs remain. The rtnetlink cut excludes seven core/IPv4 command
tables and registrations plus incoming/bind/link command entry bodies.
Compiler-unused closure excludes 168 further static definitions in those
seven files. Normal Linux keeps the original code. Kernel TCP/IP routing,
neighbour maintenance and object lifetime are validated by actual serving.
This closure is validated with the shipped reference configs; a new networking
consumer/config still needs source, link and real payload validation.

The public branch also includes the serving owner's application commits.
The source-only export excludes them. A concurrent application update was
retained and the combined CPU service revalidated; detailed provenance and
before/after size data live in ModelOS docs/kernel-trim/dispatch.md.


The netdev/TCP metrics cut removes their Generic Netlink presentation,
registration and event calls. Kernel TCP metric-cache allocation/lifetime and
page-pool lifecycle remain. All net_device.stat_ops reads were in the removed
netdev presentation unit; the selected virtio driver therefore loses its three
query callbacks, table and assignment instead of relocating a helper.
Other network-driver selections require their own consumer validation.

The ACPI cut removes its userspace event producer/family/initcall and the
x86 caller statements while keeping the blocking notifier chain and critical
thermal action. The unselected BlueField pwr-mlxbf driver is outside these
reference configs; selecting it needs a port. No empty event stub is added.

Finally, with no enabled Generic Netlink family consumers left, genetlink.o
and policy.o are omitted in the kernel-only build. The remaining raw-Netlink
user request/dump/ACK bodies and GENL-specific destructor accounting are also
excluded. The unconditional sock_diag request/socket/workqueue root is also
removed; socket destruction and the generic cookie generator remain.
Raw kernel notification sockets, TCP and hardware lifetime remain.
Normal Linux keeps the original units and interfaces. Detailed before/after
six-profile metrics, object controls, cold/service/warm-KHO acceptance and
source provenance are in ModelOS docs/kernel-trim/genl.md.


The generic kobject cut removes its sysfs dispatch functions/table and six
reference-profile initializer roots, preserving release/refcount/namespace
lifetime. The uevent cut removes the producer unit, device/bus callback roots,
selected bus/class formatters and event-only rename/move allocations and calls.
The existing kset API receives NULL event ops; no replacement event stub is
introduced. Automatic driver binding, bus/power notifiers and recovery actions
remain. Other optional drivers need a source consumer port.

The thermal cut omits thermal_sysfs.o and thermal_thresholds.o, their setup,
teardown and binding-attribute callbacks. SYSFS, THERMAL_NETLINK/STATISTICS/
HWMON/DEBUGFS are off in the reference profiles. The removed user-threshold
list has no producer once Thermal Netlink management is gone. Hardware trip
handling/programming, the step-wise governor and real cooling actions remain.
A matched kernel-only fake-sensor test exercises 0 -> 1 -> 0 cooling, critical
callback dispatch and the exact 38999/90000 trip window before and after the
cut. The probe is an explicit separate test image, not a shipped kernel hook.
All three cuts pass six builds, cold four-worker KVM/service/warm-KHO checks.
Detailed source roots, matrices and probe receipts are in ModelOS
 docs/kernel-trim/notifications.md. Raw route notifications and other attribute
metadata are still inventoried; complete interface closure is not claimed.


The next cut removes PCI new_id/remove_id driver attributes, platform
modalias/numa_node device attributes and the common bus driver_override
sysfs group. Their callback/attribute/group definitions and initializer or
registration references are excluded together. PCI static/dynamic matching,
the kernel dynamic-ID/override APIs, platform matching and driver probing,
removal, references and DMA operations remain. Other attribute groups are
not implicitly classified as hardware requirements. Six independently cached
reference profiles pass builds; CPU/Rust/RM and KHO cold boots, actual
320-token CPU service, concurrency/cancel/lifetime and warm KHO pass.
Exact ELF data-pointer holders, section/symbol/source accounting and normal
Linux object controls are in ModelOS docs/kernel-trim/bus-attributes.md.
