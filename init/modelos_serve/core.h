/* SPDX-License-Identifier: MIT */
#ifndef MODELOS_OBJECT_CORE_H
#define MODELOS_OBJECT_CORE_H
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif
struct mos_handle {u32 slot,generation;};
struct mos_work {struct mos_handle handle;u32 request,position,token,count;};
struct mos_event {struct mos_handle handle;u32 request;u64 session;u32 token,sequence;bool terminal,cancelled;};
struct mos_accounting {u64 live,host,device,admitted,released,peak,steps;};
int mos_engine_load(void);
int mos_engine_reserve(const struct mos_work *w,const u32 *prompt);
int mos_engine_batch(u32 phase,const struct mos_work *w,u32 *out);
int mos_engine_release(const struct mos_work *w);
int mos_engine_unload(void);
void mos_panic(void);
#ifdef __KERNEL__
int modelos_serve_prepare(void);
int modelos_serve_worker(unsigned int cpu);
int modelos_serve_network(void *unused);
#endif
int mos_init(u32 round_robin);
int mos_admit(u64 session,u32 id,const u32 *prompt,u32 count,u32 budget,u64 now,struct mos_handle *h);
int mos_advance(struct mos_event *event);
int mos_cancel(u64 session,struct mos_handle h);
int mos_ack(u64 session,struct mos_handle h,u64 now);
void mos_stats(struct mos_accounting *out);
int mos_stop(void);
#endif
