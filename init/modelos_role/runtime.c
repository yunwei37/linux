/* SPDX-License-Identifier: GPL-2.0 */
#include "role.h"
#include <linux/string.h>
unsigned role_cpus=1;
u64 role_tsc_hz;
static u64 gdt[ROLE_MAX_CPUS][5] __attribute__((aligned(16)));
struct role_tss {u32 reserved;u64 rsp[3];u64 reserved2;u64 ist[7];u64 reserved3;u16 reserved4,iomap;} __attribute__((packed));
static struct role_tss tss[ROLE_MAX_CPUS];
static char fault_stacks[ROLE_MAX_CPUS][8192] __attribute__((aligned(16)));
static char ap_stacks[ROLE_MAX_CPUS][32768] __attribute__((aligned(16)));
struct descriptor {u16 limit;u64 base;} __attribute__((packed));
struct gate {u16 low,selector;u8 ist,type;u16 mid;u32 high,zero;} __attribute__((packed));
static struct gate idt[256] __attribute__((aligned(16)));
extern char role_fault_stubs[],role_ap_begin[],role_ap_end[];
extern void role_wake_isr(void);
static volatile unsigned ap_online,roles_released;
static u32 apic_read(unsigned reg){return *(volatile u32 *)role_va(0xfee00000+reg);}
static void apic_write(unsigned reg,u32 value){*(volatile u32 *)role_va(0xfee00000+reg)=value;}
void role_puts(const char *s){while(*s){while(!(role_in(0x3fd)&32))role_pause();role_out(0x3f8,*s++);}}
void role_hex(u64 n){char out[19]="0x0000000000000000";int i;for(i=17;i>=2;i--){out[i]="0123456789abcdef"[n&15];n>>=4;}role_puts(out);}
static unsigned fault_print;
void __attribute__((noreturn)) role_fault(u64 *frame){while(__atomic_exchange_n(&fault_print,1,__ATOMIC_ACQUIRE))role_pause();role_puts("ROLE_EXCEPTION vector/error/rip/cr2=");role_hex(frame[0]);role_hex(frame[1]);role_hex(frame[2]);u64 cr2;asm volatile("mov %%cr2,%0":"=r"(cr2));role_hex(cr2);role_puts(" halted\n");__atomic_store_n(&fault_print,0,__ATOMIC_RELEASE);role_halt();}
static void gate(unsigned n,u64 target,unsigned ist){idt[n]=(struct gate){target,8,ist,0x8e,target>>16,target>>32,0};}
void role_core_init(unsigned slot)
{
 u64 cr0,cr4,base=(u64)&tss[slot];
 struct descriptor gd={sizeof(gdt[slot])-1,(u64)gdt[slot]},id={sizeof(idt)-1,(u64)idt};
 gdt[slot][1]=0x00af9a000000ffffUL;gdt[slot][2]=0x00cf92000000ffffUL;
 tss[slot].ist[0]=(u64)&fault_stacks[slot][sizeof(fault_stacks[slot])];tss[slot].iomap=sizeof(tss[slot]);
 gdt[slot][3]=103|((base&0xffffff)<<16)|(0x89UL<<40)|((base>>24&255)<<56);gdt[slot][4]=base>>32;
 asm volatile("lgdt %0"::"m"(gd):"memory");
 asm volatile("pushq $8; leaq 1f(%%rip),%%rax; pushq %%rax; lretq; 1:":::"rax","memory");
 asm volatile("mov $16,%%ax; mov %%ax,%%ds; mov %%ax,%%es; mov %%ax,%%ss; mov $24,%%ax; ltr %%ax":::"rax","memory");
 if(!slot){unsigned i;for(i=0;i<256;i++)gate(i,(u64)role_fault_stubs+13*16,1);for(i=0;i<32;i++)gate(i,(u64)role_fault_stubs+i*16,1);gate(0x23,(u64)role_wake_isr,0);gate(0xf1,(u64)role_wake_isr,0);}
 asm volatile("lidt %0"::"m"(id):"memory");
 asm volatile("mov %%cr0,%0":"=r"(cr0));cr0=(cr0&~12UL)|2;asm volatile("mov %0,%%cr0"::"r"(cr0));
 asm volatile("mov %%cr4,%0":"=r"(cr4));cr4|=0x600;asm volatile("mov %0,%%cr4; fninit"::"r"(cr4));
 {u32 mxcsr=0x1f80;asm volatile("ldmxcsr %0"::"m"(mxcsr));}
 apic_write(0xf0,0x1ff);apic_write(0x320,0x10000);apic_write(0x350,slot?0x10000:0x700);apic_write(0x360,0x400);
}
/* Read firmware topology, not Linux CPU/task discovery. */
static u8 checksum(const void *p,unsigned n){const u8 *b=p;u8 s=0;while(n--)s+=*b++;return s;}
struct acpi_header {char sig[4];u32 len;u8 rest[28];} __attribute__((packed));
static struct acpi_header *acpi_find(void)
{
 u64 p;u8 *rsdp=0;
 for(p=0xe0000;p<0x100000;p+=16){u8 *b=role_va(p);if(!memcmp(b,"RSD PTR ",8)&&!checksum(b,20)){rsdp=b;break;}}
 if(!rsdp)return 0;
 u64 root=*(u32 *)(rsdp+16);unsigned stride=4;
 if(rsdp[15]>=2&&!checksum(rsdp,36)&&*(u64 *)(rsdp+24)){root=*(u64 *)(rsdp+24);stride=8;}
 if(root>=0x100000000UL)return 0;
 struct acpi_header *h=role_va(root);if(h->len<36||h->len>1048576||checksum(h,h->len))return 0;
 for(p=36;p+stride<=h->len;p+=stride){u64 a=stride==8?*(u64 *)((u8 *)h+p):*(u32 *)((u8 *)h+p);if(a>=0x100000000UL)continue;struct acpi_header *t=role_va(a);if(!memcmp(t->sig,"APIC",4)&&t->len>=44&&t->len<1048576&&!checksum(t,t->len))return t;}
 return 0;
}
static void delay_us(unsigned us){u64 start=role_tsc(),cycles=(role_tsc_hz/1000000)*us;while(role_tsc()-start<cycles)role_pause();}
static void calibrate_tsc(void)
{
 /* PIT channel 2, one shot at boot. No periodic tick or runtime timer. */
 role_out(0x61,(role_in(0x61)&~2)|1);role_out(0x43,0xb0);role_out(0x42,0x9b);role_out(0x42,0x2e);
 u64 start=role_tsc();while(!(role_in(0x61)&32))role_pause();role_tsc_hz=(role_tsc()-start)*100;
}
void __attribute__((noreturn)) role_ap_entry(unsigned slot)
{
 role_core_init(slot);
 __atomic_store_n(&ap_online,slot,__ATOMIC_RELEASE);
 while(!__atomic_load_n(&roles_released,__ATOMIC_ACQUIRE))role_pause();
 asm volatile("mov %0,%%cr3"::"r"(role_pa(role_pml4)):"memory");
 for(;;)asm volatile("sti; hlt":::"memory");
}
void role_start_aps(void)
{
 calibrate_tsc();
 struct acpi_header *madt=acpi_find();
 if(!madt){role_puts("ROLE_NO_MADT\n");role_halt();}
 memcpy(role_va(0x8000),role_ap_begin,role_ap_end-role_ap_begin);
 unsigned i;u8 bsp=apic_read(0x20)>>24;
 for(i=44;i+2<=madt->len;){
  u8 *e=(u8 *)madt+i;if(e[1]<2||i+e[1]>madt->len)break;
  if(e[0]==0&&e[1]>=8&&(*(u32 *)(e+4)&1)&&e[3]!=bsp&&role_cpus<ROLE_MAX_CPUS){
   unsigned slot=role_cpus;
   *(u64 *)role_va(0x8f00)=role_pa(role_pml4);*(u64 *)role_va(0x8f08)=(u64)&ap_stacks[slot][32768];*(u64 *)role_va(0x8f10)=(u64)role_ap_entry;*(u32 *)role_va(0x8f18)=slot;
   asm volatile("mfence":::"memory");
   apic_write(0x310,(u32)e[3]<<24);apic_write(0x300,0xc500);delay_us(10000);
   apic_write(0x310,(u32)e[3]<<24);apic_write(0x300,0x8500);delay_us(200);
   apic_write(0x310,(u32)e[3]<<24);apic_write(0x300,0x608);delay_us(200);
   if(__atomic_load_n(&ap_online,__ATOMIC_ACQUIRE)!=slot){apic_write(0x310,(u32)e[3]<<24);apic_write(0x300,0x608);}
   u64 deadline=role_tsc()+role_tsc_hz;
   while(__atomic_load_n(&ap_online,__ATOMIC_ACQUIRE)!=slot&&role_tsc()<deadline)role_pause();
   if(ap_online!=slot){role_puts("ROLE_AP_FAILED\n");role_halt();}
   role_cpus++;role_puts("ROLE_AP parked slot=");role_hex(slot);role_puts("\n");
  }
  i+=e[1];
 }
 /* Remove the boot-only low alias once every AP executes high virtual code. */
 role_pml4[0]=0;
 asm volatile("mov %0,%%cr3"::"r"(role_pa(role_pml4)):"memory");
 __atomic_store_n(&roles_released,1,__ATOMIC_RELEASE);
}
/* Freestanding byte adapters; no runtime allocation. */
void *memcpy(void *d,const void *s,size_t n){u8 *p=d;const u8 *q=s;while(n--)*p++=*q++;return d;}
void *memset(void *d,int c,size_t n){u8 *p=d;while(n--)*p++=c;return d;}
int memcmp(const void *a,const void *b,size_t n){const u8 *p=a,*q=b;while(n--){if(*p!=*q)return *p-*q;p++;q++;}return 0;}
