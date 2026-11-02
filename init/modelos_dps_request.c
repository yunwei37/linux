// SPDX-License-Identifier: GPL-2.0
/* Resume native DPS start_pos against distinct retained KV arenas. */
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/modelos_dps_handoff.h>
#include <linux/modelos_dps_request.h>
#include "modelos_dps_cases.h"
static volatile u32 *queue;
static u32 serial;
static int __init command(unsigned slot,unsigned inputs,const unsigned *tokens,unsigned position,unsigned count,unsigned offset,unsigned case_id,const char *stage)
{
 const struct dps_case *tc=&dps_cases[case_id];u64 begin=ktime_get_ns(),deadline,gpu_begin,gpu_end,elapsed;u32 ids[128];
 for(unsigned i=0;i<inputs;i++)WRITE_ONCE(queue[128+i],tokens[i]);
 for(unsigned i=0;i<128;i++)WRITE_ONCE(queue[512+i],0xffffffffU);
 WRITE_ONCE(queue[2],inputs);WRITE_ONCE(queue[3],count);WRITE_ONCE(queue[4],position);WRITE_ONCE(queue[6],0);WRITE_ONCE(queue[7],slot);wmb();WRITE_ONCE(queue[0],++serial);wmb();
 deadline=ktime_get_ns()+10000000000ULL;
 while(READ_ONCE(queue[1])!=serial){if(ktime_get_ns()>deadline){pr_err("KEXEC_CONT_TIMEOUT serial=%u ack=%u\n",serial,queue[1]);return -ETIMEDOUT;}udelay(10);cond_resched();}
 rmb();for(unsigned i=0;i<count;i++)ids[i]=READ_ONCE(queue[512+i]);
 gpu_begin=READ_ONCE(*(u64 *)(queue+12));gpu_end=READ_ONCE(*(u64 *)(queue+14));elapsed=ktime_get_ns()-begin;
 pr_info("KEXEC_CONT_CHUNK os=modelos stage=%s case=%u slot=%u position=%u offset=%u count=%u elapsed_ns=%llu gpu_ns=%llu ids=",stage,case_id,slot,position,offset,count,elapsed,gpu_end-gpu_begin);
 for(unsigned i=0;i<count;i++)pr_cont("%s%u",i?",":"",ids[i]);pr_cont("\n");
 if(READ_ONCE(queue[5])||memcmp(ids,tc->output+offset,count*sizeof(u32)))return -EINVAL;
 return 0;
}
static int __init consume(void)
{
 phys_addr_t start,bytes;struct dps_handoff *h;struct dps_request_handoff *requests;struct pci_dev *dev=NULL;u16 pci_command;
 if(!is_kho_boot()||!reserve_mem_find_by_name(DPS_NAME,&start,&bytes)||bytes<DPS_REQUEST_OFFSET+sizeof(*requests))goto reject;
 h=phys_to_virt(start);requests=(void *)h+DPS_REQUEST_OFFSET;
 if(memcmp(h->magic,DPS_MAGIC,8)||h->version!=1||h->bytes!=sizeof(*h)||h->checksum!=dps_checksum(h)||h->probe_status!=1||h->copy_mode||h->serial!=DPS_REQUEST_TARGET_SERIAL||(h->queue_phys&4095)||!pfn_valid(h->queue_phys>>PAGE_SHIFT)||!PageReserved(pfn_to_page(h->queue_phys>>PAGE_SHIFT)))goto reject;
 queue=phys_to_virt(h->queue_phys);serial=h->serial;
 if(queue[0]!=serial||queue[1]!=serial||queue[17]!=serial||queue[5])goto reject;
 if(memcmp(requests->magic,DPS_REQUEST_MAGIC,8)||requests->version!=1||requests->bytes!=sizeof(*requests)||requests->count!=6||(requests->split!=8&&requests->split!=32)||requests->reserved||requests->checksum!=dps_request_checksum(requests))goto reject;
 for(unsigned c=0;c<6;c++){
  struct dps_request_cursor *cursor=&requests->request[c];const struct dps_case *tc=&dps_cases[c];
  if(cursor->id!=c+1||cursor->generation!=1||cursor->slot!=c||cursor->case_id!=c||cursor->emitted!=requests->split||cursor->next_pos!=tc->inputs+cursor->emitted-1||cursor->last_token!=tc->output[cursor->emitted-1]||cursor->budget!=tc->outputs-cursor->emitted){
   pr_err("KEXEC_CONT_REJECT reason=cursor case=%u command=%u ack=%u\n",c,queue[0],queue[1]);return -EINVAL;
  }
 }
 while((dev=pci_get_device(PCI_VENDOR_ID_NVIDIA,PCI_ANY_ID,dev)))if((dev->class>>16)==PCI_BASE_CLASS_DISPLAY)break;
 if(!dev)goto reject;pci_read_config_word(dev,PCI_COMMAND,&pci_command);pci_dev_put(dev);
 if(!(pci_command&PCI_COMMAND_MASTER))goto reject;
 pr_info("KEXEC_CONT_VALIDATED requests=6 split=%u serial=%u mm=%s\n",requests->split,serial,current->mm?"present":"none");
 for(unsigned c=0;c<6;c++){
  struct dps_request_cursor *cursor=&requests->request[c];
  if(command(c,1,&cursor->last_token,cursor->next_pos,1,cursor->emitted,c,"next"))goto reject;
  cursor->last_token=READ_ONCE(queue[512]);cursor->emitted++;cursor->next_pos++;cursor->budget--;
 }
 for(unsigned c=0;c<6;c++){
  struct dps_request_cursor *cursor=&requests->request[c];
  if(command(c,1,&cursor->last_token,cursor->next_pos,cursor->budget,cursor->emitted,c,"suffix"))goto reject;
 }
 for(unsigned c=0;c<6;c++){
  const struct dps_case *tc=&dps_cases[c];
  if(command(6,tc->inputs,tc->input,0,requests->split+1,0,c,"recompute"))goto reject;
 }
 pr_info("KEXEC_CONT_PASS requests=6 split=%u no_rm=1 gpu_slots_retained=7 resources=quarantined_until_vm_exit\n",requests->split);return 0;
reject:pr_err("KEXEC_CONT_REJECT reason=state\n");return -EINVAL;
}
late_initcall(consume);
