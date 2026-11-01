// SPDX-License-Identifier: MIT
/* L1 workload consumer. Calls only the trim lane's five direct RM bridges. */
#include "nv-linux.h"
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
/* Trace VM: bounded symbols and SDK parameter relocations, no ioctl layer. */
enum { L1_OP_ALLOC,L1_OP_MEMORY,L1_OP_MAP_CPU,L1_OP_CONTROL };
struct l1_expression { int slot; unsigned field; NvU64 add; };
struct l1_relocation { unsigned offset,width; struct l1_expression value; };
struct l1_operation {
 unsigned kind,out,cls,command,cache,flags,capture,qwen_only;
 const char *name;const unsigned char *params;unsigned params_size;
 struct l1_expression parent,object,size,gpu;
 const struct l1_relocation *patches;unsigned patch_count;
};
#include "l1-plan.h"
struct l1_resource { NvU64 value;struct l1_memory memory; };
struct l1_context { struct l1_resource resources[L1_RESOURCE_COUNT]; nvidia_stack_t *stack; NvHandle client,device,sub,va,virt,group,ctx,channel,compute,usermode; void *doorbell; NvU32 token; };
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
static u64 l1_boot_weights_phys,l1_boot_gpu_bias;
static int __init l1_physical_arg(char *s){return !kstrtou64(s,0,&l1_boot_weights_phys);}
static int __init l1_bias_arg(char *s){return !kstrtou64(s,0,&l1_boot_gpu_bias);}
__setup("l1_weights_phys=",l1_physical_arg);
__setup("l1_gpu_bias=",l1_bias_arg);
static NvU64 l1_value(struct l1_context *c,struct l1_expression e)
{
 NvU64 value=0;
 if(e.slot>=0) {
  struct l1_resource *r=&c->resources[e.slot];
  switch(e.field){case 0:value=r->value;break;case 1:value=r->memory.gpu;break;
   case 2:value=(NvU64)(unsigned long)r->memory.cpu;break;case 3:value=r->memory.bytes;break;}
 }
 return value+e.add;
}
static NV_STATUS l1_replay(struct l1_context *c)
{
 unsigned i,j;unsigned char params[512] __aligned(8);
 static_assert(sizeof(NVA06C_CTRL_GPFIFO_SCHEDULE_PARAMS)==sizeof(l1_plan_schedule));
 static_assert(sizeof(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS)==sizeof(l1_plan_fifo));
 for(i=0;i<L1_RESOURCE_COUNT;i++)c->resources[i].value=l1_defaults[i];
 c->resources[L1_CLIENT].value=c->client;c->resources[L1_DEVICE].value=c->device;c->resources[L1_SUB].value=c->sub;
 if(l1_boot_weights_phys)c->resources[L1_WEIGHTS_PHYS].value=l1_boot_weights_phys;
 c->resources[L1_GPU_BIAS].value+=l1_boot_gpu_bias;
 for(i=0;i<ARRAY_SIZE(l1_operations);i++) {
  const struct l1_operation *op=&l1_operations[i];
  struct l1_resource *r=&c->resources[op->out];NV_STATUS status=NV_ERR_INVALID_ARGUMENT;
  NvHandle handle=0;
  if(op->qwen_only&&!IS_ENABLED(CONFIG_MODELOS_L1_QWEN))continue;
  if(op->out>=L1_RESOURCE_COUNT||op->params_size>sizeof(params))return NV_ERR_INVALID_ARGUMENT;
  memset(params,0,sizeof(params));if(op->params_size)memcpy(params,op->params,op->params_size);
  for(j=0;j<op->patch_count;j++) {
   const struct l1_relocation *p=&op->patches[j];NvU64 value=l1_value(c,p->value);
   if((p->width!=4&&p->width!=8)||p->offset+p->width>op->params_size)return NV_ERR_INVALID_ARGUMENT;
   if(p->width==4&&value>U32_MAX)return NV_ERR_INVALID_ARGUMENT;
   memcpy(params+p->offset,&value,p->width);
  }
  switch(op->kind) {
  case L1_OP_ALLOC:
   status=l1_alloc(c,l1_value(c,op->parent),&handle,op->cls,op->params_size?params:NULL,op->params_size);
   if(status==NV_OK)r->value=handle;
   break;
  case L1_OP_MEMORY:
   status=l1_memory(c,&r->memory,params,op->cls,l1_value(c,op->size),l1_value(c,op->gpu),op->cache);
   if(status==NV_OK)r->value=r->memory.handle;
   break;
  case L1_OP_MAP_CPU:
   status=modelos_rm_map_cpu(c->stack,c->client,c->sub,l1_value(c,op->object),0,l1_value(c,op->size),&r->memory.cpu,op->flags);
   break;
  case L1_OP_CONTROL:
   status=modelos_rm_control(c->stack,c->client,l1_value(c,op->object),op->command,params,op->params_size);
   if(status==NV_OK&&op->capture){NvU32 v;memcpy(&v,params,sizeof(v));r->value=v;}
   break;
  }
  pr_info("L1_TRACE_OP index=%u resource=%s status=0x%x value=0x%llx gpu=0x%llx bytes=%llu\n",i,op->name,status,r->value,r->memory.gpu,r->memory.bytes);
  if(status!=NV_OK)return status;
  if(op->out==L1_VIRT)c->virt=r->value;
 }
 c->doorbell=c->resources[L1_DOORBELL].memory.cpu;c->token=c->resources[L1_TOKEN].value;
 pr_info("L1_TRACE_PASS format=modelos-l1-rm version=1 operations=%zu symbols=%u gpu_bias=0x%llx weights_phys=0x%llx\n",ARRAY_SIZE(l1_operations),L1_RESOURCE_COUNT,c->resources[L1_GPU_BIAS].value,c->resources[L1_WEIGHTS_PHYS].value);
 return NV_OK;
}
static void l1_bits(NvU32 *q,unsigned hi,unsigned lo,NvU64 value)
{
 unsigned bit;
 for(bit=lo;bit<=hi;bit++)q[bit/32]=(q[bit/32]&~(1U<<(bit%32)))|(((value>>(bit-lo))&1)<<(bit%32));
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
 struct l1_memory weights=c->resources[L1_WEIGHTS].memory,state=c->resources[L1_STATE].memory;
 const NvU64 bytes=c->resources[L1_MODEL_BYTES].value,physical=c->resources[L1_WEIGHTS_PHYS].value;
 void *source;NvU32 q[96],push[12],constants[232]={0};
 NvU64 code=local->gpu+0x40000,qmd=local->gpu+0x30000,cb=local->gpu+0x31000,entry,deadline,start;
 NvU32 *queue=host->cpu,*released=(void *)((char *)host->cpu+0x1000),serial=0,result=0;
 unsigned i,j,matched=0;NV_STATUS status=NV_OK;
 source=memremap(physical,bytes,MEMREMAP_WB);
 if(!source)return NV_ERR_NO_MEMORY;
 if(memcmp(source,"MOSQ3B1",7)||*(NvU64 *)((char *)source+48)!=bytes){memunmap(source);return NV_ERR_INVALID_DATA;}
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
 L1_TRY("resource-trace",l1_replay(&c));
 pr_info("L1_REPLAY_RESOURCES_PASS token=0x%x no_submission=1\n",c.token);
#ifdef CONFIG_MODELOS_L1_QWEN
 return l1_qwen(&c,&c.resources[L1_LOCAL].memory,&c.resources[L1_HOST].memory);
#else
 return l1_execute(&c,&c.resources[L1_LOCAL].memory,&c.resources[L1_HOST].memory);
#endif
}
