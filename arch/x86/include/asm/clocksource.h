/* SPDX-License-Identifier: GPL-2.0 */
/* x86-specific clocksource additions */

#ifndef _ASM_X86_CLOCKSOURCE_H
#define _ASM_X86_CLOCKSOURCE_H

#include <asm/vdso/clocksource.h>

extern unsigned int vclocks_used;

static inline bool vclock_was_used(int vclock)
{
#ifdef CONFIG_MODELOS_KERNEL_ONLY
	return false;
#else
	return READ_ONCE(vclocks_used) & (1U << vclock);
#endif
}

static inline void vclocks_set_used(unsigned int which)
{
#ifndef CONFIG_MODELOS_KERNEL_ONLY
	WRITE_ONCE(vclocks_used, READ_ONCE(vclocks_used) | (1 << which));
#endif
}

#endif /* _ASM_X86_CLOCKSOURCE_H */
