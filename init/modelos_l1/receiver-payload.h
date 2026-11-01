/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/elf.h>
#include "payload.h"
static u32 kexec_fnv(const u8 *p,u64 n)
{
 u32 h=2166136261U;
 for(u64 i=0;i<n;i++){h=(h^p[i])*16777619U;if(!(i&0xfffff))cond_resched();}
 return h;
}
static const void *kexec_section(const u8 *blob,u64 bytes,const char *name,u64 *size)
{
 const Elf64_Ehdr *h=(const void *)blob;const Elf64_Shdr *sh;const char *strings;
 if(bytes<sizeof(*h)||memcmp(h->e_ident,ELFMAG,SELFMAG)||h->e_machine!=190||h->e_shentsize!=sizeof(*sh)||h->e_shoff>bytes||h->e_shnum>(bytes-h->e_shoff)/sizeof(*sh)||h->e_shstrndx>=h->e_shnum)return NULL;
 sh=(const void *)(blob+h->e_shoff);
 if(sh[h->e_shstrndx].sh_offset>bytes||sh[h->e_shstrndx].sh_size>bytes-sh[h->e_shstrndx].sh_offset)return NULL;
 strings=(const void *)(blob+sh[h->e_shstrndx].sh_offset);
 for(unsigned i=0;i<h->e_shnum;i++){
  if(sh[i].sh_name>=sh[h->e_shstrndx].sh_size||!memchr(strings+sh[i].sh_name,0,sh[h->e_shstrndx].sh_size-sh[i].sh_name))return NULL;
  if(!strcmp(strings+sh[i].sh_name,name)){
   if(sh[i].sh_offset>bytes||sh[i].sh_size>bytes-sh[i].sh_offset)return NULL;
   *size=sh[i].sh_size;return blob+sh[i].sh_offset;
  }
 }
 return NULL;
}
static int kexec_payload(phys_addr_t *physical,const u8 **code,u64 *code_bytes)
{
 static bool cached;static phys_addr_t saved_phys;static const u8 *saved_code;static u64 saved_bytes;
 phys_addr_t start,size;const struct modelos_kho_payload *m;const u8 *p,*cubin;u64 text_bytes;
 if(!is_kho_boot())return 0;
 if(cached){*physical=saved_phys;*code=saved_code;*code_bytes=saved_bytes;return 0;}
 if(!reserve_mem_find_by_name(KHO_MODEL_NAME,&start,&size)||size<4096)return -ENODEV;
 p=phys_to_virt(start);m=(const void *)(p+size-4096);
 if(memcmp(m->magic,"Q3KHO001",8)||m->weights_bytes!=KHO_MODEL_BYTES||m->weights_bytes>size-4096||m->cubin_offset>size-4096||m->cubin_bytes>size-4096-m->cubin_offset)return -EINVAL;
 if(kexec_fnv(p,m->weights_bytes)!=m->weights_fnv||kexec_fnv(p+m->cubin_offset,m->cubin_bytes)!=m->cubin_fnv)return -EIO;
 cubin=p+m->cubin_offset;
 *code=kexec_section(cubin,m->cubin_bytes,".text.qwen_persistent",&text_bytes);
 if(!*code)return -EINVAL;
 *physical=start;*code_bytes=text_bytes;
 saved_phys=start;saved_code=*code;saved_bytes=text_bytes;cached=true;
 pr_info("KEXEC_PAYLOAD_PASS phys=%pa weights=%llu cubin=%llu weights_fnv=%08x cubin_fnv=%08x mode=%u old_client=%x\n",&start,m->weights_bytes,m->cubin_bytes,m->weights_fnv,m->cubin_fnv,m->mode,m->old_client);
 return 0;
}
