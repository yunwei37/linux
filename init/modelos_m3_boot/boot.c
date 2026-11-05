// SPDX-License-Identifier: GPL-2.0-only
/* RM-free diagnostic prefix. No NVIDIA headers, objects or RM symbols.
 * Register meanings/framing follow NVIDIA MIT sources at the producer pin:
 * timer_gh100.c, message_queue_cpu.c, msgq.c and mctp_format.h.
 * The emitted first event must be read from fresh DMA memory, never the tape.
 */
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/modelos_m3.h>
#include <linux/unaligned.h>
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <linux/interrupt.h>
#include "runtime.h"
extern const u8 m3_boot_begin[],m3_boot_end[];
extern int m3_execute_cubin(struct pci_dev *,const u64 *);
struct operation {u32 kind,bytes;u64 address,value;} __packed;
static void __iomem *bar;
static u64 queue_pa;
static u64 timer_offset;
static u32 tx_cursor,rx_cursor,sends,replies;
static bool gpu_ready;
static u64 model_root;
static u32 model_root_tier;
static bool have_model_root;
static struct pci_dev *runtime_gpu;
static u32 next_rpc_sequence;
static u64 next_mmu_fence,completed_mmu_fence;
static u8 frame[65536];
static u64 resources[24];
static bool have_resources;
static bool cubin_checked;
static bool fault_capture;
static bool root_diverged;
#define M3_FAULT_PHYSICAL 0x47f000000ULL
static atomic_t irq_events=ATOMIC_INIT(0);
static irqreturn_t m3_irq(int irq,void *cookie)
{
 int count=atomic_inc_return(&irq_events);
 if(count<=4)pr_info("M3_BOOT_IRQ irq=%d sends=%u replies=%u top=%x\n",irq,READ_ONCE(sends),READ_ONCE(replies),bar?readl(bar+0xb81600):0);
 return IRQ_HANDLED;
}
static int m3_msix(struct pci_dev *gpu)
{
 int available=pci_msix_vec_count(gpu),count,rc;
 if(available<1||available>32)return -EINVAL;
 count=pci_alloc_irq_vectors(gpu,available,available,PCI_IRQ_MSIX);
 if(count<0)return count;
 for(int i=0;i<count;i++){
  rc=request_irq(pci_irq_vector(gpu,i),m3_irq,0,"modelos-m3",gpu);
  if(rc){for(int j=0;j<i;j++)free_irq(pci_irq_vector(gpu,j),gpu);pci_free_irq_vectors(gpu);return rc;}
 }
 pr_info("M3_BOOT_MSIX vectors=%d no_rm_handler=1\n",count);return 0;
}
static u64 clock_store_pair=~0ULL,clock_store_value;
static bool arena(u64 a,u64 n);
static int table_read(u64 physical,u32 aperture,u64 *value)
{
 /* Aperture values are VER3 PDE encodings: VID1,SYS_COH2,SYS_NCOH3.
  * PRAMIN is physical VRAM, unlike the model's virtual BAR1 aperture. */
 if(aperture==2||aperture==3){
  if(!arena(physical,8))return -EINVAL;
  *value=READ_ONCE(*(u64 *)__va(physical));return 0;
 }
 if(aperture==1&&physical<(32ULL<<30)&&!(physical&7)){
  u32 saved=readl(bar+0x10fd40),lo,hi;
  writel(physical>>16,bar+0x10fd40);readl(bar+0x10fd40);
  lo=readl(bar+0x700000+(physical&0xffff));hi=readl(bar+0x700004+(physical&0xffff));
  writel(saved,bar+0x10fd40);readl(bar+0x10fd40);
  *value=((u64)hi<<32)|lo;return 0;
 }
 return -EINVAL;
}
static int vram_write(u64 physical,const u8 *data,u32 bytes)
{
 u32 saved;
 if(physical>=(32ULL<<30)||bytes>(32ULL<<30)-physical)return -EINVAL;
 saved=readl(bar+0x10fd40);
 for(u32 off=0;off<bytes;){
  u64 pa=physical+off;u32 n=min_t(u32,bytes-off,65536-(pa&65535));
  writel(pa>>16,bar+0x10fd40);readl(bar+0x10fd40);
  memcpy_toio(bar+0x700000+(pa&65535),data+off,n);wmb();
  /* kbusFlushSingle_GV100: PRAMIN writes redirect to the FB path. A
   * selector-register read does not flush that path; read PRAMIN itself. */
  readl(bar+0x700000);off+=n;
 }
 writel(saved,bar+0x10fd40);readl(bar+0x10fd40);return 0;
}
static int physical_copy(u64 destination,const u8 *meta,u32 bytes)
{
 static u8 buffer[4096];u64 source,size,src_vram,dst_vram;bool backwards;
 if(bytes!=32)return -EINVAL;
 source=get_unaligned_le64(meta);size=get_unaligned_le64(meta+8);
 src_vram=get_unaligned_le64(meta+16);dst_vram=get_unaligned_le64(meta+24);
 if(src_vram>1||dst_vram>1)return -EINVAL;
 if(src_vram?(source>=(32ULL<<30)||size>(32ULL<<30)-source):!arena(source,size))return -EINVAL;
 if(dst_vram?(destination>=(32ULL<<30)||size>(32ULL<<30)-destination):!arena(destination,size))return -EINVAL;
 backwards=src_vram==dst_vram&&destination>source&&destination-source<size;
 for(u64 done=0;done<size;){
  u32 n=min_t(u64,sizeof(buffer),size-done);u64 off=backwards?size-done-n:done;
  if(src_vram){
   u32 saved=readl(bar+0x10fd40);
   for(u32 i=0;i<n;){
    u64 pa=source+off+i;u32 count=min_t(u32,n-i,65536-(pa&65535));
    writel(pa>>16,bar+0x10fd40);readl(bar+0x10fd40);
    memcpy_fromio(buffer+i,bar+0x700000+(pa&65535),count);i+=count;
   }
   writel(saved,bar+0x10fd40);readl(bar+0x10fd40);
  }else memcpy(buffer,__va(source+off),n);
  if(dst_vram){int rc=vram_write(destination+off,buffer,n);if(rc)return rc;}
  else{memcpy(__va(destination+off),buffer,n);clflush_cache_range(__va(destination+off),n);wmb();}
  done+=n;
 }
 return 0;
}
static void fault_walk(void)
{
 static const u8 shift[]={56,47,38,29,21};
 u32 info=readl(bar+0xb83090),ilo=readl(bar+0xb83088);
 u64 instance=((u64)readl(bar+0xb8308c)<<32)|(ilo&~4095U);
 u64 va=((u64)readl(bar+0xb83084)<<32)|(readl(bar+0xb83080)&~4095U);
 u32 ia=(ilo>>10)&3;ia=ia?ia:1;
 if(!(info&BIT(31))){
  if(!have_resources)return;
  pr_err("M3_BOOT_BAR_WALK latched_valid=0 diagnostic_only=1\n");
 }
 for(unsigned root=0;root<2;root++){
  u64 raw,pa;u32 aperture;
  if(table_read(instance+(root?0x2a0:0x200),ia,&raw))continue;
  pr_err("M3_BOOT_RAMIN instance=%llx root=%u raw=%016llx va=%llx\n",instance,root,raw,va);
  pa=raw&0x000ffffffffff000ULL;aperture=raw&3;aperture=aperture?aperture:1;
  for(unsigned level=0;level<5;level++){
   u64 entry,slot=pa+((va>>shift[level])&(level==0?1:level==4?255:511))*(level==4?16:8);
   if(table_read(slot,aperture,&entry))break;
   pr_err("M3_BOOT_TABLE root=%u level=%u slot=%llx aperture=%u entry=%016llx\n",root,level,slot,aperture,entry);
   if(entry&1)break;
   if(level==4){
    u64 small;
    for(unsigned which=0;which<2;which++){
     u64 directory=entry,leaf,leaf_slot;u32 ap;
     if(which){if(table_read(slot+8,aperture,&small))break;directory=small;}
     ap=(directory>>1)&3;if(!ap)continue;
     leaf_slot=(directory&(which?0x000ffffffffff000ULL:0x000fffffffffff00ULL))+((va>>(which?12:16))&(which?511:31))*8;
     if(table_read(leaf_slot,ap,&leaf))continue;
     pr_err("M3_BOOT_LEAF root=%u page_shift=%u directory=%016llx slot=%llx aperture=%u entry=%016llx\n",root,which?12:16,directory,leaf_slot,ap,leaf);
    }
    break;
   }
   aperture=(entry>>1)&3;if(!aperture)break;
   pa=entry&(level==4?0x000fffffffffff00ULL:0x000ffffffffff000ULL);
  }
 }
}
static int direct_io(struct pci_dev *gpu,u64 pa,const u8 *data,u32 bytes,bool write)
{
 void __iomem *p=NULL;bool temporary=false;u64 first=pci_resource_start(gpu,0);
 if(bytes!=1&&bytes!=2&&bytes!=4&&bytes!=8)return -EINVAL;
 for(unsigned i=0;i<PCI_STD_NUM_BARS;i++){
  u64 base=pci_resource_start(gpu,i),size=pci_resource_len(gpu,i);
  if((pci_resource_flags(gpu,i)&IORESOURCE_MEM)&&pa>=base&&pa-base<=size&&bytes<=size-(pa-base)){
   if(i==0)p=bar+(pa-first);else{p=ioremap(pa,bytes);temporary=true;}
   break;
  }
 }
 if(!p)return -EINVAL;
 if(write){
  if(bytes==1)writeb(data[0],p);else if(bytes==2)writew(get_unaligned_le16(data),p);
  else if(bytes==4)writel(get_unaligned_le32(data),p);else writeq(get_unaligned_le64(data),p);
  wmb();
 }else{
  if(bytes==1)readb(p);else if(bytes==2)readw(p);else if(bytes==4)readl(p);else readq(p);
 }
 if(temporary)iounmap(p);return 0;
}
static int restore_store(struct pci_dev *gpu,u64 physical,const u8 *data,u32 bytes)
{
 if(arena(physical,bytes)){memcpy(__va(physical),data,bytes);clflush_cache_range(__va(physical),bytes);wmb();return 0;}
 for(unsigned i=0;i<PCI_STD_NUM_BARS;i++){
  u64 base=pci_resource_start(gpu,i),size=pci_resource_len(gpu,i);
  if((pci_resource_flags(gpu,i)&IORESOURCE_MEM)&&physical>=base&&physical-base<=size&&bytes<=size-(physical-base)){
   void __iomem *p=ioremap(physical,bytes);if(!p)return -ENOMEM;
   memcpy_toio(p,data,bytes);wmb();iounmap(p);return 0;
  }
 }
 return -EINVAL;
}
static int submit_frame(const u8 *data,u32 bytes)
{
 u32 slots=bytes/4096,tail,i,n,type;u64 deadline=ktime_get_ns()+3ULL*NSEC_PER_SEC;
 if(!queue_pa||!bytes||bytes%4096||bytes>sizeof(frame))return -EINVAL;
 memcpy(frame,data,bytes);n=get_unaligned_le32(frame+16);type=get_unaligned_le32(frame+12)>>24;
 if(get_unaligned_le32(frame)!=0x4d435450||n+24>bytes)return -EINVAL;
 if(type==0x26&&n==2296)put_unaligned_le64(timer_offset,frame+24+520);
 if(type==0x25&&n>=32)next_rpc_sequence=max(next_rpc_sequence,get_unaligned_le32(frame+24+24)+1);
 if(type==0x25&&n==272&&get_unaligned_le32(frame+36)==0x4c&&get_unaligned_le32(frame+64)==0x20801803)memset(frame+24+72,0,n-72);
 do {tail=readl(bar+0x110c04);if(tx_cursor-tail+slots<=63)break;usleep_range(20,40);}while(ktime_get_ns()<deadline);
 if(tx_cursor-tail+slots>63)return -ETIMEDOUT;
 for(i=0;i<slots;i++)memcpy(__va(queue_pa+0x2000+((tx_cursor+i)%63)*4096),frame+i*4096,4096);
 wmb();tx_cursor+=slots;writel(tx_cursor,bar+0x110c00);sends++;return 0;
}
static bool arena(u64 a,u64 n)
{
 return (a>=M3_LOW_BASE&&a-M3_LOW_BASE<=M3_LOW_SIZE&&n<=M3_LOW_SIZE-(a-M3_LOW_BASE))||
        (a>=M3_HIGH_BASE&&a-M3_HIGH_BASE<=M3_HIGH_SIZE&&n<=M3_HIGH_SIZE-(a-M3_HIGH_BASE));
}
int m3_gpu_read_u64(u64 address,u32 tier,u64 *value)
{
 if(!gpu_ready||!value||address&7||tier>1)return -EINVAL;
 return table_read(address,tier?1:2,value);
}
int m3_gpu_write_u64(u64 address,u32 tier,u64 value)
{
 if(!gpu_ready||address&7||tier>1)return -EINVAL;
 if(tier)return vram_write(address,(u8 *)&value,sizeof(value));
 if(!arena(address,8))return -EINVAL;
 WRITE_ONCE(*(u64 *)__va(address),value);clflush_cache_range(__va(address),8);wmb();return 0;
}
int m3_gpu_flush(void)
{
 if(!gpu_ready)return -ENODEV;
 wmb();readl(bar+0x700000);return 0;
}
int m3_gpu_invalidate_post(u64 root,u32 tier,u64 *fence)
{
 if(!gpu_ready||!fence||root&4095||root>>52||tier>1)return -EINVAL;
 if(readl(bar+0xb830b0)&BIT(31))return -EAGAIN;
 /* The preceding caller's fence is now complete before another is posted. */
 completed_mmu_fence=next_mmu_fence;
 m3_gpu_flush();writel((u32)(root>>12)<<4|(tier?0:2),bar+0xb830a0);
 writel(root>>40,bar+0xb830a4);
 /* ALL_VA,SYS_MEMBAR,global ACK,all caches/all TLBs,TRIGGER. */
 writel(0x800000c1,bar+0xb830b0);*fence=++next_mmu_fence;return 0;
}
int m3_gpu_invalidate_poll(u64 fence)
{
 if(!gpu_ready||!fence||fence>next_mmu_fence)return -EINVAL;
 if(fence<=completed_mmu_fence)return 1;
 if(readl(bar+0xb830b0)&BIT(31))return 0;
 completed_mmu_fence=next_mmu_fence;return 1;
}
int m3_gpu_translate(u64 root,u32 tier,u64 va,u64 *physical,u32 *target_tier)
{
 static const u8 shifts[]={56,47,38,29,21};u64 table=root,entry;u32 aperture=tier?1:2;
 if(!gpu_ready||!physical||!target_tier||tier>1||root&4095||va>>57)return -EINVAL;
 for(unsigned depth=0;depth<5;depth++){
  u64 slot=table+((va>>shifts[depth])&(depth==0?1:depth==4?255:511))*(depth==4?16:8);int rc;
  rc=table_read(slot,aperture,&entry);if(rc)return rc;
  if(entry&1){
   if(depth<2)return -EINVAL;
   *physical=(entry&0x000ffffffffff000ULL)+(va&((1ULL<<shifts[depth])-1));
   if(((entry>>1)&3)==1)return -EINVAL;
   *target_tier=((entry>>1)&3)==0?1:0;return 0;
  }
  if(depth==4){
   if((entry>>1)&3){
    u64 leaf;rc=table_read((entry&0x000fffffffffff00ULL)+((va>>16)&31)*8,(entry>>1)&3,&leaf);if(rc)return rc;
    if(leaf&1){if(((leaf>>1)&3)==1)return -EINVAL;*physical=(leaf&0x000ffffffffff000ULL)+(va&65535);*target_tier=((leaf>>1)&3)==0?1:0;return 0;}
   }
   rc=table_read(slot+8,aperture,&entry);if(rc)return rc;
   if(!((entry>>1)&3))return -ENOENT;
   rc=table_read((entry&0x000ffffffffff000ULL)+((va>>12)&511)*8,(entry>>1)&3,&entry);if(rc)return rc;
   if(!(entry&1))return -ENOENT;
   if(((entry>>1)&3)==1)return -EINVAL;
   *physical=(entry&0x000ffffffffff000ULL)+(va&4095);*target_tier=((entry>>1)&3)==0?1:0;return 0;
  }
  aperture=(entry>>1)&3;if(!aperture)return -ENOENT;table=entry&0x000ffffffffff000ULL;
 }
 return -EINVAL;
}
#ifdef CONFIG_MODELOS_M3_RUNTIME_OBJECTS
extern int m3_runtime_objects(u64,u64);
extern int m3_run_relocated_cubin(struct pci_dev *,const u64 *,u64,u64,u64);
int m3_gpu_cubin_at(u64 va,u64 physical,u64 bytes)
{
 if(!gpu_ready||!runtime_gpu||!have_resources)return -ENODEV;
 return m3_run_relocated_cubin(runtime_gpu,resources,va,physical,bytes);
}
static int runtime_objects_probe(void)
{
 u64 root=model_root,first,last;
 u32 tier=model_root_tier,ap1,ap2;int rc;
 if(!have_resources||!have_model_root||resources[1]!=0x200000)return -EINVAL;
 rc=m3_gpu_translate(root,tier,resources[0],&first,&ap1);if(rc)return rc;
 rc=m3_gpu_translate(root,tier,resources[0]+resources[1]-1,&last,&ap2);if(rc)return rc;
 if(tier!=1||ap1!=1||ap2!=1||last-first!=resources[1]-1)return -EINVAL;
 pr_info("M3_RUNTIME_OBJECTS_BEGIN root=%llx pool=%llx bytes=1048576\n",root,first+0x100000);
 rc=m3_runtime_objects(root,first+0x100000);
 pr_info("M3_RUNTIME_OBJECTS_END status=%d mappings=3 page_size=4096 rm_linked=0\n",rc);return rc;
}
#endif
int m3_gsp_post(u32 type,const u8 *payload,u32 bytes,u32 *sequence)
{
 u32 slots,tail;
 if(!gpu_ready||!payload||!sequence||(type!=0x25&&type!=0x26)||bytes<32||bytes>sizeof(frame)-24)return -EINVAL;
 slots=DIV_ROUND_UP(bytes+24,4096);tail=readl(bar+0x110c04);
 if(tx_cursor-tail+slots>63)return -EAGAIN;
 memset(frame,0,slots*4096);put_unaligned_le32(0x4d435450,frame);
 put_unaligned_le32(bytes+24,frame+4);put_unaligned_le32(0xc0000001,frame+8);
 put_unaligned_le32(type<<24|0x10de7e,frame+12);put_unaligned_le32(bytes,frame+16);
 memcpy(frame+24,payload,bytes);
 *sequence=next_rpc_sequence;
 if(type==0x25)put_unaligned_le32(next_rpc_sequence++,frame+24+24);
 for(u32 i=0;i<slots;i++)memcpy(__va(queue_pa+0x2000+((tx_cursor+i)%63)*4096),frame+i*4096,4096);
 dma_wmb();tx_cursor+=slots;writel(tx_cursor,bar+0x110c00);return 0;
}
int m3_gsp_poll(u32 *type,u8 *payload,u32 capacity,u32 *bytes)
{
 u32 head,n,slots;u8 *first;
 if(!gpu_ready||!type||!payload||!bytes)return -EINVAL;
 head=readl(bar+0x110c80);
 if(head==rx_cursor)return -EAGAIN;
 if(head-rx_cursor>63)return -EIO;
 dma_rmb();first=__va(queue_pa+0x42000+(rx_cursor%63)*4096);
 if(get_unaligned_le32(first)!=0x4d435450)return -EIO;
 n=get_unaligned_le32(first+16);slots=DIV_ROUND_UP(n+24,4096);
 if(!slots||n>sizeof(frame)-24||get_unaligned_le32(first+4)!=n+24)return -EIO;
 if(head-rx_cursor<slots)return -EAGAIN;
 *bytes=n;if(capacity<n)return -ENOSPC;
 for(u32 i=0;i<slots;i++)memcpy(frame+i*4096,__va(queue_pa+0x42000+((rx_cursor+i)%63)*4096),4096);
 *type=get_unaligned_le32(frame+12)>>24;memcpy(payload,frame+24,n);
 rx_cursor+=slots;dma_wmb();writel(rx_cursor,bar+0x110c84);return 0;
}
static int runtime_probe(void)
{
 u8 request[272]={0},response[4096];u32 sequence,type,bytes,events=0;u64 fence,deadline,root;int rc;
 root=((u64)readl(bar+0xb830a4)<<40)|((u64)(readl(bar+0xb830a0)>>4)<<12);
 rc=m3_gpu_invalidate_post(root,(readl(bar+0xb830a0)&2)?0:1,&fence);if(rc)return rc;
 deadline=ktime_get_ns()+NSEC_PER_SEC;
 do{rc=m3_gpu_invalidate_poll(fence);if(rc)break;cpu_relax();}while(ktime_get_ns()<deadline);
 if(rc!=1)return rc<0?rc:-ETIMEDOUT;
 /* Read-only GET_PCI_BAR_INFO on the already live internal client/subdevice. */
 put_unaligned_le32(0x03000000,request);put_unaligned_le32(0x43505256,request+4);
 put_unaligned_le32(sizeof(request),request+8);put_unaligned_le32(0x4c,request+12);
 put_unaligned_le32(~0U,request+16);put_unaligned_le32(~0U,request+20);
 put_unaligned_le32(0xc2000006,request+32);put_unaligned_le32(0xabcd2080,request+36);
 put_unaligned_le32(0x20801803,request+40);put_unaligned_le32(200,request+48);
 rc=m3_gsp_post(0x25,request,sizeof(request),&sequence);if(rc)return rc;
 deadline=ktime_get_ns()+NSEC_PER_SEC;
 do{
  rc=m3_gsp_poll(&type,response,sizeof(response),&bytes);
  if(rc==-EAGAIN){cpu_relax();continue;}
  if(rc)return rc;
  if(type==0x25&&bytes>=32&&get_unaligned_le32(response+24)==sequence){
   if(get_unaligned_le32(response+12)!=0x4c||get_unaligned_le32(response+16)!=0)return -EIO;
   pr_info("M3_RUNTIME_API_PASS mmu_fence=%llu gsp_sequence=%u events=%u rm_linked=0\n",fence,sequence,events);return 0;
  }
  events++;
 }while(ktime_get_ns()<deadline);
 return -ETIMEDOUT;
}
static int poll32(u32 off,u32 want,u64 index)
{
 u64 deadline=ktime_get_ns()+3ULL*NSEC_PER_SEC;u32 got;
 do {got=readl(bar+off);if(got==want)return 0;usleep_range(20,40);}while(ktime_get_ns()<deadline);
 pr_err("M3_BOOT_POLL_FAIL op=%llu offset=%x want=%x got=%x\n",index,off,want,got);return -ETIMEDOUT;
}
static int invalidate_fence(u64 index)
{
 u64 deadline=ktime_get_ns()+3ULL*NSEC_PER_SEC;u32 value;
 do {value=readl(bar+0xb830b0);if(!(value&0x80000000))return 0;usleep_range(20,40);}while(ktime_get_ns()<deadline);
 pr_err("M3_BOOT_INVALIDATE_TIMEOUT op=%llu value=%x\n",index,value);return -ETIMEDOUT;
}
static void timer(void)
{
 u32 hi,lo,hi2;u64 offset;
 do {hi=readl(bar+0x118f58);lo=readl(bar+0x118f54);hi2=readl(bar+0x118f58);}while(hi!=hi2);
 offset=ktime_get_real_ns()-(((u64)hi<<32)|lo);
 timer_offset=offset;
 writel(offset>>32,bar+0x118df8);writel((u32)offset|1,bar+0x118df4);
}
static int send_gmc(u32 type,const u8 *d,u32 n)
{
 u32 slots=DIV_ROUND_UP(n+24,4096),i,tail;
 u32 words[6]={0x4d435450,n+24,0xc0000001,(type<<24)|0x10de7e,n,0};u64 deadline=ktime_get_ns()+3ULL*NSEC_PER_SEC;
 if(!queue_pa||n+24>sizeof(frame))return -EINVAL;
 memset(frame,0,slots*4096);memcpy(frame,words,sizeof(words));memcpy(frame+24,d,n);
 /* GMC boot args contain the same system-minus-secure-time offset that was
  * written to SCI. The second recording identified exactly this word. */
 if(type==0x26&&n==2296)put_unaligned_le64(timer_offset,frame+24+520);
 /* This control has output-only parameters. The producer copied unused
  * stack contents into its request. Zero the whole output, not just two
  * differing words; real BAR values come from the live GSP reply. */
 if(type==0x25&&n==272&&get_unaligned_le32(d+12)==0x4c&&get_unaligned_le32(d+40)==0x20801803)memset(frame+24+72,0,n-72);
 do {tail=readl(bar+0x110c04);if(tx_cursor-tail+slots<=63)break;usleep_range(20,40);}while(ktime_get_ns()<deadline);
 if(tx_cursor-tail+slots>63)return -ETIMEDOUT;
 for(i=0;i<slots;i++)memcpy(__va(queue_pa+0x2000+((tx_cursor+i)%63)*4096),frame+i*4096,4096);
 wmb();tx_cursor+=slots;writel(tx_cursor,bar+0x110c00);sends++;
 return 0;
}
static int receive(const u8 *expected,u32 n)
{
 u32 magic,total,wire_n,head,slots=DIV_ROUND_UP(n+24,4096),i;u64 deadline=ktime_get_ns()+3ULL*NSEC_PER_SEC;
 if(n+24>sizeof(frame)||!queue_pa)return -EINVAL;
 do {head=readl(bar+0x110c80);if(head-rx_cursor>=slots&&head-rx_cursor<=63)break;usleep_range(20,40);}while(ktime_get_ns()<deadline);
 if(head-rx_cursor<slots||head-rx_cursor>63){pr_err("M3_BOOT_RX_TIMEOUT replies=%u head=%u cursor=%u expected_function=%x\n",replies,head,rx_cursor,get_unaligned_le32(expected+12));return -ETIMEDOUT;}
 dma_rmb();for(i=0;i<slots;i++)memcpy(frame+i*4096,__va(queue_pa+0x42000+((rx_cursor+i)%63)*4096),4096);
 magic=get_unaligned_le32(frame);total=get_unaligned_le32(frame+4);wire_n=get_unaligned_le32(frame+16);
 if(magic!=0x4d435450||wire_n!=n||total!=n+24||memcmp(frame+24,expected,n)){
  pr_err("M3_BOOT_RX_DIFF reply=%u magic=%x total=%u bytes=%u function=%x expected=%x expected_bytes=%u\n",replies,magic,total,wire_n,get_unaligned_le32(frame+36),get_unaligned_le32(expected+12),n);
  for(i=0;i<min(n,wire_n);i++)if(frame[24+i]!=expected[i]){pr_err("M3_BOOT_RX_BYTE offset=%u got=%x expected=%x\n",i,frame[24+i],expected[i]);break;}
  return -EIO;
 }
 rx_cursor+=slots;wmb();writel(rx_cursor,bar+0x110c84);replies++;
 if(replies==1)pr_info("M3_BOOT_FIRST_EVENT_PASS bytes=%u rm_linked=0 cubin_checked=0\n",n);
 return 0;
}
static int __init boot(void)
{
 struct pci_dev *gpu=NULL;const u8 *p=m3_boot_begin;u64 index=0;int rc=-ENODEV;
 for_each_pci_dev(gpu)if(gpu->vendor==0x10de&&gpu->device==0x2b85)break;
 if(!gpu)goto out;
 runtime_gpu=gpu;
 rc=pci_enable_device(gpu);if(rc)goto out;
 pci_set_master(gpu);rc=dma_set_mask_and_coherent(&gpu->dev,DMA_BIT_MASK(64));if(rc)goto out;
 rc=m3_msix(gpu);if(rc)goto out;
 {u16 ctl=0,command=0;pci_read_config_word(gpu,PCI_COMMAND,&command);if(gpu->msi_cap)pci_read_config_word(gpu,gpu->msi_cap+PCI_MSI_FLAGS,&ctl);pr_info("M3_HW_INFO msi=%u msix=%u irq=%u msi_cap=%x msi_ctl=%x command=%x\n",gpu->msi_enabled,gpu->msix_enabled,gpu->irq,gpu->msi_cap,ctl,command);}
 bar=pci_iomap(gpu,0,0);if(!bar){rc=-ENOMEM;goto out;}
 pr_info("M3_BOOT_BEGIN pci=%s bar0=%llx bytes=%llu rm_linked=0\n",pci_name(gpu),(u64)pci_resource_start(gpu,0),(u64)(m3_boot_end-m3_boot_begin));
 while(p<m3_boot_end){
  struct operation op;
  if(m3_boot_end-p<sizeof(op)){rc=-EINVAL;break;}
  memcpy(&op,p,sizeof(op));p+=sizeof(op);index++;
  if(op.bytes>m3_boot_end-p){rc=-EINVAL;break;}
  if(!op.kind){rc=0;break;}
  if((op.kind==1||op.kind==2||op.kind==17)&&(op.address&3||op.address+4>pci_resource_len(gpu,0))){rc=-EINVAL;break;}
  if(op.kind==1){wmb();writel(op.value,bar+op.address);if(op.address==0xb830b0&&(op.value&0x80000000)){rc=invalidate_fence(index);if(rc)break;}}
  else if(op.kind==2){rc=poll32(op.address,op.value,index);if(rc)break;}
  else if(op.kind==3){
   if(!arena(op.address,op.bytes)){rc=-EINVAL;break;}
   memcpy(__va(op.address),p,op.bytes);clflush_cache_range(__va(op.address),op.bytes);wmb();
   /* The shared ring is the only 528384-byte initial RAM write. */
   if(op.bytes==528384)queue_pa=op.address;
  }
  else if(op.kind==4){rc=send_gmc(op.address,p,op.bytes);if(rc)break;}
  else if(op.kind==5){rc=receive(p,op.bytes);if(rc)break;}
  else if(op.kind==6)timer();
  else if(op.kind==7){u32 got=readl(bar+op.address);if(got!=op.value){pr_err("M3_BOOT_PORT_FAIL op=%llu offset=%llx want=%llx got=%x\n",index,op.address,op.value,got);rc=-EIO;break;}}
  else if(op.kind==8){pr_info("M3_BOOT_PROTOCOL_PASS sends=%u replies=%u rm_linked=0 cubin_checked=0\n",sends,replies);gpu_ready=true;if(have_resources){rc=m3_execute_cubin(gpu,resources);if(rc)break;cubin_checked=true;}}
  else if(op.kind==9){
   /* Frozen QEMU topology exposes this GPU as conventional PCI: walking
    * its guest capability list finds no PCIe cap. RM uses the published
    * NV_EP_PCFG_GPU_DEVICE_CONTROL_STATUS offset 0x68 directly instead.
    */
   u16 ctl;u32 actual=readl(bar+0x92068);
   pci_read_config_word(gpu,0x68,&ctl);
   pci_write_config_word(gpu,0x68,(u16)actual&~PCI_EXP_DEVCTL_NOSNOOP_EN);
   pr_info("M3_BOOT_PCICFG config=%x alias_before=%x alias_after=%x\n",ctl,actual,readl(bar+0x92068));
  }
  else if(op.kind==10){rc=submit_frame(p,op.bytes);if(rc)break;}
  else if(op.kind==11){
   u64 bdf,value;if(op.bytes!=16||op.address+op.value>4096){rc=-EINVAL;break;}
   bdf=get_unaligned_le64(p);value=get_unaligned_le64(p+8);
   if(bdf!=((u64)pci_domain_nr(gpu->bus)<<32|((u64)gpu->bus->number<<8)|gpu->devfn)){rc=-ENODEV;break;}
   if(op.value==1)rc=pci_write_config_byte(gpu,op.address,value);
   else if(op.value==2)rc=pci_write_config_word(gpu,op.address,value);
   else if(op.value==4)rc=pci_write_config_dword(gpu,op.address,value);
   else rc=-EINVAL;
   if(rc)break;
  }
  else if(op.kind==12){if(op.bytes!=sizeof(resources)){rc=-EINVAL;break;}memcpy(resources,p,sizeof(resources));have_resources=true;}
  else if(op.kind==13){rc=restore_store(gpu,op.address,p,op.bytes);if(rc)break;}
  else if(op.kind==14){
   u32 lo,hi,hi2,word;
   if((op.address&~7ULL)!=clock_store_pair){
    do {hi=readl(bar+0xbb0084);lo=readl(bar+0xbb0080);hi2=readl(bar+0xbb0084);}while(hi!=hi2);
    clock_store_pair=op.address&~7ULL;clock_store_value=((u64)hi<<32)|lo;
   }
   if(op.value!=0xbb0080&&op.value!=0xbb0084){rc=-EINVAL;break;}
   word=op.value==0xbb0080?(u32)clock_store_value:clock_store_value>>32;
   rc=restore_store(gpu,op.address,(u8 *)&word,sizeof(word));if(rc)break;
  }
  else if(op.kind==16){
   if(!arena(op.address,op.value)||op.value>UINT_MAX){rc=-EINVAL;break;}
   clflush_cache_range(__va(op.address),op.value);mb();readl(bar);
   pr_info("M3_BOOT_DMA_VISIBLE physical=%llx bytes=%llu\n",op.address,op.value);
  }
  else if(op.kind==17)readl(bar+op.address);
  else if(op.kind==18){rc=direct_io(gpu,op.address,p,op.bytes,true);if(rc)break;}
  else if(op.kind==19){rc=direct_io(gpu,op.address,NULL,op.value,false);if(rc)break;}
  else if(op.kind==20){
   u32 expected,actual;
   if(op.bytes!=48){rc=-EINVAL;break;}
   expected=get_unaligned_le32(p+5*4);actual=readl(bar+0xb8300c);
   if(actual!=expected){
    pr_err("M3_BOOT_FIRST_FAULT boundary=%llu sends=%u replies=%u expected_put=%x actual_put=%x get=%x\n",op.address,sends,replies,expected,actual,readl(bar+0xb83008));
    rc=-EIO;break;
   }
  }
  else if(op.kind==21){
   /* Diagnostic only. GB100 dev_vm.h publishes physical-address mode and
    * SYS_COH aperture. Keep firmware's ring size; redirect its hardware
    * output before the failing class, never replay a recorded fault. */
   u32 size=readl(bar+0xb83010);
   if(op.value==1){
    writel(size&~BIT(31),bar+0xb83010);readl(bar+0xb83010);
    pr_info("M3_BOOT_FAULT_UNBUFFERED before=%x after=%x\n",size,readl(bar+0xb83010));
    fault_capture=true;p+=op.bytes;continue;
   }
   memset(__va(M3_FAULT_PHYSICAL),0,0x40000);
   clflush_cache_range(__va(M3_FAULT_PHYSICAL),0x40000);mb();
   writel(size&~BIT(31),bar+0xb83010);readl(bar+0xb83010);
   writel(M3_FAULT_PHYSICAL>>32,bar+0xb83004);
   writel((u32)M3_FAULT_PHYSICAL|5,bar+0xb83000);
   writel(0,bar+0xb83008);writel(size,bar+0xb83010);
   pr_info("M3_BOOT_FAULT_CAPTURE physical=%llx size=%x lo=%x hi=%x get=%x put=%x\n",M3_FAULT_PHYSICAL,size,readl(bar+0xb83000),readl(bar+0xb83004),readl(bar+0xb83008),readl(bar+0xb8300c));
   fault_capture=true;
  }
  else if(op.kind==22){
   u64 actual,expected;
   if(op.bytes!=16){rc=-EINVAL;break;}
   expected=get_unaligned_le64(p);
   if(table_read(0x7e6aa1000ULL,1,&actual)){rc=-EIO;break;}
   if(!root_diverged&&actual!=expected){
    pr_err("M3_BOOT_FIRST_ROOT_DIFF boundary=%llu sends=%u replies=%u expected=%016llx actual=%016llx\n",op.address,sends,replies,expected,actual);root_diverged=true;
   }
  }
  else if(op.kind==23){rc=vram_write(op.address,p,op.bytes);if(rc)break;}
  else if(op.kind==24){rc=physical_copy(op.address,p,op.bytes);if(rc)break;}
  else if(op.kind==25){rc=runtime_probe();if(rc)break;}
  else if(op.kind==26){
#ifdef CONFIG_MODELOS_M3_RUNTIME_OBJECTS
   rc=runtime_objects_probe();if(rc)break;
#else
   rc=-EINVAL;break;
#endif
  }
  else if(op.kind==27){model_root=op.address;model_root_tier=op.value;have_model_root=true;pr_info("M3_BOOT_MODEL_PDB root=%llx tier=%u\n",model_root,model_root_tier);}
  else {rc=-EINVAL;break;}
  p+=op.bytes;
 }
out:
 if(rc&&bar){
  if(fault_capture){
   u32 *entry=__va(M3_FAULT_PHYSICAL);dma_rmb();
   pr_err("M3_BOOT_FAULT_ENTRY %08x %08x %08x %08x %08x %08x %08x %08x\n",entry[0],entry[1],entry[2],entry[3],entry[4],entry[5],entry[6],entry[7]);
  }
  pr_err("M3_BOOT_STATE gsp_cpuctl=%x gsp_riscv=%x gsp_irq=%x tx_head=%x tx_tail=%x rx_head=%x rx_tail=%x invalidate=%x\n",readl(bar+0x110040),readl(bar+0x1100f4),readl(bar+0x110094),readl(bar+0x110c00),readl(bar+0x110c04),readl(bar+0x110c80),readl(bar+0x110c84),readl(bar+0xb830b0));
  for(unsigned i=0;i<16;i++){u32 value=readl(bar+0xb81000+4*i);if(value)pr_err("M3_BOOT_PENDING leaf=%u value=%x\n",i,value);}
  for(unsigned i=0;i<2;i++)pr_err("M3_BOOT_FAULT buffer=%u lo=%x hi=%x get=%x put=%x\n",i,readl(bar+0xb83000+32*i),readl(bar+0xb83004+32*i),readl(bar+0xb83008+32*i),readl(bar+0xb8300c+32*i));
  pr_err("M3_BOOT_FAULT_DETAIL address_lo=%x address_hi=%x instance_lo=%x instance_hi=%x info=%x\n",readl(bar+0xb83080),readl(bar+0xb83084),readl(bar+0xb83088),readl(bar+0xb8308c),readl(bar+0xb83090));
  fault_walk();
 }
 pr_info("M3_BOOT_END status=%d operations=%llu sends=%u replies=%u irq_events=%d rm_linked=0 checkpoint_a=%u\n",rc,index,sends,replies,atomic_read(&irq_events),cubin_checked);
 return 0; /* Failed diagnostic leaves serial live for teardown, no retry. */
}
late_initcall(boot);
