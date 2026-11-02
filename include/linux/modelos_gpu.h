/* SPDX-License-Identifier: MIT */
#ifndef _LINUX_MODELOS_GPU_H
#define _LINUX_MODELOS_GPU_H
#include <linux/types.h>
#define MODELOS_GPU_ARENAS 8
#ifdef CONFIG_MODELOS_L1_LAUNCH
#define MODELOS_GPU_ARENA_BYTES (512ULL << 20)
#else
#define MODELOS_GPU_ARENA_BYTES (64ULL << 20)
#endif
int modelos_gpu_prepare(void);
int modelos_gpu_arena_acquire(unsigned arena, u32 request);
int modelos_gpu_arena_release(unsigned arena, u32 request);
int modelos_gpu_step(unsigned worker, unsigned arena, u32 request,
                     u32 token, u32 position, u32 *result);
int modelos_gpu_stop(void);
int modelos_gpu_prefill(unsigned worker,unsigned arena,u32 request,const u32 *tokens,unsigned count,u32 *result);
u64 modelos_kernel_faults(void);
void modelos_gpu_domain_account(u32 id,u32 input,u32 output,u64 wait,u64 send,u64 alloc,u64 free,u64 compute,u64 prefill,u64 decode);
bool modelos_l1_serving(void);
int modelos_l1_release_held(void);
#endif
