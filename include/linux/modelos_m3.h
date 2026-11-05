/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef MODELOS_M3_H
#define MODELOS_M3_H
#include <linux/types.h>
#define M3_LOW_BASE  0x40000000ULL
#define M3_LOW_SIZE  (512ULL<<20)
#define M3_HIGH_BASE 0x400000000ULL
#define M3_HIGH_SIZE (2ULL<<30)
#define M3_TAPE_BASE 0x600000000ULL
#define M3_TAPE_SIZE (512ULL<<20)
void modelos_m3_reserve(void);
void *modelos_m3_alloc(u64 bytes,u64 alignment,u32 kind,bool dma32);
bool modelos_m3_owned(const void *p);
void modelos_m3_free(const void *p);
void modelos_m3_cpu_write(void *p,unsigned long long bytes);
void modelos_m3_fb_map(void *p,unsigned long long bytes);
void modelos_m3_fb_unmap(void *p);
void modelos_m3_snapshot_inputs(void);
void modelos_m3_snapshot_new_inputs(void);
void modelos_m3_mmio_map(void *base);
void modelos_m3_cpu_scalar(void *p,unsigned long long value,unsigned bytes);
void modelos_m3_cpu_read_value(void *p,unsigned long long value,unsigned bytes);
void l2_record(unsigned type,unsigned long long address,const void *data,unsigned length);
void l2_counts_dump(void);
#endif
