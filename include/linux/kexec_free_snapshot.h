/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_KEXEC_FREE_SNAPSHOT_H
#define MODELOS_KEXEC_FREE_SNAPSHOT_H
#include <linux/types.h>
#define KEXEC_FREE_NAME "modelos-free"
#define KEXEC_FREE_MAGIC "KHOFB001"
struct kexec_free_snapshot {
 char magic[8];
 __u32 version,header_bytes;
 __u64 max_pfn,free_pages,protected_phys;
 __u32 bitmap_bytes,status,checksum,capture_flags;
 unsigned long bitmap[];
};
static inline __u32 kexec_free_checksum(const struct kexec_free_snapshot *h)
{
 const unsigned char *p=(const void *)h;__u32 result=2166136261U;
 unsigned long bytes=sizeof(*h)+h->bitmap_bytes;
 for(unsigned long i=0;i<bytes;i++)
  result=(result^((i>=__builtin_offsetof(struct kexec_free_snapshot,checksum)&&i<__builtin_offsetof(struct kexec_free_snapshot,checksum)+4)?0:p[i]))*16777619U;
 return result;
}
void modelos_kexec_snapshot_free(void);
#endif
