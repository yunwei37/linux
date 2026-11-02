// SPDX-License-Identifier: GPL-2.0
/* Experimental one-way ownership transfer. No RM construction or reclamation. */
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include "modelos_resident.h"
static const u32 expected[LIVE_CONTINUE]={9756,4280,1119,14473,6973,89492,13,1096};
static int __init consume(void)
{
 phys_addr_t start,bytes;struct live_handoff *h;struct pci_dev *dev=NULL;volatile u32 *q;
 unsigned bad=0,unreserved=0;u16 command=0;u64 deadline,begin;
 if(!is_kho_boot()||!reserve_mem_find_by_name(LIVE_NAME,&start,&bytes)||bytes<sizeof(*h))goto reject;
 h=phys_to_virt(start);
 if(memcmp(h->magic,LIVE_MAGIC,8)||h->version!=1||h->bytes!=sizeof(*h)||h->checksum!=live_checksum(h)||h->samples!=LIVE_SAMPLES||h->mode>1)goto reject;
 for(unsigned i=0;i<h->samples;i++){
  unsigned long pfn=h->page[i].phys>>PAGE_SHIFT;
  if((h->page[i].phys&4095)||!pfn_valid(pfn))goto reject;
  bad+=live_fnv(phys_to_virt(h->page[i].phys),PAGE_SIZE)!=h->page[i].fnv;
  unreserved+=!PageReserved(pfn_to_page(pfn));
 }
 pr_info("KEXEC_LIVE_RAM samples=%u bad=%u unreserved=%u kho_boot=1\n",h->samples,bad,unreserved);
 if(bad||unreserved)goto reject;
 if(!h->mode){pr_info("KEXEC_LIVE_CPU_PASS\n");return 0;}
 if(h->emitted!=8||h->position!=33||h->serial!=33||h->next_token!=16566||h->queue_phys&4095||!pfn_valid(h->queue_phys>>PAGE_SHIFT)||!PageReserved(pfn_to_page(h->queue_phys>>PAGE_SHIFT)))goto reject;
 q=phys_to_virt(h->queue_phys);
 if(memcmp((const void *)q,h->queue,sizeof(h->queue)))goto reject;
 while((dev=pci_get_device(PCI_VENDOR_ID_NVIDIA,PCI_ANY_ID,dev)))if((dev->class>>16)==PCI_BASE_CLASS_DISPLAY)break;
 if(!dev)goto reject;
 pci_read_config_word(dev,PCI_COMMAND,&command);
 pr_info("KEXEC_LIVE_PCI pci=%s before=%04x ",pci_name(dev),command);
 pci_set_master(dev);pci_read_config_word(dev,PCI_COMMAND,&command);pci_dev_put(dev);
 pr_cont("after=%04x\n",command);
 begin=ktime_get_ns();
 for(unsigned i=0;i<LIVE_CONTINUE;i++){
  WRITE_ONCE(q[2],h->next_token);WRITE_ONCE(q[3],h->position++);wmb();WRITE_ONCE(q[0],++h->serial);wmb();
  deadline=ktime_get_ns()+5000000000ULL;
  while(READ_ONCE(q[1])!=h->serial){
   if(ktime_get_ns()>deadline){pr_err("KEXEC_LIVE_TIMEOUT index=%u command=%u ack=%u error=%u\n",i,h->serial,q[1],q[5]);return -ETIMEDOUT;}
   udelay(50);cond_resched();
  }
  rmb();h->next_token=READ_ONCE(q[4]);
  pr_info("KEXEC_LIVE_TOKEN index=%u token=%u expected=%u error=%u elapsed_ns=%llu\n",8+i,h->next_token,expected[i],q[5],ktime_get_ns()-begin);
  if(h->next_token!=expected[i]||q[5])goto reject;
 }
 pr_info("KEXEC_LIVE_GPU_PASS tokens=8 mm=%s no_rm=1 resources=quarantined_until_vm_exit\n",current->mm?"present":"none");
 return 0;
reject:pr_err("KEXEC_LIVE_REJECT\n");return -EINVAL;
}
late_initcall(consume);
