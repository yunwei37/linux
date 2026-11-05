// SPDX-License-Identifier: GPL-2.0-only
/* Byte-wise full_name_hash fallback from fs/namei.c, for netdev names only. */
#include <linux/stringhash.h>
unsigned int full_name_hash(const void *salt, const char *name, unsigned int len)
{
	unsigned long hash = init_name_hash(salt);
	while (len--)
		hash = partial_name_hash((unsigned char)*name++, hash);
	return end_name_hash(hash);
}
