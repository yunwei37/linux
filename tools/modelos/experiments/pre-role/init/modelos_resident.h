/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_RESIDENT_H
#define MODELOS_RESIDENT_H
#include <linux/types.h>
#define LIVE_NAME "modelos-live"
#define LIVE_MAGIC "MOSLIVE1"
#define LIVE_SAMPLES 4096
#define LIVE_CONTINUE 8
struct live_page { __u64 phys; __u32 fnv, reserved; };
struct live_handoff {
 char magic[8];
 __u32 version, bytes, mode, samples;
 __u64 queue_phys, queue_gpu, generation;
 __u32 serial, emitted, position, next_token;
 __u32 queue[6], checksum, reserved;
 struct live_page page[LIVE_SAMPLES];
};
static inline __u32 live_fnv(const void *v, unsigned long bytes)
{
 const unsigned char *p=v; __u32 h=2166136261U;
 while(bytes--)h=(h^*p++)*16777619U;
 return h;
}
static inline __u32 live_checksum(struct live_handoff *h)
{
 __u32 old=h->checksum, result;
 h->checksum=0;result=live_fnv(h,sizeof(*h));h->checksum=old;return result;
}
#endif
