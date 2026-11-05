# Superseded pre-role ModelOS kernel variants

These cited source snapshots preserve the minimal/task/RM/Linux-TCP/KHO/DPS
experiments from commit 63cb67e76c65b92935d6e9a92b8b9d2f295dc12b and its
handoff ancestors. They are historical evidence, excluded from Kbuild, and
not alternative supported kernel configurations or service images.

Only MODELOS_ROLE_BOOT remains. The one system boots directly, using the
Linux-source boot envelope and canonical ModelOS runtime/driver adapters.
Legacy conditional specializations were folded as disabled, matching the
qualified role config; their exact old content remains at the commit above.
The polled NIC has one source file and is compiled by the role adapter.
