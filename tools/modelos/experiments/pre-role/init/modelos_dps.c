// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include "modelos_dps_handoff.h"
#include "modelos_dps_cases.h"
void modelos_buddy_arena_admit(void);
int modelos_buddy_arena_release(void);
static int __init consume(void)
{
 phys_addr_t start,bytes;struct dps_handoff *h;volatile u32 *q;struct pci_dev *dev=NULL;u16 command;
 if(!is_kho_boot()||!reserve_mem_find_by_name(DPS_NAME,&start,&bytes)||bytes<sizeof(*h))goto reject;
 h=phys_to_virt(start);
 if(memcmp(h->magic,DPS_MAGIC,8)||h->version!=1||h->bytes!=sizeof(*h)||h->checksum!=dps_checksum(h)||h->probe_status!=1||h->copy_mode>1||h->serial!=26||(h->queue_phys&4095)||!pfn_valid(h->queue_phys>>PAGE_SHIFT)||!PageReserved(pfn_to_page(h->queue_phys>>PAGE_SHIFT)))goto reject;
 q=phys_to_virt(h->queue_phys);if(q[0]!=h->serial||q[1]!=h->serial||q[5])goto reject;
 while((dev=pci_get_device(PCI_VENDOR_ID_NVIDIA,PCI_ANY_ID,dev)))if((dev->class>>16)==PCI_BASE_CLASS_DISPLAY)break;
 if(!dev)goto reject;pci_read_config_word(dev,PCI_COMMAND,&command);pci_dev_put(dev);
 if(!(command&PCI_COMMAND_MASTER))goto reject;
 pr_info("KEXEC_DPS_VALIDATED copy=%u serial=%u pci_command=%04x mm=%s\n",h->copy_mode,h->serial,command,current->mm?"present":"none");
 modelos_buddy_arena_admit();
 for(unsigned repeat=0;repeat<4;repeat++)for(unsigned c=0;c<ARRAY_SIZE(dps_cases);c++){
  const struct dps_case *tc=&dps_cases[c];u64 begin=ktime_get_ns(),deadline,gpu_begin,gpu_end,elapsed;u32 ids[128];
  for(unsigned i=0;i<tc->inputs;i++)WRITE_ONCE(q[128+i],tc->input[i]);
  WRITE_ONCE(q[2],tc->inputs);WRITE_ONCE(q[3],tc->outputs);wmb();WRITE_ONCE(q[0],++h->serial);wmb();
  deadline=ktime_get_ns()+10000000000ULL;
  while(READ_ONCE(q[1])!=h->serial){if(ktime_get_ns()>deadline){pr_err("KEXEC_DPS_TIMEOUT serial=%u ack=%u\n",h->serial,q[1]);return -ETIMEDOUT;}udelay(10);cond_resched();}
  rmb();for(unsigned i=0;i<tc->outputs;i++)ids[i]=READ_ONCE(q[512+i]);
  gpu_begin=READ_ONCE(*(u64 *)(q+12));gpu_end=READ_ONCE(*(u64 *)(q+14));elapsed=ktime_get_ns()-begin;
  if(q[5]||memcmp(ids,tc->output,tc->outputs*sizeof(u32)))goto reject;
  pr_info("KEXEC_DPS_REQUEST os=modelos case=%u repeat=%u count=%u elapsed_ns=%llu gpu_ns=%llu ids=",c,repeat,tc->outputs,elapsed,gpu_end-gpu_begin);
  for(unsigned i=0;i<tc->outputs;i++)pr_cont("%s%u",i?",":"",ids[i]);pr_cont("\n");
 }
 if(modelos_buddy_arena_release())goto reject;
 pr_info("KEXEC_DPS_PASS requests=24 tokens=1792 no_rm=1 resources=quarantined_until_vm_exit\n");return 0;
reject:pr_err("KEXEC_DPS_REJECT\n");return -EINVAL;
}
late_initcall(consume);
