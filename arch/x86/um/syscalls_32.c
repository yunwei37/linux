// SPDX-License-Identifier: GPL-2.0
#include <linux/syscalls.h>
#include <os.h>

#ifndef CONFIG_MODELOS_KERNEL_ONLY
SYSCALL_DEFINE2(arch_prctl, int, option, unsigned long, arg2)
{
	return -EINVAL;
}
#endif
