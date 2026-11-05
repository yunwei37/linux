/* SPDX-License-Identifier: GPL-2.0 */
/* No task, initcall, allocator or timer roots in this profile. */
#include "role.h"
#include <asm/bootparam.h>
#include <linux/efi.h>
#include <linux/string.h>
static struct boot_params role_boot_params;
static u64 ram_bytes;
u64 role_rsdp_address(void)
{
 if(role_boot_params.acpi_rsdp_addr)return role_boot_params.acpi_rsdp_addr;
 const struct efi_info *info=&role_boot_params.efi_info;
 u64 address=(u64)info->efi_systab|((u64)info->efi_systab_hi<<32),fallback=0;
 if(memcmp(&info->efi_loader_signature,"EL64",4)||!address||address>ROLE_DIRECT_LIMIT-sizeof(efi_system_table_64_t))return 0;
 const efi_system_table_64_t *system=role_va(address);
 if(system->hdr.signature!=EFI_SYSTEM_TABLE_SIGNATURE||system->nr_tables>4096||!system->tables||system->tables>ROLE_DIRECT_LIMIT-system->nr_tables*sizeof(efi_config_table_64_t))return 0;
 const efi_config_table_64_t *tables=role_va(system->tables);
 static const efi_guid_t acpi20=ACPI_20_TABLE_GUID,acpi10=ACPI_TABLE_GUID;
 for(unsigned i=0;i<system->nr_tables;i++){
  if(!memcmp(&tables[i].guid,&acpi20,sizeof(acpi20)))return tables[i].table;
  if(!memcmp(&tables[i].guid,&acpi10,sizeof(acpi10)))fallback=tables[i].table;
 }
 return fallback;
}
int role_ram_contains(u64 start,u64 bytes)
{
 unsigned i;if(!bytes||start+bytes<start)return 0;
 for(i=0;i<role_boot_params.e820_entries;i++){
  const struct boot_e820_entry *e=&role_boot_params.e820_table[i];
  if(e->type==1&&start>=e->addr&&start-e->addr<=e->size&&bytes<=e->size-(start-e->addr))return 1;
 }
 return 0;
}
static u64 highest_ram;
void role_memory_init(void)
{
 extern char _end[],_text[];unsigned i;u64 kernel_end=role_pa(_end),kernel_begin=role_pa(_text);
 for(i=0;i<role_boot_params.e820_entries;i++){
  const struct boot_e820_entry *e=&role_boot_params.e820_table[i];
  if(e->type!=1||e->addr>=ROLE_DIRECT_LIMIT)continue;
  u64 end=e->size>ROLE_DIRECT_LIMIT-e->addr?ROLE_DIRECT_LIMIT:e->addr+e->size;
  if(end>highest_ram)highest_ram=end;
  u64 begin=e->addr<0x100000?0x100000:e->addr;
  if(begin<kernel_begin&&end>begin){u64 n=end<kernel_begin?end:kernel_begin;if(mos_role_memory_add_ram(begin,n-begin))role_runtime_fault();}
  if(end>kernel_end){begin=begin<kernel_end?kernel_end:begin;if(begin<end&&mos_role_memory_add_ram(begin,end-begin))role_runtime_fault();}
 }
 if(highest_ram>0x100000000UL){
  u64 address=(highest_ram-256)&~63UL;
  if(mos_role_high_ram_probe(address))role_runtime_fault();
  role_puts("ROLE_HIGH_RAM_OBJECT_PASS address=");role_hex(address);role_puts(" bytes=137 freed=1\n");
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
