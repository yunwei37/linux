// SPDX-License-Identifier: MIT
/* Typed graph executor and matched launch controls; GPU instruction bytes unchanged. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include "nvtypes.h"
#include "nvstatus.h"
#include "launch.h"
struct l1_launch_node {const char *name;const unsigned char *code,*constant;unsigned code_bytes,constant_bytes,registers,shared_bytes;int next;NvU32 qmd[96];};
#include "l1-launch.h"
#if L1_LAUNCH_IS_QWEN
#include "l1-golden.h"
#endif
struct runtime_node {NvU32 qmd[96];NvU64 code;};
struct executor {struct l1_launch_resources *r;unsigned put,completion;bool system;struct runtime_node nodes[];};
static bool benchmark_mode,system_commands=true;
static int __init mode_arg(char *s){benchmark_mode=!strcmp(s,"bench");return 1;}
static int __init memory_arg(char *s){system_commands=strcmp(s,"vram")!=0;return 1;}
__setup("l1_launch_mode=",mode_arg);__setup("l1_launch_memory=",memory_arg);
static void field(NvU32 *q,unsigned hi,unsigned lo,NvU64 value)
{for(unsigned b=lo;b<=hi;b++)q[b/32]=(q[b/32]&~(1U<<(b%32)))|((b-lo<64?(value>>(b-lo))&1:0)<<(b%32));}
static NvU64 command_gpu(struct executor *c){return c->system?c->r->host.gpu:c->r->local.gpu;}
static void store(struct executor *c,unsigned off,const void *p,unsigned n)
{if(c->system)memcpy((char *)c->r->host.cpu+off,p,n);else memcpy_toio((void __iomem *)c->r->local.cpu+off,p,n);}
static NvU64 qmd_address(struct executor *c,unsigned i){return command_gpu(c)+0x10000+i*0x200;}
static NvU64 cb_address(struct executor *c,unsigned i){return command_gpu(c)+0x60000+i*0x400;}
static NvU32 *queue(struct executor *c){return c->r->host.cpu;}
static NvU32 *release_ptr(struct executor *c){return (void *)((char *)c->r->host.cpu+0x1000);}
static NvU64 counter(NvU32 *q,unsigned i){return READ_ONCE(*(NvU64 *)(q+i));}
static NV_STATUS wait_counter(NvU32 *p,NvU32 wanted)
{
 NvU64 deadline=ktime_get_ns()+20ULL*NSEC_PER_SEC;unsigned polls=0;
 while(READ_ONCE(*p)!=wanted){cpu_relax();if((++polls&1023)==0&&ktime_get_ns()>deadline)return NV_ERR_TIMEOUT;if((polls&65535)==0)cond_resched();}
 dma_rmb();return NV_OK;
}
static NV_STATUS prepare(struct executor *c)
{
 NvU64 cursor=c->r->local.gpu+0x100000;unsigned i,j,n=ARRAY_SIZE(graph_nodes);
 if(n>512)return NV_ERR_INVALID_ARGUMENT;
 for(i=0;i<n;i++){
  const struct l1_launch_node *node=&graph_nodes[i];NvU32 cb[256]={0},*q=c->nodes[i].qmd;NvU64 ca=cb_address(c,i);
  if(node->constant_bytes>sizeof(cb)||node->next>=(int)n||cursor+node->code_bytes>c->r->local.gpu+0x200000)return NV_ERR_INVALID_ARGUMENT;
  c->nodes[i].code=cursor;cursor=ALIGN(cursor+node->code_bytes,0x100);
  memcpy_toio((void __iomem *)c->r->local.cpu+c->nodes[i].code-c->r->local.gpu,node->code,node->code_bytes);
  memcpy(cb,node->constant,node->constant_bytes);*(NvU64 *)((char *)cb+0x380)=c->r->host.gpu;
#if L1_LAUNCH_IS_QWEN
  *(NvU64 *)((char *)cb+0x388)=c->r->weights.gpu;*(NvU64 *)((char *)cb+0x390)=c->r->state.gpu;*(NvU64 *)((char *)cb+0x398)=c->r->state.gpu+0x3c00000;
#endif
  cb[188]=0;cb[189]=0x7294;cb[190]=0;cb[191]=0x7293;cb[223]=0xfffdc0;store(c,ca-command_gpu(c),cb,sizeof(cb));
  memcpy(q,node->qmd,sizeof(c->nodes[i].qmd));memset(q,0,16);
  field(q,1199,1184,0);field(q,1215,1200,0);
  field(q,1055,1024,(NvU32)(c->nodes[i].code>>4));field(q,1076,1056,c->nodes[i].code>>36);
  field(q,1085,1077,0);field(q,1087,1086,0);field(q,1919,1888,(NvU32)(c->nodes[i].code>>8));field(q,1936,1920,c->nodes[i].code>>40);
  for(j=0;j<8;j++){
   field(q,1375+j*64,1344+j*64,j?0:(NvU32)(ca>>6));field(q,1394+j*64,1376+j*64,j?0:ca>>38);
   field(q,1407+j*64,1395+j*64,j?0:sizeof(cb)/16);field(q,1856+j*4+3,1856+j*4,j?0:1);
  }
  field(q,351,336,0);field(q,447,352,0);field(q,623,608,0);field(q,703,640,0);field(q,734,730,0);
  field(q,288,288,node->next<0);field(q,290,289,1);field(q,291,291,1);field(q,300,300,0);field(q,304,304,0);
  field(q,511,480,(NvU32)(c->r->host.gpu+0x1000));field(q,536,512,(c->r->host.gpu+0x1000)>>32);
  if(node->next>=0){field(q,336,336,1);field(q,339,337,1);field(q,415,384,qmd_address(c,node->next)>>8);field(q,350,350,1);}
  pr_info("L1_GRAPH_NODE index=%u entry=%s next=%d code=0x%llx qmd=0x%llx regs=%u\n",i,node->name,node->next,c->nodes[i].code,qmd_address(c,i),node->registers);
 }
 wmb();pr_info("L1_GRAPH_UPLOAD nodes=%u code_bytes=%llu command_memory=%s typed_edges=1\n",n,cursor-c->r->local.gpu-0x100000,c->system?"system":"vram");return NV_OK;
}
static NvU32 submit(struct executor *c,unsigned root,bool timestamp)
{
 NvU32 push[32],completion=++c->completion;NvU64 entry;unsigned words=0,slot=c->put&1023;
 WRITE_ONCE(*release_ptr(c),0);WRITE_ONCE(*(NvU64 *)((char *)c->r->host.cpu+0x1108),0);
 for(int node=root;node>=0;node=graph_nodes[node].next){field(c->nodes[node].qmd,575,544,completion);field(c->nodes[node].qmd,607,576,0);store(c,qmd_address(c,node)-command_gpu(c),c->nodes[node].qmd,sizeof(c->nodes[node].qmd));}
 push[words++]=0x20012000;push[words++]=0xcec0;
 push[words++]=0x20022000|(0x7b0/4);push[words++]=0x7293;push[words++]=0;
 push[words++]=0x20022000|(0x2a0/4);push[words++]=0x7294;push[words++]=0;
 if(timestamp){
  NvU64 addr=c->r->host.gpu+0x1100;
  push[words++]=0x20050000|(0x5c/4);push[words++]=(NvU32)addr;push[words++]=addr>>32;push[words++]=0;push[words++]=0;
  push[words++]=1U|(1U<<20)|(1U<<24)|(1U<<25);
 }
 push[words++]=0x20012000|(0x2b4/4);push[words++]=qmd_address(c,root)>>8;
 push[words++]=0x20012000|(0x2c0/4);push[words++]=9;
 store(c,0xe0000+slot*128,push,words*4);
 entry=cpu_to_le64((command_gpu(c)+0xe0000+slot*128)|((NvU64)words<<42)|(1ULL<<41));
 memcpy_toio((void __iomem *)c->r->local.cpu+slot*8,&entry,8);wmb();
 writel((++c->put)&1023,(void __iomem *)c->r->local.cpu+0x208c);wmb();writel(c->r->token,(void __iomem *)c->r->doorbell+0x90);
 return completion;
}
static NV_STATUS smoke(struct executor *c,unsigned root,unsigned launches)
{
 NvU32 *q=queue(c);
 for(unsigned launch=1;launch<=launches;launch++){
  for(unsigned i=0;i<32;i++)WRITE_ONCE(q[16+i],0xdeadbeef);
  NvU64 start=ktime_get_ns();NvU32 completion=submit(c,root,true);NV_STATUS s=wait_counter(release_ptr(c),completion);if(s!=NV_OK)return s;
  for(unsigned i=0;i<32;i++)if(READ_ONCE(q[16+i])!=6*i+36)return NV_ERR_INVALID_DATA;
  pr_info("L1_GRAPH_LAUNCH_PASS launch=%u nodes=3 host_submissions=1 words=32 first=%u last=%u elapsed_ns=%llu fe_timestamp=%llu\n",launch,READ_ONCE(q[16]),READ_ONCE(q[47]),ktime_get_ns()-start,counter(q,1090));
 }
 return NV_OK;
}
#if L1_LAUNCH_IS_QWEN
static NV_STATUS upload_model(struct executor *c)
{
 void *source=memremap(c->r->weights_phys,c->r->model_bytes,MEMREMAP_WB);if(!source)return NV_ERR_NO_MEMORY;
 if(memcmp(source,"MOSQ3B1",7)||*(NvU64 *)((char *)source+48)!=c->r->model_bytes){memunmap(source);return NV_ERR_INVALID_DATA;}
 for(NvU64 off=0;off<c->r->model_bytes;off+=0x10000){memcpy_toio((void __iomem *)c->r->weights.cpu+off,(char *)source+off,min_t(NvU64,0x10000,c->r->model_bytes-off));cond_resched();}
 wmb();memunmap(source);pr_info("L1_GRAPH_QWEN_WEIGHTS bytes=%llu gpu=0x%llx\n",c->r->model_bytes,c->r->weights.gpu);return NV_OK;
}
static NV_STATUS graph_step(struct executor *c,NvU32 serial,NvU32 token,NvU32 position,unsigned root,NvU32 *result)
{
 NvU32 *q=queue(c);WRITE_ONCE(q[2],token);WRITE_ONCE(q[3],position);WRITE_ONCE(q[0],serial);wmb();
 NV_STATUS s=wait_counter(release_ptr(c),submit(c,root,true));if(s!=NV_OK)return s;
 if(READ_ONCE(q[5]))return NV_ERR_INVALID_DATA;*result=READ_ONCE(q[4]);return NV_OK;
}
static NV_STATUS qwen_graph(struct executor *c)
{
 unsigned matched=0,serial=0;NvU32 result=0;
 for(unsigned id=0;id<ARRAY_SIZE(l1_cases);id++){
  for(unsigned p=0;p<l1_cases[id].inputs+l1_cases[id].outputs-1;p++){
   NV_STATUS s=graph_step(c,++serial,p<l1_cases[id].inputs?l1_cases[id].input[p]:result,p,0,&result);if(s!=NV_OK)return s;
   if(p+1>=l1_cases[id].inputs){unsigned index=p+1-l1_cases[id].inputs;
    pr_info("L1_GRAPH_QWEN_TOKEN case=%u index=%u got=%u expected=%u\n",id,index,result,l1_cases[id].output[index]);if(result!=l1_cases[id].output[index])return NV_ERR_INVALID_DATA;++matched;
   }
  }
  pr_info("L1_GRAPH_QWEN_CASE_PASS case=%u\n",id);
 }
 pr_info("L1_GRAPH_QWEN_PASS cases=5 tokens=%u steps=%u cpl=0 mm=none no_libcuda=1\n",matched,serial);return NV_OK;
}
static NV_STATUS benchmark(struct executor *c)
{
 static const unsigned roots[4]={3,0,4,5};static const char *names[4]={"finite","graph","persistent","graph_persistent"};
 NvU32 *q=queue(c),result=0;NV_STATUS s;
 if(ARRAY_SIZE(graph_nodes)<9)return NV_ERR_INVALID_ARGUMENT;
 s=smoke(c,6,3);if(s!=NV_OK)return s;
 for(unsigned p=0;p<32;p++){
  s=graph_step(c,p+1,p<l1_cases[0].inputs?l1_cases[0].input[p]:result,p,0,&result);if(s!=NV_OK)return s;
  if(p+1>=l1_cases[0].inputs&&result!=l1_cases[0].output[p+1-l1_cases[0].inputs])return NV_ERR_INVALID_DATA;
 }
 for(unsigned round=0;round<3;round++)for(unsigned order=0;order<4;order++){
  unsigned mode=(order+round)%4,root=roots[mode],serial=0,completion=0;bool resident=mode>=2;
  WRITE_ONCE(q[0],0);WRITE_ONCE(q[1],0);WRITE_ONCE(q[32],3);WRITE_ONCE(q[33],0);WRITE_ONCE(q[34],1);WRITE_ONCE(q[35],2);wmb();
  if(resident)completion=submit(c,root,false);
  for(unsigned sample=0;sample<10;sample++){
   NvU64 begin=ktime_get_ns(),submitted,done,start,end,work,gap,fe,launch_ns=0;unsigned launch_valid=resident;
   WRITE_ONCE(q[2],l1_cases[0].output[6]);WRITE_ONCE(q[3],32);WRITE_ONCE(q[0],++serial);wmb();
   if(!resident)completion=submit(c,root,true);
   submitted=ktime_get_ns();s=wait_counter(resident?q+1:release_ptr(c),resident?serial:completion);if(s!=NV_OK)return s;done=ktime_get_ns();
   result=READ_ONCE(q[4]);if(READ_ONCE(q[5])||result!=l1_cases[0].output[7])return NV_ERR_INVALID_DATA;
   start=counter(q,12);end=counter(q,14);work=counter(q,42)-counter(q,40)+counter(q,46)-counter(q,44)+counter(q,50)-counter(q,48);
   if(end<start||end-start<work)return NV_ERR_INVALID_DATA;gap=end-start-work;fe=counter(q,1090);
   if(!resident&&fe&&start>=fe&&start-fe<done-begin){launch_ns=start-fe;launch_valid=1;}
   pr_info("L1_LAUNCH_SAMPLE round=%u mode=%s sample=%u position=32 output=%u expected=%u host_submit_ns=%llu total_ns=%llu gpu_span_ns=%llu gpu_work_ns=%llu gpu_gap_ns=%llu gpu_launch_ns=%llu gpu_launch_valid=%u hardware_launches=%u residual_ns=%lld\n",round,names[mode],sample,result,l1_cases[0].output[7],submitted-begin,done-begin,end-start,work,gap,launch_ns,launch_valid,!resident,(long long)(done-begin)-(long long)(end-start));
  }
  if(resident){WRITE_ONCE(q[0],0xffffffffU);wmb();s=wait_counter(release_ptr(c),completion);if(s!=NV_OK)return s;}
  pr_info("L1_LAUNCH_MODE_PASS round=%u mode=%s samples=10\n",round,names[mode]);
 }
 pr_info("L1_LAUNCH_BENCH_PASS modes=4 samples=120 position=32 identical_arithmetic=1 cpl=0 mm=none\n");return NV_OK;
}
#endif
NV_STATUS modelos_l1_launch(struct l1_launch_resources *r)
{
 struct executor *c=kvzalloc(struct_size(c,nodes,ARRAY_SIZE(graph_nodes)),GFP_KERNEL);NV_STATUS s;if(!c)return NV_ERR_NO_MEMORY;
 c->r=r;c->system=system_commands&&r->host.bytes>=0x100000;s=prepare(c);if(s!=NV_OK)goto out;
#if L1_LAUNCH_IS_QWEN
 s=upload_model(c);if(s==NV_OK)s=benchmark_mode?benchmark(c):qwen_graph(c);
#else
 s=smoke(c,0,3);if(s==NV_OK)pr_info("L1_GRAPH_PASS launches=3 nodes=3 words=96 cpl=0 mm=none no_libcuda=1\n");
#endif
out:kvfree(c);return s;
}
