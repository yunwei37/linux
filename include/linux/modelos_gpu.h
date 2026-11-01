/* SPDX-License-Identifier: MIT */
#ifndef _LINUX_MODELOS_GPU_H
#define _LINUX_MODELOS_GPU_H
#include <linux/types.h>
#define MODELOS_GPU_ARENAS 8
#define MODELOS_GPU_ARENA_BYTES (64ULL << 20)
int modelos_gpu_prepare(void);
int modelos_gpu_arena_acquire(unsigned arena, u32 request);
int modelos_gpu_arena_release(unsigned arena, u32 request);
int modelos_gpu_step(unsigned worker, unsigned arena, u32 request,
                     u32 token, u32 position, u32 *result);
int modelos_gpu_stop(void);
bool modelos_l1_serving(void);
int modelos_l1_release_held(void);
#endif
