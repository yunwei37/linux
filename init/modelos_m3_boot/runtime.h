/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MODELOS_M3_RUNTIME_H
#define MODELOS_M3_RUNTIME_H
#include <linux/types.h>
/* Single owner, after bring-up. Tier0=reserved SYS RAM,Tier1=physical VRAM.
 * Fence the device's work before altering mappings; these calls do not stop it.
 * No allocator, RM symbol, task, sleep or lock is used by the runtime calls. */
int m3_gpu_read_u64(u64 address,u32 tier,u64 *value);
int m3_gpu_write_u64(u64 address,u32 tier,u64 value);
int m3_gpu_flush(void);
int m3_gpu_invalidate_post(u64 root,u32 tier,u64 *fence);
int m3_gpu_invalidate_poll(u64 fence); /* 0 pending,1 complete,<0 error */
int m3_gpu_translate(u64 root,u32 tier,u64 va,u64 *physical,u32 *target_tier);
/* Diagnostic fixture only;relaunch the checked cubin from an owner mapping. */
int m3_gpu_cubin_at(u64 va,u64 physical,u64 bytes);
/* Caller supplies an RM/GMC payload, not an outer MCTP frame. RM sequence is
 * assigned from the live queue. Poll returns every frame,including events;
 * the owner correlates replies and must not discard firmware notifications. */
int m3_gsp_post(u32 type,const u8 *payload,u32 bytes,u32 *sequence);
int m3_gsp_poll(u32 *type,u8 *payload,u32 capacity,u32 *bytes);
#endif
