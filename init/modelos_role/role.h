/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_ROLE_H
#define MODELOS_ROLE_H
#include <linux/types.h>
#define ROLE_DIRECT 0xffff888000000000UL
#define ROLE_MAX_CPUS 32
#define ROLE_DIRECT_LIMIT (64UL<<40)
extern u64 role_phys_delta;
static inline void *role_va(u64 p) { return (void *)(ROLE_DIRECT+p); }
static inline u64 role_pa(const void *p) { u64 a=(u64)p; return a>=0xffffffff80000000UL?a-0xffffffff80000000UL+role_phys_delta:a-ROLE_DIRECT; }
static inline u8 role_in(u16 port) { u8 v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void role_out(u16 port,u8 v) { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline u64 role_tsc(void) { u32 l,h; asm volatile("rdtsc":"=a"(l),"=d"(h)); return ((u64)h<<32)|l; }
static inline void role_pause(void) { asm volatile("pause"); }
static inline __attribute__((noreturn)) void role_halt(void) { asm volatile("cli"); for(;;)asm volatile("hlt"); }
void start_kernel(void *) __attribute__((noreturn));
void role_fault(u64 *) __attribute__((noreturn));
void role_ap_entry(unsigned) __attribute__((noreturn));
void *role_mmio_map(u64,u64,bool);
int role_nic_arm(void);
void role_nic_disarm(void);
u64 role_now_ns(void);
u64 role_net_wait(u64 deadline_ns);
void mos_net_panic(void) __attribute__((noreturn));
enum role_kind { ROLE_IDLE, ROLE_CPU };
int role_reconfigure(unsigned cpu,enum role_kind role,u64 boundary);
int role_acquire_worker(u64 boundary);
bool role_crc32_accelerated(const void *data,u64 bytes,u32 *result);
int role_cpu_copy(unsigned cpu,u64 source,u64 destination,u64 bytes,u64 boundary);
void role_step_boundary(void);
int mos_role_enable_fpu(void);
int mos_role_memory_add_ram(u64,u64);
int mos_role_high_ram_probe(u64);
int mos_role_memory_import(u64,u64,unsigned);
u64 mos_role_memory_free_bytes(void);
int mos_role_move_begin(u64 *);
int mos_role_move_finish(void);
void role_runtime_fault(void) __attribute__((noreturn));
void role_memory_init(void);
void role_puts(const char *s);
void role_hex(u64 n);
int role_ram_contains(u64 start,u64 bytes);
void role_core_init(unsigned slot);
void role_start_aps(void);
void role_service(void) __attribute__((noreturn));
extern u64 role_pml4[512];
extern unsigned role_cpus;
extern u64 role_tsc_hz;
#endif
