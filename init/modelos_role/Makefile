# SPDX-License-Identifier: GPL-2.0
obj-y := head.o runtime.o strings.o text-aot.o role-runtime.o net-stack.o nic.o storage.o
always-y := vmlinux.lds
ccflags-y += -fno-stack-protector -fno-builtin
ccflags-y += -I$(MODELOS_TEXT_SOURCE) -I$(MODELOS_CORE_SOURCE) -I$(MODELOS_NET_SOURCE)
CFLAGS_storage.o += -DMODELOS_STRATA_STORAGE_SOURCE=\"$(MODELOS_CORE_SOURCE)/strata-storage.c\"

ifeq ($(CONFIG_MODELOS_ROLE_GPU),y)
obj-y += gpu-service.o gpu-boot.o gpu-production.o gpu-owner.o gpu-text.o gpu-image.o gpu-engine.o gpu-io.o core-runtime.o
ccflags-y += -I$(obj) -I$(MODELOS_ROLE_GPU_SOURCE)
else
obj-y += service.o image.o
endif
