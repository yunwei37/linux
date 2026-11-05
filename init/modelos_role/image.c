#include "role-step.h"
// SPDX-License-Identifier: GPL-2.0
#include "image.h"
extern u64 mos_core_role_expert_bind(const void *,u64);
extern int mos_core_role_expert_check(void);
static u64 expert_args;
#include <linux/string.h>
#include "strata-storage.h"
u64 role_linear_phys,role_table_phys,role_tokenizer_phys,role_tokenizer_bytes;
const u32 *role_fixture_input,*role_fixture_output;
unsigned role_fixture_inputs,role_fixture_outputs;
static struct role_image_header image __attribute__((aligned(512)));
static u32 image_crc(const void *buffer,size_t n)
{
 const u8 *p=buffer;u32 crc=~0U;unsigned bit;
 while(n--){crc^=*p++;for(bit=0;bit<8;bit++)crc=(crc>>1)^(0xedb88320U&-(crc&1));}
 return ~crc;
}
struct range {u64 start,bytes;};
static struct range ranges[8];
static unsigned range_count;
static int claim_range(u64 start,u64 bytes)
{
 extern char _end[];unsigned i;
 if(!bytes||bytes>0x100000000UL||start>0x100000000UL-bytes||
    start<role_pa(_end)||!role_ram_contains(start,bytes))return -22;
 for(i=0;i<range_count;i++)if(start<ranges[i].start+ranges[i].bytes&&ranges[i].start<start+bytes)return -22;
 if(range_count>=8)return -22;ranges[range_count++]=(struct range){start,bytes};return 0;
}
int role_image_load(void)
{
 struct mos_strata_disk *disk;u32 crc;unsigned i;int ret;
 ret=mos_strata_disk_open("vda",&disk);if(ret)return ret;
 ret=mos_strata_disk_read(disk,0,&image,sizeof(image));if(ret)goto out;
 crc=image.crc32;image.crc32=0;
 if(memcmp(image.magic,"MOSROLE1",8)||image.version!=1||image.segments!=3||image.arenas!=2||
    image.cpus!=role_cpus||image.reserved||image_crc(&image,sizeof(image))!=crc){ret=-22;goto out;}
 role_memory_init();
 /* Validate every destination before permitting any Image DMA. */
 for(i=0;i<image.arenas;i++){
  struct role_image_arena *a=&image.arena[i];
  if(a->reserved||a->physical&0x1fffff||claim_range(a->physical,a->bytes)){ret=-22;goto out;}
  if(mos_role_memory_import(a->physical,a->bytes,4)){ret=-22;goto out;}
  if(a->kind==1&&a->bytes==(512UL<<20)&&!role_linear_phys)role_linear_phys=a->physical;
  else if(a->kind==2&&a->bytes==(4UL<<20)&&!role_table_phys)role_table_phys=a->physical;
  else{ret=-22;goto out;}
 }
 for(i=0;i<image.segments;i++){
  struct role_image_segment *s=&image.segment[i];
  if(!s->bytes||s->bytes>(16UL<<20)||!s->sector||s->physical&511||
     claim_range(s->physical,(s->bytes+511)&~511UL)){ret=-22;goto out;}
  if(mos_role_memory_import(s->physical,(s->bytes+511)&~511UL,4)){ret=-22;goto out;}
  if(s->kind==1&&!role_tokenizer_phys){role_tokenizer_phys=s->physical;role_tokenizer_bytes=s->bytes;}
  else if(s->kind==2&&!role_fixture_input)role_fixture_input=role_va(s->physical);
  else if(s->kind==3&&!expert_args)expert_args=mos_core_role_expert_bind(role_va(s->physical),s->bytes);
  else{ret=-22;goto out;}
 }
 struct mos_core_disk_segment transfers[4];
 for(i=0;i<image.segments;i++){
  struct role_image_segment *s=&image.segment[i];
  transfers[i]=(struct mos_core_disk_segment){s->sector,(u64)role_va(s->physical),s->bytes};
 }
 ret=mos_core_role_disk_bind(disk);if(ret)goto out;
 if(!expert_args){ret=-22;goto out;}
 if(role_reconfigure(1,ROLE_CPU,0)){ret=-5;goto out;}
 ret=mos_core_role_disk_cpu_post(1,transfers,image.segments,1,expert_args,0,1);if(ret)goto out;
 do{ret=mos_core_role_disk_poll();if(!ret)role_pause();}while(!ret);
 if(ret<0)goto out;ret=0;
 role_puts("ROLE_STEP_ACTIVATION disk_ops=post_poll full_fence=1 segments=");role_hex(image.segments);role_puts("\n");
 for(i=0;i<image.segments;i++){
  struct role_image_segment *s=&image.segment[i];void *target=role_va(s->physical);
  if(image_crc(target,s->bytes)!=s->crc32){ret=-5;goto out;}
  if(s->kind==2){
   u32 *data=target;
   if(s->bytes<8||data[0]>2048||data[1]>2048||s->bytes!=8+4UL*(data[0]+data[1])||!data[0]||!data[1]){ret=-22;goto out;}
   role_fixture_inputs=data[0];role_fixture_outputs=data[1];role_fixture_input=data+2;role_fixture_output=data+2+data[0];
  }
 }
 if(role_reconfigure(1,ROLE_IDLE,0)){ret=-5;goto out;}
 ret=mos_core_role_expert_check();if(ret)goto out;
 role_puts("ROLE_STEP_CPU_EXPERT production_rows=source_exact fixed_workers=2 fpu=left_on\n");
 struct mos_strata_io_stats stats;mos_strata_disk_stats(disk,&stats);
 role_puts("ROLE_IMAGE disk=virtio-blk CRC32=pass bytes=");role_hex(stats.read_bytes);role_puts(" reads=");role_hex(stats.read_requests);role_puts("\n");
 role_puts("ROLE_LAYOUT E820 validated; Image initial placement; object allocator over E820; no Linux allocator\n");
out:
 if(ret)mos_strata_disk_close(disk);return ret;
}

void __attribute__((noreturn)) mos_core_role_panic(void){role_puts("ROLE_STEP_PANIC\n");role_halt();}
