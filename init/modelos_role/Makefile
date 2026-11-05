# SPDX-License-Identifier: GPL-2.0
obj-y := head.o runtime.o service.o strings.o text-aot.o role-runtime.o net-stack.o nic.o image.o storage.o
always-y := vmlinux.lds
ccflags-y += -fno-stack-protector -fno-builtin
ccflags-y += -I$(MODELOS_TEXT_SOURCE) -I$(MODELOS_CORE_SOURCE) -I$(MODELOS_NET_SOURCE)
CFLAGS_storage.o += -DMODELOS_STRATA_STORAGE_SOURCE=\"$(MODELOS_CORE_SOURCE)/strata-storage.c\"
