ModelOS kexec workload test
==========================

``CONFIG_KEXEC_HANDOVER_RECEIVER`` is the incoming-RAM mechanism.
``CONFIG_MODELOS_KEXEC_TEST`` enables a separate acceptance workload. With
L1 Qwen enabled it checks the first generated token and exits the persistent
GPU queue; when disabled the normal five-prompt L1 workload is unchanged.

The producer reserves ``reserve_mem=1280M:2M:modelos-image`` and finalizes
KHO before kexec_file_load. The region contains the flat Qwen weights, the
precompiled cubin and a ``Q3KHO001`` manifest in its last page. The receiver
finds it by name, validates lengths and full FNV32 checksums and consumes the
handed-over ``.text.qwen_persistent`` section. The input is a trusted offline
image, not an arbitrary CUDA ELF linker. Constant/resource metadata remains
compiled into the consumer. The manifest's old RM identifiers are diagnostic;
they do not recreate the old driver object graph or DMA/RPC mappings.

For the small RAM prerequisite, a 2-MiB region with the same name is checked
against a deterministic pattern. A warm KHO boot is mandatory for that test.
Use ``earlyprintk=serial,ttyS0,115200 keep_bootcon`` for the kernel-only serial
console. GPU experiments and observations are documented at
https://github.com/yunwei37/modelos/blob/main/docs/gpu-serve/kexec.md .
