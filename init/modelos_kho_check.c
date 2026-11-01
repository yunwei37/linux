// SPDX-License-Identifier: GPL-2.0
/* RAM handover check only: this does not preserve an NVIDIA context. */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/kexec_handover.h>
#include <linux/io.h>
static bool expect_warm = true;
module_param(expect_warm,bool,0400);
MODULE_DESCRIPTION("Explicit KHO RAM continuity probe");
static int __init probe_init(void)
{
 phys_addr_t start,size;
 unsigned char *p;
 unsigned long i;
 u32 sum=2166136261U;
 if (!kho_is_enabled() || !reserve_mem_find_by_name("modelos-image",&start,&size))
  return -ENODEV;
 if (expect_warm && !is_kho_boot()) return -EINVAL;
 if(size!=0x200000)return 0;
 p=phys_to_virt(start);
 if (!is_kho_boot())
  for (i=0;i<size;i++) p[i]=(i*13+7)&255;
 for(i=0;i<size;i++) {
  if(p[i]!=((i*13+7)&255)) { pr_err("KEXEC_RAM_FAIL offset=%lu\n",i);return -EINVAL; }
  sum=(sum^p[i])*16777619U;
 }
 pr_info("KEXEC_RAM_PASS kho_boot=%d phys=%pa bytes=%pa fnv32=%08x\n",is_kho_boot(),&start,&size,sum);
 return 0;
}
module_init(probe_init);
MODULE_LICENSE("GPL");
