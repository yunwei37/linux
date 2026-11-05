/* SPDX-License-Identifier: GPL-2.0 */
#include "role.h"
#include <linux/string.h>
unsigned role_cpus=1;
u64 role_tsc_hz;
static u64 gdt[ROLE_MAX_CPUS][5] __attribute__((aligned(16)));
struct role_tss {u32 reserved;u64 rsp[3];u64 reserved2;u64 ist[7];u64 reserved3;u16 reserved4,iomap;} __attribute__((packed));
static struct role_tss tss[ROLE_MAX_CPUS];
static char fault_stacks[ROLE_MAX_CPUS][8192] __attribute__((aligned(16)));
static char ap_stacks[ROLE_MAX_CPUS][524288] __attribute__((aligned(16)));
struct descriptor {u16 limit;u64 base;} __attribute__((packed));
struct gate {u16 low,selector;u8 ist,type;u16 mid;u32 high,zero;} __attribute__((packed));
static struct gate idt[256] __attribute__((aligned(16)));
extern char role_fault_stubs[],role_ap_begin[],role_ap_end[];
extern void role_wake_isr(void);
static volatile unsigned ap_online,roles_released;
struct role_mailbox {
 u64 posted,done,boundary;u32 operation,role;
 u64 source,destination,bytes;
} __attribute__((aligned(64)));
static struct role_mailbox mailboxes[ROLE_MAX_CPUS];
static u8 apic_ids[ROLE_MAX_CPUS];
static u32 apic_read(unsigned reg){return *(volatile u32 *)role_va(0xfee00000+reg);}
static void apic_write(unsigned reg,u32 value){*(volatile u32 *)role_va(0xfee00000+reg)=value;}
void role_puts(const char *s){while(*s){while(!(role_in(0x3fd)&32))role_pause();role_out(0x3f8,*s++);}}
void role_hex(u64 n){char out[19]="0x0000000000000000";int i;for(i=17;i>=2;i--){out[i]="0123456789abcdef"[n&15];n>>=4;}role_puts(out);}
static unsigned fault_print;
void __attribute__((noreturn)) role_fault(u64 *frame){while(__atomic_exchange_n(&fault_print,1,__ATOMIC_ACQUIRE))role_pause();role_puts("ROLE_EXCEPTION vector/error/rip/cr2=");role_hex(frame[0]);role_hex(frame[1]);role_hex(frame[2]);u64 cr2;asm volatile("mov %%cr2,%0":"=r"(cr2));role_hex(cr2);role_puts(" halted\n");__atomic_store_n(&fault_print,0,__ATOMIC_RELEASE);role_halt();}
static void gate(unsigned n,u64 target,unsigned ist){idt[n]=(struct gate){target,8,ist,0x8e,target>>16,target>>32,0};}
void role_core_init(unsigned slot)
{
 u64 base=(u64)&tss[slot];
 struct descriptor gd={sizeof(gdt[slot])-1,(u64)gdt[slot]},id={sizeof(idt)-1,(u64)idt};
 gdt[slot][1]=0x00af9a000000ffffUL;gdt[slot][2]=0x00cf92000000ffffUL;
 tss[slot].ist[0]=(u64)&fault_stacks[slot][sizeof(fault_stacks[slot])];tss[slot].iomap=sizeof(tss[slot]);
 gdt[slot][3]=103|((base&0xffffff)<<16)|(0x89UL<<40)|((base>>24&255)<<56);gdt[slot][4]=base>>32;
 asm volatile("lgdt %0"::"m"(gd):"memory");
 asm volatile("pushq $8; leaq 1f(%%rip),%%rax; pushq %%rax; lretq; 1:":::"rax","memory");
 asm volatile("mov $16,%%ax; mov %%ax,%%ds; mov %%ax,%%es; mov %%ax,%%ss; mov $24,%%ax; ltr %%ax":::"rax","memory");
 if(!slot){unsigned i;for(i=0;i<256;i++)gate(i,(u64)role_fault_stubs+13*16,1);for(i=0;i<32;i++)gate(i,(u64)role_fault_stubs+i*16,1);gate(0xf1,(u64)role_wake_isr,0);}
 asm volatile("lidt %0"::"m"(id):"memory");
 /* PAT slot 4 is WC for the fixed GPU BAR1 direct-map aperture. */
 u32 pl,ph;asm volatile("rdmsr":"=a"(pl),"=d"(ph):"c"(0x277));ph=(ph&~255U)|1;asm volatile("wrmsr"::"c"(0x277),"a"(pl),"d"(ph));
 if(mos_role_enable_fpu())role_runtime_fault();
 apic_write(0xf0,0x1ff);apic_write(0x320,0x10000);apic_write(0x350,slot?0x10000:0x700);apic_write(0x360,0x400);
}
/* Read firmware topology, not Linux CPU/task discovery. */
static u8 checksum(const void *p,unsigned n){const u8 *b=p;u8 s=0;while(n--)s+=*b++;return s;}
struct acpi_header {char sig[4];u32 len;u8 rest[28];} __attribute__((packed));
static struct acpi_header *acpi_find(void)
{
 extern u64 role_rsdp_address(void);
 u64 p,address=role_rsdp_address();u8 *rsdp=address&&address<ROLE_DIRECT_LIMIT?role_va(address):0;
 for(p=0xe0000;!rsdp&&p<0x100000;p+=16){u8 *b=role_va(p);if(!memcmp(b,"RSD PTR ",8)&&!checksum(b,20)){rsdp=b;break;}}
 role_puts("ROLE_ACPI RSDP=");role_hex(rsdp?role_pa(rsdp):0);role_puts("\n");
 if(!rsdp||memcmp(rsdp,"RSD PTR ",8)||checksum(rsdp,20))return 0;
 u64 root=*(u32 *)(rsdp+16);unsigned stride=4;
 if(rsdp[15]>=2&&!checksum(rsdp,36)&&*(u64 *)(rsdp+24)){root=*(u64 *)(rsdp+24);stride=8;}
 if(!root||root>ROLE_DIRECT_LIMIT-sizeof(struct acpi_header))return 0;
 struct acpi_header *h=role_va(root);if(h->len<36||h->len>1048576||checksum(h,h->len))return 0;
 for(p=36;p+stride<=h->len;p+=stride){u64 a=stride==8?*(u64 *)((u8 *)h+p):*(u32 *)((u8 *)h+p);if(!a||a>ROLE_DIRECT_LIMIT-sizeof(struct acpi_header))continue;struct acpi_header *t=role_va(a);if(!memcmp(t->sig,"APIC",4)&&t->len>=44&&t->len<1048576&&!checksum(t,t->len))return t;}
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
 struct role_mailbox *m=&mailboxes[slot];
 for(;;){
  asm volatile("cli":::"memory");
  u64 posted=__atomic_load_n(&m->posted,__ATOMIC_ACQUIRE);
  if(posted!=__atomic_load_n(&m->done,__ATOMIC_RELAXED)){
   if(m->operation==1)m->role=m->bytes;
   else if(m->operation==2&&m->role==ROLE_CPU)memcpy(role_va(m->destination),role_va(m->source),m->bytes);
   else if(m->operation==3&&m->role==ROLE_CPU)((void (*)(void *))m->source)((void *)m->destination);
   else role_runtime_fault();
   __atomic_store_n(&m->done,posted,__ATOMIC_RELEASE);
  }else asm volatile("sti; hlt":::"memory");
 }

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
   unsigned slot=role_cpus;apic_ids[slot]=e[3];
   *(u64 *)role_va(0x8f00)=role_pa(role_pml4);*(u64 *)role_va(0x8f08)=(u64)&ap_stacks[slot][524288];*(u64 *)role_va(0x8f10)=(u64)role_ap_entry;*(u32 *)role_va(0x8f18)=slot;
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
void *memset(void *d,int c,size_t n)
{
 void *result=d;u64 word=(u8)c;word*=0x0101010101010101UL;
 size_t head=(-(u64)d)&7;if(head>n)head=n;size_t count=head;
 asm volatile("rep stosb":"+D"(d),"+c"(count):"a"((u8)c):"memory");
 n-=head;count=n/8;asm volatile("rep stosq":"+D"(d),"+c"(count):"a"(word):"memory");
 count=n&7;asm volatile("rep stosb":"+D"(d),"+c"(count):"a"((u8)c):"memory");return result;
}
int memcmp(const void *a,const void *b,size_t n){const u8 *p=a,*q=b;while(n--){if(*p!=*q)return *p-*q;p++;q++;}return 0;}

void __attribute__((noreturn)) role_runtime_fault(void){role_puts("ROLE_RUNTIME_ERROR\n");role_halt();}
static int role_publish(unsigned cpu,unsigned operation,u64 source,u64 destination,u64 bytes,u64 boundary)
{
 if(!cpu||cpu>=role_cpus)return -22;
 struct role_mailbox *m=&mailboxes[cpu];
 u64 seq=__atomic_load_n(&m->posted,__ATOMIC_RELAXED);
 if(seq!=__atomic_load_n(&m->done,__ATOMIC_ACQUIRE)||boundary<m->boundary||seq==~0UL)return -16;
 if(operation>1&&m->role!=ROLE_CPU)return -22;
 m->operation=operation;m->source=source;m->destination=destination;m->bytes=bytes;m->boundary=boundary;
 __atomic_store_n(&m->posted,seq+1,__ATOMIC_RELEASE);
 while(apic_read(0x300)&(1<<12))role_pause();
 apic_write(0x310,(u32)apic_ids[cpu]<<24);apic_write(0x300,0xf1);
 return 0;
}
int role_cpu_done(unsigned cpu){return cpu&&cpu<role_cpus&&__atomic_load_n(&mailboxes[cpu].done,__ATOMIC_ACQUIRE)==__atomic_load_n(&mailboxes[cpu].posted,__ATOMIC_RELAXED);}
static int role_command(unsigned cpu,unsigned operation,u64 source,u64 destination,u64 bytes,u64 boundary)
{
 int rc=role_publish(cpu,operation,source,destination,bytes,boundary);if(rc)return rc;
 struct role_mailbox *m=&mailboxes[cpu];u64 seq=__atomic_load_n(&m->posted,__ATOMIC_RELAXED);
 u64 deadline=role_tsc()+role_tsc_hz;
 while(__atomic_load_n(&m->done,__ATOMIC_ACQUIRE)!=seq){if(role_tsc()>deadline)return -5;role_pause();}
 return 0;
}
int role_reconfigure(unsigned cpu,enum role_kind role,u64 boundary)
{
 if(role!=ROLE_IDLE&&role!=ROLE_CPU)return -22;
 int ret=role_command(cpu,1,0,0,role,boundary);
 if(!ret){role_puts("ROLE_CHANGE cpu=");role_hex(cpu);role_puts(" role=");role_hex(role);role_puts(" boundary=");role_hex(boundary);role_puts("\n");}return ret;
}
int role_cpu_copy(unsigned cpu,u64 source,u64 destination,u64 bytes,u64 boundary)
{
 if(!role_ram_contains(source,bytes)||!role_ram_contains(destination,bytes))return -22;
 return role_command(cpu,2,source,destination,bytes,boundary);
}
static u64 step_boundary;
u64 role_next_boundary(void){return ++step_boundary;}
int role_cpu_post(unsigned cpu,void (*fn)(void *),void *arg,u64 boundary){return role_publish(cpu,3,(u64)fn,(u64)arg,0,boundary);}
void role_step_boundary(void)
{
 u64 boundary=role_next_boundary();u64 move[3],free_before=mos_role_memory_free_bytes();
 unsigned cpu=2+(boundary-1)%(role_cpus-2);
 if(mos_role_move_begin(move)||role_reconfigure(cpu,ROLE_CPU,boundary)||
    role_cpu_copy(cpu,move[0],move[1],move[2],boundary)||mos_role_move_finish()||
    role_reconfigure(cpu,ROLE_IDLE,boundary)||mos_role_memory_free_bytes()!=free_before)role_runtime_fault();
 role_puts("ROLE_OBJECT_MOVE_PASS bytes=137 freed=1 cpu_op=1\n");
}

u64 role_now_ns(void){u64 t=role_tsc();return t/role_tsc_hz*1000000000+(t%role_tsc_hz)*1000000000/role_tsc_hz;}
void __attribute__((noreturn)) mos_net_panic(void){role_puts("ROLE_NET_ERROR\n");role_halt();}
int role_nic_arm(void);
void role_nic_disarm(void);
u64 role_net_wait(u64 deadline_ns)
{
 asm volatile("cli":::"memory");
 u64 now=role_now_ns(),begin=role_tsc();
 if(deadline_ns<=now)return 0;
 if(!role_nic_arm()){role_nic_disarm();return 0;}
 u32 low,high;
 /* No periodic clock: program an absolute one-shot only for a TCP deadline. */
 if(deadline_ns!=~0UL){
  u64 delta=deadline_ns-now;
  u64 cycles=(delta/1000000000)*role_tsc_hz+(delta%1000000000)*role_tsc_hz/1000000000;
  u64 deadline=role_tsc()+cycles;low=deadline;high=deadline>>32;
  apic_write(0x320,0x40000|0xf1);asm volatile("wrmsr"::"c"(0x6e0),"a"(low),"d"(high));
 }
 asm volatile("sti; hlt; cli":::"memory");
 low=high=0;asm volatile("wrmsr"::"c"(0x6e0),"a"(low),"d"(high));apic_write(0x320,0x10000);
 role_nic_disarm();return role_tsc()-begin;
}

/* Boot-only device cache attributes. A doorbell and framebuffer can share
 * one 1GiB aperture; split that interval using a fixed table pool so UC MMIO
 * never changes the adjacent 2MiB WC framebuffer mappings. No page allocator. */
static u64 mmio_pmd[128][512] __attribute__((aligned(4096)));
static unsigned mmio_pmd_used;
void *role_mmio_map(u64 p,u64 bytes,bool wc)
{
 extern u64 role_puds[];if(!bytes||p>=ROLE_DIRECT_LIMIT||bytes>ROLE_DIRECT_LIMIT-p)return NULL;
 u64 last=p+bytes-1;
 for(u64 i=p>>30;i<=(last>>30);i++){
  if(role_puds[i]&0x80){
   if(mmio_pmd_used==128)return NULL;
   u64 *table=mmio_pmd[mmio_pmd_used++],base=i<<30;
   for(unsigned j=0;j<512;j++)table[j]=base+((u64)j<<21)|0x83;
   asm volatile("mfence":::"memory");role_puds[i]=role_pa(table)|3;
  }
  u64 *table=role_va(role_puds[i]&~4095UL);
  unsigned begin=i==(p>>30)?(p>>21)&511:0,end=i==(last>>30)?(last>>21)&511:511;
  for(unsigned j=begin;j<=end;j++)table[j]=(table[j]&~(0x18UL|0x1000UL))|(wc?0x1000UL:0x18UL);
 }
 asm volatile("mov %0,%%cr3"::"r"(role_pa(role_pml4)):"memory");return role_va(p);
}
