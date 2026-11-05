/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_DPS_HANDOFF_H
#define MODELOS_DPS_HANDOFF_H
#include <linux/types.h>
#define DPS_NAME "modelos-dps"
#define DPS_MAGIC "MOSDPS01"
struct dps_handoff {
 char magic[8];
 __u32 version,bytes;
 __u64 queue_phys;
 __u32 serial,copy_mode,probe_status,checksum;
};
static inline __u32 dps_checksum(const struct dps_handoff *h)
{
 const unsigned char *p=(const void *)h;__u32 sum=2166136261U;
 for(unsigned i=0;i<sizeof(*h);i++)sum=(sum^((i>=__builtin_offsetof(struct dps_handoff,checksum)&&i<__builtin_offsetof(struct dps_handoff,checksum)+4)?0:p[i]))*16777619U;
 return sum;
}
#endif
