/* SPDX-License-Identifier: GPL-2.0 */
/* Hardware adapter for the SAME drivers/modelos-net split-ring implementation.
 * PCI config cycles and model-object RAM replace the Linux PCI/DMA lifetimes. */
#ifndef MODELOS_ROLE_NIC_PLATFORM_H
#define MODELOS_ROLE_NIC_PLATFORM_H
#include "role.h"
#include "net.h"
#include <linux/modelos_net.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/log2.h>
#include <linux/io.h>
#include <linux/virtio_config.h>
#define PCI_CAP_ID_VNDR 9
#define PCI_STD_NUM_BARS 6
typedef u64 dma_addr_t;
struct pci_dev {u32 bdf;u64 bar[6],length[6];u32 flags[6];unsigned dev;};
static struct pci_dev role_nic_device;
u64 mos_role_dma_alloc(u64,u64);
int mos_role_dma_free(u64);
static u32 role_cfg_read(u32 bdf,unsigned off)
{
 u32 v,address=0x80000000U|(bdf<<8)|(off&~3U);
 asm volatile("outl %0,%1"::"a"(address),"Nd"((u16)0xcf8));
 asm volatile("inl %1,%0":"=a"(v):"Nd"((u16)0xcfc));return v;
}
static void role_cfg_write(u32 bdf,unsigned off,u32 value)
{
 u32 address=0x80000000U|(bdf<<8)|(off&~3U);
 asm volatile("outl %0,%1"::"a"(address),"Nd"((u16)0xcf8));
 asm volatile("outl %0,%1"::"a"(value),"Nd"((u16)0xcfc));
}
static void pci_read_config_dword(struct pci_dev *p,unsigned off,u32 *v){*v=role_cfg_read(p->bdf,off);}
static void pci_read_config_byte(struct pci_dev *p,unsigned off,u8 *v){*v=role_cfg_read(p->bdf,off)>>((off&3)*8);}
static struct pci_dev *pci_get_device(u16 vendor,u16 device,void *unused)
{
 struct pci_dev *p=&role_nic_device;u32 bdf;
 for(bdf=0;bdf<65536;bdf++)if(role_cfg_read(bdf,0)==((u32)device<<16|vendor))break;
 if(bdf==65536)return NULL;memset(p,0,sizeof(*p));p->bdf=bdf;
 u32 command=role_cfg_read(bdf,4)&0xffff;role_cfg_write(bdf,4,command&~3U);
 for(unsigned i=0;i<6;i++){
  unsigned off=16+i*4;u32 low=role_cfg_read(bdf,off);if(!low)continue;
  bool wide=!(low&1)&&((low>>1)&3)==2;u32 high=wide?role_cfg_read(bdf,off+4):0;
  role_cfg_write(bdf,off,~0U);if(wide)role_cfg_write(bdf,off+4,~0U);
  u64 mask=role_cfg_read(bdf,off);if(wide)mask|=(u64)role_cfg_read(bdf,off+4)<<32;
  role_cfg_write(bdf,off,low);if(wide)role_cfg_write(bdf,off+4,high);
  u64 bits=low&1?~3UL:~15UL;mask&=bits;
  if(!wide)mask|=0xffffffff00000000UL;
  p->bar[i]=(((u64)high<<32)|low)&bits;p->length[i]=~mask+1;p->flags[i]=low&1?IORESOURCE_IO:IORESOURCE_MEM;
  if(wide)i++;
 }
 role_cfg_write(bdf,4,command);return p;
}
static int pci_find_next_capability(struct pci_dev *p,unsigned cap,unsigned id)
{
 u8 next;unsigned n=0;pci_read_config_byte(p,cap?cap+1:0x34,&next);
 while(next>=0x40&&next<0xff&&n++<48){u8 type;pci_read_config_byte(p,next,&type);if(type==id)return next;pci_read_config_byte(p,next+1,&next);}return 0;
}
static int pci_find_capability(struct pci_dev *p,unsigned id){return pci_find_next_capability(p,0,id);}
static int pci_enable_device(struct pci_dev *p){role_cfg_write(p->bdf,4,(role_cfg_read(p->bdf,4)&0xffff)|3);return 0;}
static void pci_disable_device(struct pci_dev *p){role_cfg_write(p->bdf,4,(role_cfg_read(p->bdf,4)&0xffff)&~7U);}
static void pci_set_master(struct pci_dev *p){role_cfg_write(p->bdf,4,(role_cfg_read(p->bdf,4)&0xffff)|4);}
static void pci_clear_master(struct pci_dev *p){role_cfg_write(p->bdf,4,(role_cfg_read(p->bdf,4)&0xffff)&~4U);}
static void pci_intx(struct pci_dev *p,int enable){u32 c=role_cfg_read(p->bdf,4)&0xffff;role_cfg_write(p->bdf,4,enable?c&~0x400U:c|0x400U);}
static u64 pci_resource_flags(struct pci_dev *p,unsigned bar){return p->flags[bar];}
static u64 pci_resource_len(struct pci_dev *p,unsigned bar){return p->length[bar];}
#define pci_iomap_range role_pci_iomap_range
static void *role_pci_iomap_range(struct pci_dev *p,unsigned bar,u64 offset,u64 bytes)
{
 if(bar>=6||!bytes||offset>p->length[bar]||bytes>p->length[bar]-offset||p->bar[bar]+offset+bytes>0x100000000UL)return NULL;
 return role_va(p->bar[bar]+offset);
}
static volatile u32 *role_msix_table;
static int role_nic_msix_prepare(struct pci_dev *p)
{
 unsigned cap=pci_find_capability(p,17);if(!cap)return -ENOTSUPP;
 u32 table=role_cfg_read(p->bdf,cap+4),control=role_cfg_read(p->bdf,cap);
 role_msix_table=pci_iomap_range(p,table&7,table&~7U,16);if(!role_msix_table)return -EINVAL;
 role_msix_table[3]=1;role_msix_table[0]=0xfee00000|((*(volatile u32 *)role_va(0xfee00020)>>24)<<12);role_msix_table[1]=0;role_msix_table[2]=0xf1;
 asm volatile("mfence":::"memory");role_cfg_write(p->bdf,cap,(control|0x80000000U)&~0x40000000U);
 role_out(0x21,0xff);role_out(0xa1,0xff);
 return 0;
}
static void *role_nic_allocate(void *dev,size_t bytes,dma_addr_t *address,unsigned flags)
{
 *address=mos_role_dma_alloc(bytes,4096);if(!*address)return NULL;
 void *memory=role_va(*address);memset(memory,0,bytes);return memory;
}
static void role_nic_free(void *dev,size_t bytes,void *memory,dma_addr_t address){if(mos_role_dma_free(address))role_runtime_fault();}
#define dma_alloc_coherent role_nic_allocate
#define dma_free_coherent role_nic_free
#define pci_iounmap(p,memory) ((void)0) /* fixed direct map has no map lifetime */
#undef pr_info
#define pr_info(...) role_puts("ROLE_NIC_READY modern split-ring model-object DMA irq-wake=1\n")
#define panic(message,...) do{role_puts(message);role_runtime_fault();}while(0)
static void udelay(unsigned us){u64 end=role_tsc()+role_tsc_hz/1000000*us;while(role_tsc()<end)role_pause();}
#endif
