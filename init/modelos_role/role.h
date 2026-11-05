/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MODELOS_ROLE_H
#define MODELOS_ROLE_H
#include <linux/types.h>
#define ROLE_DIRECT 0xffff888000000000UL
#define ROLE_MAX_CPUS 32
static inline void *role_va(u64 p) { return (void *)(ROLE_DIRECT+p); }
static inline u64 role_pa(const void *p) { u64 a=(u64)p; return a>=0xffffffff80000000UL?a-0xffffffff80000000UL:a-ROLE_DIRECT; }
static inline u8 role_in(u16 port) { u8 v; asm volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static inline void role_out(u16 port,u8 v) { asm volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static inline u64 role_tsc(void) { u32 l,h; asm volatile("rdtsc":"=a"(l),"=d"(h)); return ((u64)h<<32)|l; }
static inline void role_pause(void) { asm volatile("pause"); }
static inline __attribute__((noreturn)) void role_halt(void) { asm volatile("cli"); for(;;)asm volatile("hlt"); }
void start_kernel(void *) __attribute__((noreturn));
void role_fault(u64 *) __attribute__((noreturn));
void role_ap_entry(unsigned) __attribute__((noreturn));
void role_puts(const char *s);
void role_hex(u64 n);
void role_core_init(unsigned slot);
void role_start_aps(void);
void role_service(void) __attribute__((noreturn));
extern u64 role_pml4[512];
extern unsigned role_cpus;
extern u64 role_tsc_hz;
#endif
