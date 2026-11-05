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
