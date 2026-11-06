// SPDX-License-Identifier: GPL-2.0-or-later
#include "role.h"
#include <linux/cache.h>
#include "../../lib/crc/x86/crc-pclmul-consts.h"

extern u32 role_crc32_pclmul(u32, const void *, size_t, const void *);

bool role_crc32_accelerated(const void *data, u64 bytes, u32 *result)
{
	u32 a = 1, b, c, d;
	const u32 required = (1U << 1) | (1U << 9) | (1U << 19);

	if (bytes < 16)
		return false;
	asm volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
	if ((c & required) != required)
		return false;
	/* PCLMULQDQ, SSSE3 and SSE4.1 were checked above. Every role core has
	 * already enabled its FPU once and owns it for its complete Op loop. */
	*result = ~role_crc32_pclmul(~0U, data, bytes,
			crc32_lsb_0xedb88320_consts.fold_across_128_bits_consts);
	return true;
}
