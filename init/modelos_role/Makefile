# SPDX-License-Identifier: GPL-2.0
# Linux supplies entry assembly and the linker script; our runtime is Rust.
obj-y := head.o role-runtime.o
# The runtime is Rust: objtool cannot decode its compiler-builtin bodies
# (`fma`, the float helpers), and it has no kernel C to validate anyway.
OBJECT_FILES_NON_STANDARD := y
always-y := vmlinux.lds
