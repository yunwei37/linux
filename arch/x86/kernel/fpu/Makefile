# SPDX-License-Identifier: GPL-2.0-only
#
# Build rules for the FPU support code:
#

obj-y				+= init.o bugs.o core.o regset.o signal.o xstate.o

ifdef CONFIG_MODELOS_KERNEL_ONLY
obj-y := $(filter-out regset.o signal.o,$(obj-y))
endif
