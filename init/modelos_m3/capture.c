/* SPDX-License-Identifier: GPL-2.0-only
 * Build-time RM capture only. RM stays linked here; this is NOT checkpoint(a).
 * Fixed RAM reservations survive until QEMU teardown. Freed arena objects are
 * not reused, so their identities and snapshots remain available to analysis.
 */
#include <linux/modelos_m3.h>
#include <linux/memblock.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <asm/io.h>
#include <linux/vmalloc.h>
#include <linux/pci.h>
static DEFINE_SPINLOCK(arena_lock);
static DEFINE_SPINLOCK(tape_lock);
static DEFINE_SPINLOCK(mmio_lock);
static u64 low_used,high_used,allocations,frees,exhaustions;
static u64 serial,used,dropped;
static bool frozen,ready;
static bool after_boot_snapshot,have_fb;
static u64 queue_physical;
static u64 post_boot_high;
static u64 next_input_high,input_boundary;
static u8 vram_observation[4096];
static void *mmio_bases[16];
static u64 gpu_bar0,gpu_bar0_bytes=64ULL<<20;
static void __iomem *audit_bar;
static u64 gpu_bar1=0x380000000000ULL,gpu_bar1_bytes=32ULL<<30;
struct m3_record {u64 serial,time,address;u32 kind,bytes;};
void __init modelos_m3_reserve(void)
{
 const u64 ranges[][2]={{M3_LOW_BASE,M3_LOW_SIZE},{M3_HIGH_BASE,M3_HIGH_SIZE},{M3_TAPE_BASE,M3_TAPE_SIZE}};
 for(unsigned i=0;i<ARRAY_SIZE(ranges);i++){
  if(!memblock_is_region_memory(ranges[i][0],ranges[i][1])||memblock_is_region_reserved(ranges[i][0],ranges[i][1]))
   panic("M3 capture physical arena not free RAM: %llx+%llx",ranges[i][0],ranges[i][1]);
  if(memblock_reserve(ranges[i][0],ranges[i][1]))panic("M3 arena reserve failed");
 }
 ready=true;
 pr_info("M3_CAPTURE_ARENAS low=%llx+%llx high=%llx+%llx tape=%llx+%llx\n",M3_LOW_BASE,M3_LOW_SIZE,M3_HIGH_BASE,M3_HIGH_SIZE,M3_TAPE_BASE,M3_TAPE_SIZE);
}
void l2_record(unsigned kind,unsigned long long address,const void*data,unsigned length)
{
 unsigned long flags;struct m3_record r;char*tape=__va(M3_TAPE_BASE);
 if(!ready||READ_ONCE(frozen))return;
 if(kind==5)WRITE_ONCE(queue_physical,address);
 spin_lock_irqsave(&tape_lock,flags);
 if(length>M3_TAPE_SIZE-sizeof(r)||used>M3_TAPE_SIZE-sizeof(r)-length){dropped++;goto out;}
 r=(struct m3_record){++serial,ktime_get_ns(),address,kind,length};
 memcpy(tape+used,&r,sizeof(r));if(length)memcpy(tape+used+sizeof(r),data,length);
 used+=sizeof(r)+length;
out:spin_unlock_irqrestore(&tape_lock,flags);
}
void*modelos_m3_alloc(u64 bytes,u64 alignment,u32 kind,bool dma32)
{
 unsigned long flags;u64 base,offset,*cursor,limit,meta[4];void*p;
 if(!ready||!bytes||!is_power_of_2(alignment))return NULL;
 /* Small RM bookkeeping and DMA32 buffers use the low arena; page-backed
  * allocations use high RAM unless the original request requires DMA32. */
 base=kind!=2||dma32?M3_LOW_BASE:M3_HIGH_BASE;
 cursor=base==M3_LOW_BASE?&low_used:&high_used;limit=base==M3_LOW_BASE?M3_LOW_SIZE:M3_HIGH_SIZE;
 spin_lock_irqsave(&arena_lock,flags);offset=ALIGN(*cursor,alignment);
 if(offset>limit||bytes>limit-offset){exhaustions++;spin_unlock_irqrestore(&arena_lock,flags);pr_err("M3_ARENA_EXHAUSTED kind=%u bytes=%llu align=%llu\n",kind,bytes,alignment);return NULL;}
 *cursor=offset+bytes;allocations++;spin_unlock_irqrestore(&arena_lock,flags);
 p=__va(base+offset);memset(p,0,bytes);
 meta[0]=bytes;meta[1]=alignment;meta[2]=kind;meta[3]=dma32;
 l2_record(13,base+offset,meta,sizeof(meta));return p;
}
bool modelos_m3_owned(const void*p)
{
 unsigned long x=(unsigned long)p,l=(unsigned long)__va(M3_LOW_BASE),h=(unsigned long)__va(M3_HIGH_BASE);
 return(x>=l&&x-l<M3_LOW_SIZE)||(x>=h&&x-h<M3_HIGH_SIZE);
}
void modelos_m3_free(const void*p)
{
 unsigned long flags;if(!p)return;
 if(!modelos_m3_owned(p)){pr_err("M3_FOREIGN_FREE ptr=%px\n",p);return;}
 spin_lock_irqsave(&arena_lock,flags);frees++;spin_unlock_irqrestore(&arena_lock,flags);
 l2_record(14,__pa(p),NULL,0);
}
unsigned long l2_mmio_enter(void){unsigned long flags;spin_lock_irqsave(&mmio_lock,flags);return flags;}
void l2_mmio_exit(unsigned long flags){spin_unlock_irqrestore(&mmio_lock,flags);}
struct m3_mapping{void*cpu;u64 size,physical;bool framebuffer;};
static struct m3_mapping mappings[4096];
void l2_map(void*cpu,unsigned long long size,unsigned long long physical)
{
 unsigned long f;unsigned i;u64 meta[2]={size,physical};l2_record(6,(u64)cpu,meta,sizeof(meta));
 spin_lock_irqsave(&tape_lock,f);for(i=0;i<ARRAY_SIZE(mappings);i++)if(!mappings[i].cpu){mappings[i]=(struct m3_mapping){.cpu=cpu,.size=size,.physical=physical};break;}
 if(i==ARRAY_SIZE(mappings))dropped++;spin_unlock_irqrestore(&tape_lock,f);
}
void l2_unmap(void*cpu)
{
 unsigned long f;struct m3_mapping m={0};
 spin_lock_irqsave(&tape_lock,f);for(unsigned i=0;i<ARRAY_SIZE(mappings);i++)if(mappings[i].cpu==cpu){m=mappings[i];mappings[i].cpu=NULL;break;}spin_unlock_irqrestore(&tape_lock,f);
 for(u64 off=0;off<m.size;off+=65536)l2_record(7,(u64)cpu+off,(char*)cpu+off,min_t(u64,65536,m.size-off));
}
static u64 mapped_physical(void *p)
{
 return is_vmalloc_addr(p)?((u64)vmalloc_to_pfn(p)<<PAGE_SHIFT)|offset_in_page(p):virt_to_phys(p);
}
void modelos_m3_mmio_map(void *base)
{
 /* Caller already holds l2_mmio's lock. A base is logged once, not per
  * access. Record real physical BAR identity, not only relative offsets. */
 for(unsigned i=0;i<ARRAY_SIZE(mmio_bases);i++){
  if(mmio_bases[i]==base)return;
  if(!mmio_bases[i]){u64 pa=mapped_physical(base);mmio_bases[i]=base;if(!gpu_bar0)gpu_bar0=pa;l2_record(25,(u64)base,&pa,sizeof(pa));return;}
 }
}
static void pci_snapshot(void)
{
 struct pci_dev *gpu=pci_get_device(0x10de,0x2b85,NULL);u8 cfg[256];u16 ctl=0,command=0;u64 info[6];
 if(!gpu)return;
 for(unsigned i=0;i<sizeof(cfg);i++)pci_read_config_byte(gpu,i,cfg+i);
 pci_read_config_word(gpu,PCI_COMMAND,&command);
 if(gpu->msi_cap)pci_read_config_word(gpu,gpu->msi_cap+PCI_MSI_FLAGS,&ctl);
 info[0]=gpu->msi_enabled;info[1]=gpu->msix_enabled;info[2]=gpu->irq;
 info[3]=pci_resource_start(gpu,0);info[4]=pci_resource_start(gpu,1);info[5]=pci_resource_len(gpu,1);
 gpu_bar0=info[3];gpu_bar0_bytes=pci_resource_len(gpu,0);gpu_bar1=info[4];gpu_bar1_bytes=info[5];
 if(!audit_bar)audit_bar=ioremap(gpu_bar0,gpu_bar0_bytes);
 l2_record(23,gpu->devfn,cfg,sizeof(cfg));l2_record(24,gpu->devfn,info,sizeof(info));
 pr_info("M3_HW_INFO msi=%u msix=%u irq=%u msi_cap=%x msi_ctl=%x command=%x bar0=%llx\n",gpu->msi_enabled,gpu->msix_enabled,gpu->irq,gpu->msi_cap,ctl,command,info[3]);
 pci_dev_put(gpu);
}
static bool gpu_io(u64 pa,unsigned bytes)
{
 return(gpu_bar0&&pa>=gpu_bar0&&pa-gpu_bar0<gpu_bar0_bytes&&bytes<=gpu_bar0_bytes-(pa-gpu_bar0))||
       (pa>=gpu_bar1&&pa-gpu_bar1<gpu_bar1_bytes&&bytes<=gpu_bar1_bytes-(pa-gpu_bar1));
}
void modelos_m3_cpu_scalar(void *p,unsigned long long value,unsigned bytes)
{
 u64 pa;if(!ready||READ_ONCE(frozen))return;pa=mapped_physical(p);
 /* A volatile MMIO store can be W1C/write-only. Record its intended value,
  * never a read-back of the register after the write. */
 if(gpu_io(pa,bytes)){l2_record(26,pa,&value,bytes);return;}
 modelos_m3_cpu_write(p,bytes);
}
void modelos_m3_cpu_read_value(void *p,unsigned long long value,unsigned bytes)
{
 u64 pa;if(!ready||READ_ONCE(frozen))return;pa=mapped_physical(p);
 if(gpu_io(pa,bytes))l2_record(28,pa,&value,bytes);
}
void modelos_m3_fb_map(void *cpu,unsigned long long bytes)
{
 unsigned long f;unsigned i;u64 meta[2]={bytes,mapped_physical(cpu)};
 l2_record(19,(u64)cpu,meta,sizeof(meta));WRITE_ONCE(have_fb,true);
 spin_lock_irqsave(&tape_lock,f);
 for(i=0;i<ARRAY_SIZE(mappings);i++)if(!mappings[i].cpu){mappings[i]=(struct m3_mapping){.cpu=cpu,.size=bytes,.physical=meta[1],.framebuffer=true};break;}
 if(i==ARRAY_SIZE(mappings))dropped++;spin_unlock_irqrestore(&tape_lock,f);
}
void modelos_m3_fb_unmap(void *cpu)
{
 unsigned long f;spin_lock_irqsave(&tape_lock,f);
 for(unsigned i=0;i<ARRAY_SIZE(mappings);i++)if(mappings[i].cpu==cpu&&mappings[i].framebuffer){mappings[i].cpu=NULL;break;}
 spin_unlock_irqrestore(&tape_lock,f);
}
void modelos_m3_cpu_write(void *p,unsigned long long bytes)
{
 unsigned long f;u64 size=0;bool fb=false;unsigned long address=(unsigned long)p;
 if(!ready||READ_ONCE(frozen))return;
 /* Internal BAR2/PRAMIN transfers do not necessarily call memdescMapOld,
  * so they are absent from mappings[]. Observe the actual bulk CPU store
  * by physical PCI BAR identity, including transfers before FSP starts. */
 if(bytes&&gpu_io(mapped_physical(p),1)){
  for(u64 off=0;off<bytes;){
   u64 pa=mapped_physical((char*)p+off),n=min_t(u64,bytes-off,PAGE_SIZE-offset_in_page((char*)p+off));
   if(!gpu_io(pa,n)){pr_err("M3_GPU_BULK_RANGE physical=%llx bytes=%llu\n",pa,n);return;}
   l2_record(32,pa,(char*)p+off,n);off+=n;
  }
  return;
 }
 if(!READ_ONCE(after_boot_snapshot)&&!READ_ONCE(have_fb))return;
 spin_lock_irqsave(&tape_lock,f);
 for(unsigned i=0;i<ARRAY_SIZE(mappings);i++){
  unsigned long base=(unsigned long)mappings[i].cpu;
  if(base&&address>=base&&address-base<mappings[i].size){size=min_t(u64,bytes,mappings[i].size-(address-base));fb=mappings[i].framebuffer;break;}
 }
 spin_unlock_irqrestore(&tape_lock,f);
 if(!size&&READ_ONCE(after_boot_snapshot)){
  /* Some kernel DMA users take the fixed page allocation directly, without
   * a memdescMap registration. Capture their CPU stores too. */
  u64 pa=mapped_physical(p);
  if(pa>=M3_HIGH_BASE&&pa-M3_HIGH_BASE<M3_HIGH_SIZE)size=min_t(u64,bytes,M3_HIGH_SIZE-(pa-M3_HIGH_BASE));
 }
 if(!size||(!fb&&!READ_ONCE(after_boot_snapshot)))return;
 for(u64 off=0;off<size;){
  u64 pa=mapped_physical((char*)p+off),n=min_t(u64,size-off,PAGE_SIZE-offset_in_page((char*)p+off));
  /* Queue framing and receive acknowledgements are handled by live cursors.
   * Never put recorded reply words or SEEN markers into a fresh RX ring. */
  if(!queue_physical||pa<queue_physical||pa-queue_physical>=528384)l2_record(20,pa,(char*)p+off,n);
  off+=n;
 }
}
void l2_counts_dump(void)
{
 unsigned long flags;if(READ_ONCE(frozen))return;spin_lock_irqsave(&tape_lock,flags);frozen=true;spin_unlock_irqrestore(&tape_lock,flags);
 pr_info("M3_CAPTURE_READY tape=%llx bytes=%llu records=%llu dropped=%llu low_used=%llu high_used=%llu allocations=%llu frees=%llu exhaustions=%llu rm_linked=1\n",M3_TAPE_BASE,used,serial,dropped,low_used,high_used,allocations,frees,exhaustions);
}

/* Ordered host RAM image immediately before FSP consumes the firmware/boot
 * structures. This complements, rather than replaces, queue/RPC recordings. */
void modelos_m3_snapshot(const char*label)
{
 pci_snapshot();
 l2_record(11,0,label,strlen(label));
 const u64 ranges[][2]={{M3_LOW_BASE,READ_ONCE(low_used)},{M3_HIGH_BASE,READ_ONCE(high_used)}};
 for(unsigned i=0;i<ARRAY_SIZE(ranges);i++)for(u64 off=0;off<ranges[i][1];off+=65536)
  l2_record(15,ranges[i][0]+off,__va(ranges[i][0]+off),min_t(u64,65536,ranges[i][1]-off));
 WRITE_ONCE(after_boot_snapshot,true);
 WRITE_ONCE(post_boot_high,M3_HIGH_BASE+READ_ONCE(high_used));
 WRITE_ONCE(next_input_high,READ_ONCE(post_boot_high));
}
void modelos_m3_snapshot_new_inputs(void)
{
 u64 start=READ_ONCE(next_input_high),end=M3_HIGH_BASE+READ_ONCE(high_used);
 void __iomem *bar;u32 state[12];
 if(!start||start>end)return;
 /* Snapshot before the FIRST consumer, not only when GR allocates a
  * channel. Direct structure assignments into newly allocated DMA memory
  * do not necessarily pass through the mapped-write macros. */
 for(u64 pa=start;pa<end;pa+=65536)l2_record(22,pa,__va(pa),min_t(u64,65536,end-pa));
 WRITE_ONCE(next_input_high,end);
 bar=READ_ONCE(audit_bar);
 if(!bar)return;
 state[0]=readl(bar+0xb81600);
 state[1]=readl(bar+0xb81010);
 for(unsigned i=0;i<8;i++)state[2+i]=readl(bar+0xb83000+4*i);
 state[10]=readl(bar+0xb83028);state[11]=readl(bar+0xb8302c);
 l2_record(29,++input_boundary,state,sizeof(state));
 {
  u32 saved=readl(bar+0x10fd40);u64 roots[2];
  writel(0x7e6aa,bar+0x10fd40);readl(bar+0x10fd40);
  roots[0]=readl(bar+0x701000)|((u64)readl(bar+0x701004)<<32);
  roots[1]=readl(bar+0x700000)|((u64)readl(bar+0x700004)<<32);
  writel(saved,bar+0x10fd40);readl(bar+0x10fd40);
  l2_record(33,input_boundary,roots,sizeof(roots));
 }
 if(input_boundary>=129&&input_boundary<=131){
  /* Observation only: compare the failing instance/root with native RM.
   * These VRAM bytes must NEVER become replay stores. Save/restore XAL's
   * PRAMIN selector; the serialized boot producer has no serving traffic. */
  u32 saved=readl(bar+0x10fd40);
  l2_record(31,input_boundary,NULL,0);
  for(u64 pa=0x7e6a99000ULL;pa<0x7e6aa3000ULL;pa+=4096){
   writel(pa>>16,bar+0x10fd40);readl(bar+0x10fd40);
   for(unsigned off=0;off<4096;off+=4)*(u32 *)(vram_observation+off)=readl(bar+0x700000+(pa&65535)+off);
   l2_record(30,pa,vram_observation,sizeof(vram_observation));
  }
  writel(0x7e670,bar+0x10fd40);readl(bar+0x10fd40);
  for(unsigned off=0;off<4096;off+=4)*(u32 *)(vram_observation+off)=readl(bar+0x705000+off);
  l2_record(30,0x7e6705000ULL,vram_observation,sizeof(vram_observation));
  writel(saved,bar+0x10fd40);readl(bar+0x10fd40);
 }
}
void modelos_m3_snapshot_inputs(void)
{
 u64 start=READ_ONCE(post_boot_high),end=M3_HIGH_BASE+READ_ONCE(high_used);
 if(!start||start>end)return;
 /* Earlier firmware and GSP log/ring buffers precede this boundary. These
  * new DMA buffers contain GR inputs and CPU-built tables, including direct
  * structure assignments that do not use MEM_WR/portMemCopy. At replay only
  * bytes identical in both producer boots are admitted. No cubin has run. */
 for(u64 pa=start;pa<end;pa+=65536)l2_record(22,pa,__va(pa),min_t(u64,65536,end-pa));
}
