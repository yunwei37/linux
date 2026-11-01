/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_KHO_REQUEST_H
#define MODELOS_KHO_REQUEST_H
#include <linux/types.h>
#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif
#define MODELOS_REQUEST_NAME "modelos-request"
#define MODELOS_REQUEST_MAGIC "Q3REQ001"
#define MODELOS_REQUEST_HEADER 4096U
#define MODELOS_REQUEST_REGION (64ULL << 20)
#define MODELOS_REQUEST_CONTINUE 8U
/* Data-only checkpoint: no CPU pointers, RM handles or live queue state. */
struct modelos_kho_request {
 char magic[8];
 __u32 version,header_bytes;
 __u64 arena_bytes,layout_tag,old_gpu_va,instance_generation,request_generation;
 __u32 model_fnv,cubin_fnv,state_fnv,header_fnv;
 __u32 case_id,emitted,position,next_token,capacity,history_count;
 __u32 history[256],emitted_ids[64];
};
_Static_assert(sizeof(struct modelos_kho_request)<=MODELOS_REQUEST_HEADER,"request header size");
static inline __u32 modelos_request_fnv(const void *ptr,__u64 bytes)
{
 const __u8 *p=ptr;__u32 h=2166136261U;
 for(__u64 i=0;i<bytes;i++)h=(h^p[i])*16777619U;
 return h;
}
static inline __u32 modelos_request_header_fnv(const void *ptr)
{
 const __u8 *p=ptr;__u32 h=2166136261U;
 const unsigned off=offsetof(struct modelos_kho_request,header_fnv);
 for(unsigned i=0;i<MODELOS_REQUEST_HEADER;i++)
  h=(h^((i>=off&&i<off+sizeof(__u32))?0:p[i]))*16777619U;
 return h;
}
#endif
