// SPDX-License-Identifier: MIT
/* Native QMD graph executable. No CUDA API, ioctl, userspace or SASS editing. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include "nvtypes.h"
#include "nvstatus.h"
#include "launch.h"
struct l1_launch_node {
 const char *name;const unsigned char *code,*constant;
 unsigned code_bytes,constant_bytes,registers,shared_bytes;int next;NvU32 qmd[96];
};
#include "l1-launch.h"
#if L1_LAUNCH_IS_QWEN
#include "l1-golden.h"
#endif
static void field(NvU32 *q,unsigned hi,unsigned lo,NvU64 value)
{for(unsigned b=lo;b<=hi;b++)q[b/32]=(q[b/32]&~(1U<<(b%32)))|(((b-lo<64?(value>>(b-lo))&1:0))<<(b%32));}
static NvU64 qmd_address(NvU64 base,unsigned node){return base+0x10000+node*0x200;}
static NvU64 constant_address(NvU64 base,unsigned node){return base+0x50000+node*0x400;}
NV_STATUS modelos_l1_launch(struct l1_launch_resources *resources)
{
 void *local_cpu=resources->local.cpu,*host_cpu=resources->host.cpu,*doorbell=resources->doorbell;
 NvU64 local_gpu=resources->local.gpu,host_gpu=resources->host.gpu;NvU32 token=resources->token;
 unsigned launch_count=3,case_id=0,position=0,matched=0,result=0;
#if L1_LAUNCH_IS_QWEN
 void *source=memremap(resources->weights_phys,resources->model_bytes,MEMREMAP_WB);
 if(!source)return NV_ERR_NO_MEMORY;
 if(memcmp(source,"MOSQ3B1",7)||*(NvU64 *)((char *)source+48)!=resources->model_bytes){memunmap(source);return NV_ERR_INVALID_DATA;}
 for(NvU64 off=0;off<resources->model_bytes;off+=0x10000){memcpy_toio((void __iomem *)resources->weights.cpu+off,(char *)source+off,min_t(NvU64,0x10000,resources->model_bytes-off));cond_resched();}
 wmb();memunmap(source);launch_count=0;
 for(unsigned c=0;c<ARRAY_SIZE(l1_cases);c++)launch_count+=l1_cases[c].inputs+l1_cases[c].outputs-1;
 pr_info("L1_GRAPH_QWEN_WEIGHTS bytes=%llu gpu=0x%llx\n",resources->model_bytes,resources->weights.gpu);
#endif
 unsigned n=ARRAY_SIZE(graph_nodes),i,j,put=0;NvU64 code_gpu[ARRAY_SIZE(graph_nodes)],cursor=local_gpu+0x100000;
 NvU32 templates[ARRAY_SIZE(graph_nodes)][96],*queue=host_cpu;
 NvU32 *released=(void *)((char *)host_cpu+0x1000);
 for(i=0;i<n;i++) {
  const struct l1_launch_node *node=&graph_nodes[i];NvU32 cb[256]={0},*q=templates[i];NvU64 ca=constant_address(local_gpu,i);
  if(node->constant_bytes>sizeof(cb)||cursor+node->code_bytes>local_gpu+0x200000)return NV_ERR_INVALID_ARGUMENT;
  code_gpu[i]=cursor;cursor=ALIGN(cursor+node->code_bytes,0x100);
  memcpy_toio((void __iomem *)local_cpu+code_gpu[i]-local_gpu,node->code,node->code_bytes);
  memcpy(cb,node->constant,node->constant_bytes);*(NvU64 *)((char *)cb+0x380)=host_gpu;
#if L1_LAUNCH_IS_QWEN
  *(NvU64 *)((char *)cb+0x388)=resources->weights.gpu;*(NvU64 *)((char *)cb+0x390)=resources->state.gpu;*(NvU64 *)((char *)cb+0x398)=resources->state.gpu+0x3c00000;
#endif
  cb[188]=0;cb[189]=0x7294;cb[190]=0;cb[191]=0x7293;cb[223]=0xfffdc0;
  memcpy_toio((void __iomem *)local_cpu+ca-local_gpu,cb,sizeof(cb));
  memcpy(q,node->qmd,sizeof(templates[i]));memset(q,0,16);
  field(q,1199,1184,0);field(q,1215,1200,0);
  field(q,1055,1024,(NvU32)(code_gpu[i]>>4));field(q,1076,1056,code_gpu[i]>>36);
  field(q,1085,1077,0);field(q,1087,1086,0);
  field(q,1919,1888,(NvU32)(code_gpu[i]>>8));field(q,1936,1920,code_gpu[i]>>40);
  for(j=0;j<8;j++) {
   field(q,1375+j*64,1344+j*64,j?0:(NvU32)(ca>>6));field(q,1394+j*64,1376+j*64,j?0:ca>>38);
   field(q,1407+j*64,1395+j*64,j?0:sizeof(cb)/16);field(q,1856+j*4+3,1856+j*4,j?0:1);
  }
  field(q,351,336,0);field(q,447,352,0);field(q,623,608,0);
  field(q,703,640,0);field(q,734,730,0);
  field(q,288,288,i+1==n);field(q,290,289,1);field(q,291,291,1);field(q,300,300,0);field(q,304,304,0);
  field(q,511,480,(NvU32)(host_gpu+0x1000));field(q,536,512,(host_gpu+0x1000)>>32);
  if(node->next>=0){field(q,336,336,1);field(q,339,337,1);field(q,415,384,qmd_address(local_gpu,node->next)>>8);field(q,350,350,1);}
  pr_info("L1_GRAPH_NODE index=%u entry=%s next=%d code=0x%llx qmd=0x%llx regs=%u\n",i,node->name,node->next,code_gpu[i],qmd_address(local_gpu,i),node->registers);
 }
 wmb();pr_info("L1_GRAPH_UPLOAD nodes=%u code_bytes=%llu pools=existing_RM_VRAM typed_edges=1\n",n,cursor-local_gpu-0x100000);
 for(unsigned launch=1;launch<=launch_count;launch++) {
  NvU32 push[12];NvU64 entry,deadline,start=ktime_get_ns();
  #if L1_LAUNCH_IS_QWEN
  WRITE_ONCE(queue[2],position<l1_cases[case_id].inputs?l1_cases[case_id].input[position]:result);WRITE_ONCE(queue[3],position);WRITE_ONCE(queue[0],launch);
#else
  for(i=0;i<32;i++)WRITE_ONCE(queue[16+i],0xdeadbeef);
#endif
  WRITE_ONCE(*released,0);
  for(i=0;i<n;i++) {
   field(templates[i],575,544,launch);field(templates[i],607,576,0);
   memcpy_toio((void __iomem *)local_cpu+qmd_address(local_gpu,i)-local_gpu,templates[i],sizeof(templates[i]));
  }
  push[0]=0x20012000;push[1]=0xcec0;
  push[2]=0x20022000|(0x7b0/4);push[3]=0x7293;push[4]=0;
  push[5]=0x20022000|(0x2a0/4);push[6]=0x7294;push[7]=0;
  push[8]=0x20012000|(0x2b4/4);push[9]=qmd_address(local_gpu,0)>>8;
  push[10]=0x20012000|(0x2c0/4);push[11]=9;
  memcpy_toio((void __iomem *)local_cpu+0xc0000+(put&1023)*64,push,sizeof(push));
  entry=cpu_to_le64((local_gpu+0xc0000+(put&1023)*64)|(12ULL<<42)|(1ULL<<41));
  memcpy_toio((void __iomem *)local_cpu+(put&1023)*8,&entry,8);
  wmb();writel(++put,(void __iomem *)local_cpu+0x208c);wmb();writel(token,(void __iomem *)doorbell+0x90);
  deadline=ktime_get_ns()+20ULL*NSEC_PER_SEC;
  while(READ_ONCE(*released)!=launch&&ktime_get_ns()<deadline)usleep_range(5,10);
  if(READ_ONCE(*released)!=launch){pr_err("L1_GRAPH_TIMEOUT launch=%u release=%u\n",launch,READ_ONCE(*released));return NV_ERR_TIMEOUT;}
  dma_rmb();
#if L1_LAUNCH_IS_QWEN
  result=READ_ONCE(queue[4]);
  if(position+1>=l1_cases[case_id].inputs){unsigned index=position+1-l1_cases[case_id].inputs;
   pr_info("L1_GRAPH_QWEN_TOKEN case=%u index=%u got=%u expected=%u\n",case_id,index,result,l1_cases[case_id].output[index]);
   if(result!=l1_cases[case_id].output[index])return NV_ERR_INVALID_DATA;++matched;
  }
  if(++position==l1_cases[case_id].inputs+l1_cases[case_id].outputs-1){pr_info("L1_GRAPH_QWEN_CASE_PASS case=%u\n",case_id);case_id++;position=0;}
#else
  for(i=0;i<32;i++)if(READ_ONCE(queue[16+i])!=6*i+36){pr_err("L1_GRAPH_MISMATCH launch=%u i=%u got=%u\n",launch,i,READ_ONCE(queue[16+i]));return NV_ERR_INVALID_DATA;}
  pr_info("L1_GRAPH_LAUNCH_PASS launch=%u nodes=%u host_submissions=1 words=32 first=%u last=%u elapsed_ns=%llu\n",launch,n,READ_ONCE(queue[16]),READ_ONCE(queue[47]),ktime_get_ns()-start);
#endif
 }
#if L1_LAUNCH_IS_QWEN
 pr_info("L1_GRAPH_QWEN_PASS cases=%u tokens=%u steps=%u cpl=0 mm=none no_libcuda=1\n",case_id,matched,launch_count);
#else
 pr_info("L1_GRAPH_PASS launches=3 nodes=3 words=96 cpl=0 mm=none no_libcuda=1\n");
#endif
 return NV_OK;
}
