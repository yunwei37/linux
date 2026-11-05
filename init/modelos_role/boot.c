/* SPDX-License-Identifier: GPL-2.0 */
/* No task, initcall, allocator or timer roots in this profile. */
#include "role.h"
#include <asm/bootparam.h>
static struct boot_params role_boot_params;
static u64 ram_bytes;
int role_ram_contains(u64 start,u64 bytes)
{
 unsigned i;if(!bytes||start+bytes<start)return 0;
 for(i=0;i<role_boot_params.e820_entries;i++){
  const struct boot_e820_entry *e=&role_boot_params.e820_table[i];
  if(e->type==1&&start>=e->addr&&start-e->addr<=e->size&&bytes<=e->size-(start-e->addr))return 1;
 }
 return 0;
}
void role_memory_init(void)
{
 extern char _end[];unsigned i;u64 kernel_end=role_pa(_end);
 for(i=0;i<role_boot_params.e820_entries;i++){
  const struct boot_e820_entry *e=&role_boot_params.e820_table[i];
  if(e->type!=1||e->addr>=0x100000000UL)continue;
  u64 end=e->size>0x100000000UL-e->addr?0x100000000UL:e->addr+e->size;
  u64 begin=e->addr<0x100000?0x100000:e->addr;
  if(begin<0x1000000UL&&end>begin){u64 n=end<0x1000000UL?end:0x1000000UL;if(mos_role_memory_add_ram(begin,n-begin))role_runtime_fault();}
  if(end>kernel_end){begin=begin<kernel_end?kernel_end:begin;if(begin<end&&mos_role_memory_add_ram(begin,end-begin))role_runtime_fault();}
 }
}
void __attribute__((noreturn)) start_kernel(void *physical_boot_params)
{
 unsigned i;
 struct boot_params *bp=role_va((u64)physical_boot_params);
 role_boot_params=*bp;
 role_core_init(0);
 role_puts("ROLE_BOOT start_kernel Linux boot; no rest_init\n");
 for(i=0;i<role_boot_params.e820_entries;i++)
  if(role_boot_params.e820_table[i].type==1)ram_bytes+=role_boot_params.e820_table[i].size;
 role_puts("ROLE_RAM bytes=");role_hex(ram_bytes);role_puts("\n");
 role_start_aps();
 role_puts("ROLE_READY fixed cores=");role_hex(role_cpus);role_puts("\n");
 role_service();
}
