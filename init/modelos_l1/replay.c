// SPDX-License-Identifier: MIT
/* L1 workload consumer. Calls only the trim lane's five direct RM bridges. */
#include "nv-linux.h"
#include <linux/pci.h>
#include "nvos.h"
#include "class/cl0070.h"
#include "class/cl90f1.h"
#include "class/cl9067.h"
#include "alloc/alloc_channel.h"
#include "ctrl/ctrla06c.h"
#include "ctrl/ctrlc36f.h"
#include "l1-image.h"

extern NV_STATUS modelos_rm_alloc(nvidia_stack_t *,NvHandle,NvHandle,NvHandle *,NvU32,void *,NvU32);
extern NV_STATUS modelos_rm_control(nvidia_stack_t *,NvHandle,NvHandle,NvU32,void *,NvU32);
extern NV_STATUS modelos_rm_map_cpu(nvidia_stack_t *,NvHandle,NvHandle,NvHandle,NvU64,NvU64,void **,NvU32);
extern NV_STATUS modelos_rm_map_dma(nvidia_stack_t *,NVOS46_PARAMETERS *);

struct l1_memory { NvHandle handle; void *cpu; NvU64 gpu,bytes; };
struct l1_context { nvidia_stack_t *stack; NvHandle client,device,sub,va,virt,group,ctx,channel,compute,usermode; void *doorbell; NvU32 token; };
#define L1_TRY(label,expr) do { NV_STATUS status=(expr); pr_info("L1_REPLAY_CALL %s status=0x%08x\n",label,status);if(status!=NV_OK)return status; } while(0)
static NV_STATUS l1_alloc(struct l1_context *c,NvHandle parent,NvHandle *h,NvU32 cls,void *p,NvU32 n)
{ return modelos_rm_alloc(c->stack,c->client,parent,h,cls,p,n); }
static NV_STATUS l1_memory(struct l1_context *c,struct l1_memory *m,const unsigned char *template,NvU32 cls,NvU64 size,NvU64 gpu,NvU32 cache)
{
 NV_MEMORY_ALLOCATION_PARAMS p;
 NVOS46_PARAMETERS dma={0};
 memcpy(&p,template,sizeof(p));p.owner=p.type==13?c->device:c->client;p.size=size;
 if(cls==0x40 && cache==2)p.attr=(p.attr&~(7U<<29))|(2U<<29); /* Match VRAM allocation coherence to its WC CPU map. */
 L1_TRY("memory-alloc",l1_alloc(c,c->device,&m->handle,cls,&p,sizeof(p)));
 dma.hClient=c->client;dma.hDevice=c->device;dma.hDma=c->virt;dma.hMemory=m->handle;
 dma.length=size;dma.dmaOffset=gpu;dma.flags=1U<<15;
 L1_TRY("memory-gpu-map",modelos_rm_map_dma(c->stack,&dma));
 m->gpu=dma.dmaOffset;m->bytes=size;
 L1_TRY("memory-cpu-map",modelos_rm_map_cpu(c->stack,c->client,c->sub,m->handle,0,size,&m->cpu,cache<<23));
 pr_info("L1_REPLAY_MEMORY class=0x%x bytes=%llu gpu=0x%llx cpu=%px\n",cls,size,m->gpu,m->cpu);
 if(size<=0x200000)memset_io((void __iomem *)m->cpu,0,size);
 return NV_OK;
}
static void l1_bits(NvU32 *q,unsigned hi,unsigned lo,NvU64 value)
{
 unsigned bit;
 for(bit=lo;bit<=hi;bit++)q[bit/32]=(q[bit/32]&~(1U<<(bit%32)))|(((value>>(bit-lo))&1)<<(bit%32));
}
/* Fixed GB202 PF profile, from the recorded window and driver dev_vm.h:
 * virtualRegPhysOffset 0xb80000 + NV_VIRTUAL_FUNCTION base 0x30000.
 * PCI BAR addresses are discovered on this boot; no recorded GPA is reused.
 * This replaces the C661 CPU resource and RM CPU-map operation only. */
static NV_STATUS l2_map_doorbell(struct l1_context *c)
{
 struct pci_dev *dev=NULL;
 resource_size_t base;
 while((dev=pci_get_device(0x10de,PCI_ANY_ID,dev))) {
  if((dev->class>>16)!=PCI_BASE_CLASS_DISPLAY)continue;
  if(dev->device!=0x2b85 || !(pci_resource_flags(dev,0)&IORESOURCE_MEM) || pci_resource_len(dev,0)<0xbc0000) {
   pci_dev_put(dev);return NV_ERR_NOT_SUPPORTED;
  }
  base=pci_resource_start(dev,0);
  c->doorbell=ioremap(base+0xbb0000,0x10000);
  pr_info("L2_DIRECT_BAR0 pci=%s bar=%pa offset=0xbb0000 bytes=65536 mapped=%u\n",pci_name(dev),&base,!!c->doorbell);
  pci_dev_put(dev);
  return c->doorbell?NV_OK:NV_ERR_NO_MEMORY;
 }
 return NV_ERR_INVALID_DEVICE;
}
static NV_STATUS l1_execute(struct l1_context *c,struct l1_memory *local,struct l1_memory *host)
{
 NvU32 q[96],push[12],constants[228]={0};
 NvU64 code=local->gpu+0x10000,qmd=local->gpu+0x30000,cb=local->gpu+0x31000;
 NvU64 entry,deadline;NvU32 *queue=host->cpu;NvU32 *released=(void *)((char *)host->cpu+0x1000);
 unsigned i;bool correct=true;
 memcpy(q,l1_qmd,sizeof(q));memcpy(constants,l1_constant0,sizeof(l1_constant0));
 *(NvU64 *)((char *)constants+0x380)=host->gpu;
 constants[188]=0;constants[189]=0x7294;constants[190]=0;constants[191]=0x7293;constants[223]=0xfffdc0;
 l1_bits(q,1199,1184,0);l1_bits(q,1215,1200,0); /* PTX has no local memory/stack. */
 l1_bits(q,1055,1024,(NvU32)(code>>4));l1_bits(q,1076,1056,code>>36);
 l1_bits(q,1919,1888,(NvU32)(code>>8));l1_bits(q,1936,1920,code>>40);
 l1_bits(q,1375,1344,(NvU32)(cb>>6));l1_bits(q,1394,1376,cb>>38);
 l1_bits(q,1856,1856,1);
 l1_bits(q,288,288,1);l1_bits(q,290,289,1);l1_bits(q,291,291,1);l1_bits(q,300,300,0);
 l1_bits(q,511,480,(NvU32)(host->gpu+0x1000));l1_bits(q,536,512,(host->gpu+0x1000)>>32);
 l1_bits(q,575,544,1);l1_bits(q,607,576,0);l1_bits(q,304,304,0);
 /* No dependent QMD, latch or reference-count links from CUDA's scheduler. */

 l1_bits(q,351,336,0);l1_bits(q,447,384,0);l1_bits(q,623,608,0);
 for(i=0;i<32;i++)WRITE_ONCE(queue[16+i],0xdeadbeef);
 WRITE_ONCE(queue[0],0);WRITE_ONCE(queue[1],0);WRITE_ONCE(*released,0);
 memcpy_toio((void __iomem *)local->cpu+0x10000,l1_code,sizeof(l1_code));
 memcpy_toio((void __iomem *)local->cpu+0x30000,q,sizeof(q));
 memcpy_toio((void __iomem *)local->cpu+0x31000,constants,sizeof(constants));
 push[0]=0x20012000;push[1]=0xcec0;
 push[2]=0x20022000|(0x7b0/4);push[3]=0x7293;push[4]=0;
 push[5]=0x20022000|(0x2a0/4);push[6]=0x7294;push[7]=0;
 push[8]=0x20012000|(0x2b4/4);push[9]=qmd>>8;
 push[10]=0x20012000|(0x2c0/4);push[11]=9;
 memcpy_toio((void __iomem *)local->cpu+0x20000,push,sizeof(push));
 entry=cpu_to_le64((local->gpu+0x20000)|(12ULL<<42)|(1ULL<<41));
 memcpy_toio((void __iomem *)local->cpu,&entry,sizeof(entry));
 wmb();writel(1,(void __iomem *)local->cpu+0x208c);wmb();
 pr_info("L1_REPLAY_SUBMIT token=0x%x code=0x%llx qmd=0x%llx queue=0x%llx\n",c->token,code,qmd,host->gpu);
 writel(c->token,(void __iomem *)c->doorbell+0x90);
 WRITE_ONCE(queue[0],1);wmb();
 deadline=ktime_get_ns()+5ULL*NSEC_PER_SEC;
 while(READ_ONCE(queue[1])!=1 && ktime_get_ns()<deadline)usleep_range(50,100);
 dma_rmb();
 for(i=0;i<32;i++)if(READ_ONCE(queue[16+i])!=3*i+7)correct=false;
 pr_info("L1_REPLAY_REQUEST done=%u first=%u last=%u correct=%u gpget=%u\n",READ_ONCE(queue[1]),READ_ONCE(queue[16]),READ_ONCE(queue[47]),correct,readl((void __iomem *)local->cpu+0x2088));
 WRITE_ONCE(queue[0],2);wmb();
 deadline=ktime_get_ns()+5ULL*NSEC_PER_SEC;
 while(READ_ONCE(*released)!=1 && ktime_get_ns()<deadline)usleep_range(50,100);
 pr_info("L1_REPLAY_COMPLETION release=%u\n",READ_ONCE(*released));
 if(READ_ONCE(*released)!=1)return NV_ERR_TIMEOUT;
 if(!correct||READ_ONCE(queue[1])!=1)return NV_ERR_INVALID_DATA;
 pr_info("L1_REPLAY_PERSISTENT_PASS words=32 first=7 last=100 cpl=0 no_userspace=1\n");
 return NV_OK;
}
#ifdef CONFIG_MODELOS_L1_QWEN
#include "l1-golden.h"
static NV_STATUS l1_step(NvU32 *queue,NvU32 *serial,NvU32 token,NvU32 position,NvU32 *result)
{
 NvU64 deadline=ktime_get_ns()+20ULL*NSEC_PER_SEC;
 WRITE_ONCE(queue[2],token);WRITE_ONCE(queue[3],position);wmb();
 ++*serial;WRITE_ONCE(queue[0],*serial);wmb();
 while(READ_ONCE(queue[1])!=*serial && ktime_get_ns()<deadline)usleep_range(50,100);
 if(READ_ONCE(queue[1])!=*serial) {pr_err("L1_QWEN_STEP_TIMEOUT serial=%u token=%u pos=%u\n",*serial,token,position);return NV_ERR_TIMEOUT;}
 dma_rmb();if(READ_ONCE(queue[5]))return NV_ERR_INVALID_DATA;
 *result=READ_ONCE(queue[4]);return NV_OK;
}
static NV_STATUS l1_qwen(struct l1_context *c,struct l1_memory *local,struct l1_memory *host)
{
 struct l1_memory weights={0},state={0};
 const NvU64 bytes=1192235008,physical=0x100000000ULL;
 void *source;NvU32 q[96],push[12],constants[232]={0};
 NvU64 code=local->gpu+0x40000,qmd=local->gpu+0x30000,cb=local->gpu+0x31000,entry,deadline,start;
 NvU32 *queue=host->cpu,*released=(void *)((char *)host->cpu+0x1000),serial=0,result=0;
 unsigned i,j,matched=0;NV_STATUS status=NV_OK;
 source=memremap(physical,bytes,MEMREMAP_WB);
 if(!source)return NV_ERR_NO_MEMORY;
 if(memcmp(source,"MOSQ3B1",7)||*(NvU64 *)((char *)source+48)!=bytes){memunmap(source);return NV_ERR_INVALID_DATA;}
 L1_TRY("qwen-weights",l1_memory(c,&weights,l1_local_memory,0x40,ALIGN(bytes,0x2000),0x10000000ULL,2));
 L1_TRY("qwen-state",l1_memory(c,&state,l1_local_memory,0x40,0x4000000,0x60000000ULL,2));
 for(NvU64 offset=0;offset<bytes;offset+=0x10000) {
  memcpy_toio((void __iomem *)weights.cpu+offset,(char *)source+offset,min_t(NvU64,0x10000,bytes-offset));
  cond_resched();
 }
 wmb();memunmap(source);
 pr_info("L1_QWEN_WEIGHTS_READY bytes=%llu GPU=0x%llx\n",bytes,weights.gpu);
 memcpy(q,l1_qmd,sizeof(q));memcpy(constants,l1_qwen_constant0,sizeof(l1_qwen_constant0));
 *(NvU64 *)((char *)constants+0x380)=host->gpu;
 *(NvU64 *)((char *)constants+0x388)=weights.gpu;
 *(NvU64 *)((char *)constants+0x390)=state.gpu;
 *(NvU64 *)((char *)constants+0x398)=state.gpu+0x3c00000;
 constants[188]=0;constants[189]=0x7294;constants[190]=0;constants[191]=0x7293;constants[223]=0xfffdc0;
 l1_bits(q,1199,1184,0);l1_bits(q,1215,1200,0);
 l1_bits(q,1136,1128,L1_QWEN_REGISTERS);l1_bits(q,1162,1152,DIV_ROUND_UP(L1_QWEN_SHARED_BYTES,128));
 l1_bits(q,1055,1024,(NvU32)(code>>4));l1_bits(q,1076,1056,code>>36);
 l1_bits(q,1919,1888,(NvU32)(code>>8));l1_bits(q,1936,1920,code>>40);
 l1_bits(q,1375,1344,(NvU32)(cb>>6));l1_bits(q,1394,1376,cb>>38);l1_bits(q,1407,1395,sizeof(constants)/16);
 l1_bits(q,1856,1856,1);l1_bits(q,288,288,1);l1_bits(q,290,289,1);l1_bits(q,291,291,1);l1_bits(q,300,300,0);
 l1_bits(q,511,480,(NvU32)(host->gpu+0x1000));l1_bits(q,536,512,(host->gpu+0x1000)>>32);
 l1_bits(q,575,544,1);l1_bits(q,607,576,0);l1_bits(q,304,304,0);
 l1_bits(q,351,336,0);l1_bits(q,447,384,0);l1_bits(q,623,608,0);
 memset(queue,0,4096);WRITE_ONCE(*released,0);
 memcpy_toio((void __iomem *)local->cpu+0x40000,l1_qwen_code,sizeof(l1_qwen_code));
 memcpy_toio((void __iomem *)local->cpu+0x30000,q,sizeof(q));
 memcpy_toio((void __iomem *)local->cpu+0x31000,constants,sizeof(constants));
 push[0]=0x20012000;push[1]=0xcec0;
 push[2]=0x20022000|(0x7b0/4);push[3]=0x7293;push[4]=0;
 push[5]=0x20022000|(0x2a0/4);push[6]=0x7294;push[7]=0;
 push[8]=0x20012000|(0x2b4/4);push[9]=qmd>>8;
 push[10]=0x20012000|(0x2c0/4);push[11]=9;
 memcpy_toio((void __iomem *)local->cpu+0x20000,push,sizeof(push));
 entry=cpu_to_le64((local->gpu+0x20000)|(12ULL<<42)|(1ULL<<41));memcpy_toio((void __iomem *)local->cpu,&entry,sizeof(entry));
 wmb();writel(1,(void __iomem *)local->cpu+0x208c);wmb();writel(c->token,(void __iomem *)c->doorbell+0x90);
 start=ktime_get_ns();
 pr_info("L1_QWEN_PERSISTENT_SUBMIT regs=%u smem=%u code_bytes=%zu\n",L1_QWEN_REGISTERS,L1_QWEN_SHARED_BYTES,sizeof(l1_qwen_code));
 for(i=0;i<ARRAY_SIZE(l1_cases);i++) {
  for(j=0;j<l1_cases[i].inputs;j++){status=l1_step(queue,&serial,l1_cases[i].input[j],j,&result);if(status!=NV_OK)goto stop;}
  for(j=0;j<l1_cases[i].outputs;j++) {
   pr_info("L1_QWEN_TOKEN case=%u index=%u got=%u expected=%u\n",i,j,result,l1_cases[i].output[j]);
   if(result!=l1_cases[i].output[j]){status=NV_ERR_INVALID_DATA;goto stop;}
   ++matched;
   if(j+1<l1_cases[i].outputs){status=l1_step(queue,&serial,result,l1_cases[i].inputs+j,&result);if(status!=NV_OK)goto stop;}
  }
  pr_info("L1_QWEN_CASE_PASS case=%u tokens=%u\n",i,l1_cases[i].outputs);
 }
stop:
 WRITE_ONCE(queue[0],0xffffffffU);wmb();deadline=ktime_get_ns()+20ULL*NSEC_PER_SEC;
 while(READ_ONCE(*released)!=1 && ktime_get_ns()<deadline)usleep_range(50,100);
 pr_info("L1_QWEN_FINISH status=0x%x release=%u matched=%u steps=%u elapsed_ns=%llu\n",status,READ_ONCE(*released),matched,serial,ktime_get_ns()-start);
 if(READ_ONCE(*released)!=1)return NV_ERR_TIMEOUT;
 if(status==NV_OK)pr_info("L1_QWEN_PASS cases=5 tokens=320 cpl=0 mm=none\n");
 return status;
}
#endif
NV_STATUS modelos_l1_resources(nvidia_stack_t *stack,NvHandle client,NvHandle device,NvHandle sub)
{
 struct l1_context c={.stack=stack,.client=client,.device=device,.sub=sub};
 NV_VASPACE_ALLOCATION_PARAMETERS va;
 NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS virt={0};
 NV_CHANNEL_GROUP_ALLOCATION_PARAMETERS grp;
 NV_CTXSHARE_ALLOCATION_PARAMETERS ctx;
 NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS fifo;
 NVA06C_CTRL_GPFIFO_SCHEDULE_PARAMS schedule={.bEnable=NV_TRUE};
 NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS token={0};
 struct l1_memory local={0},host={0},error={0};
 NV_STATUS result;
 memcpy(&va,l1_vaspace,sizeof(va));va.vaSize=(1ULL<<47)-0x1000;va.flags=0; /* RM-owned tables; no UVM/external page directory. */
 L1_TRY("vaspace",l1_alloc(&c,device,&c.va,0x90f1,&va,sizeof(va)));
 virt.hVASpace=c.va;virt.offset=0x1000;virt.limit=(1ULL<<47)-1;
 L1_TRY("virtual",l1_alloc(&c,device,&c.virt,0x70,&virt,sizeof(virt)));
 pr_info("L1_REPLAY_VIRTUAL base=0x%llx limit=0x%llx va_size=0x%llx\n",virt.offset,virt.limit,va.vaSize);
 memcpy(&grp,l1_group,sizeof(grp));
 L1_TRY("group",l1_alloc(&c,device,&c.group,0xa06c,&grp,sizeof(grp)));
 memcpy(&ctx,l1_ctxshare,sizeof(ctx));ctx.hVASpace=c.va;
 L1_TRY("ctxshare",l1_alloc(&c,c.group,&c.ctx,0x9067,&ctx,sizeof(ctx)));
 L1_TRY("local",l1_memory(&c,&local,l1_local_memory,0x40,0x200000,0x02000000,2));
 L1_TRY("host",l1_memory(&c,&host,l1_system_memory,0x3e,0x10000,0x01000000,0));
 L1_TRY("error",l1_memory(&c,&error,l1_error_memory,0x3e,0x1000,0x01100000,1));
 L1_TRY("direct-bar0-doorbell",l2_map_doorbell(&c));
 memcpy(&fifo,l1_fifo,sizeof(fifo));fifo.hObjectError=error.handle;fifo.hObjectBuffer=local.handle;
 fifo.gpFifoOffset=local.gpu;fifo.hContextShare=c.ctx;fifo.hUserdMemory[0]=local.handle;fifo.userdOffset[0]=0x2000;
 L1_TRY("fifo",l1_alloc(&c,c.group,&c.channel,0xc96f,&fifo,sizeof(fifo)));
 L1_TRY("compute",l1_alloc(&c,c.channel,&c.compute,0xcec0,NULL,0));
 L1_TRY("schedule",modelos_rm_control(stack,client,c.group,NVA06C_CTRL_CMD_GPFIFO_SCHEDULE,&schedule,sizeof(schedule)));
 L1_TRY("work-token",modelos_rm_control(stack,client,c.channel,NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,&token,sizeof(token)));
 c.token=token.workSubmitToken;
 pr_info("L1_REPLAY_RESOURCES_PASS token=0x%x no_submission=1\n",c.token);
#ifdef CONFIG_MODELOS_L1_QWEN
 result=l1_qwen(&c,&local,&host);
#else
 result=l1_execute(&c,&local,&host);
#endif
 if(result==NV_OK){iounmap((void __iomem *)c.doorbell);pr_info("L2_DIRECT_BAR0_UNMAP status=0x0\n");}
 return result;
}
