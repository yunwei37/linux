/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_DPS_REQUEST_HANDOFF_H
#define MODELOS_DPS_REQUEST_HANDOFF_H
#include <linux/types.h>
#define DPS_REQUEST_MAGIC "MOSREQ01"
#define DPS_REQUEST_OFFSET 4096
#define DPS_REQUEST_COUNT 6
#define DPS_REQUEST_SOURCE_SERIAL 48
#define DPS_REQUEST_TARGET_SERIAL 50
struct dps_request_cursor {
 __u32 id,generation,slot,case_id,emitted,next_pos,last_token,budget;
};
struct dps_request_handoff {
 char magic[8];
 __u32 version,bytes,count,split,checksum,reserved;
 struct dps_request_cursor request[DPS_REQUEST_COUNT];
};
struct dps_request_bundle {
 struct dps_handoff instance;
 struct dps_request_handoff requests;
};
static inline __u32 dps_request_checksum(const struct dps_request_handoff *h)
{
 const unsigned char *p=(const void *)h;__u32 sum=2166136261U;
 for(unsigned i=0;i<sizeof(*h);i++)sum=(sum^((i>=__builtin_offsetof(struct dps_request_handoff,checksum)&&i<__builtin_offsetof(struct dps_request_handoff,checksum)+4)?0:p[i]))*16777619U;
 return sum;
}
#endif
