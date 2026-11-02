// SPDX-License-Identifier: MIT
/* Typed graph executor and matched launch controls; GPU instruction bytes unchanged. */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sched.h>
#ifdef CONFIG_MODELOS_L1_TCP
#include <linux/modelos_gpu.h>
#endif
#include "nvtypes.h"
#include "nvstatus.h"
#include "launch.h"
struct l1_launch_node {const char *name;const unsigned char *code,*constant;unsigned code_bytes,constant_bytes,registers,shared_bytes;int next;NvU32 qmd[96];};
#include "l1-launch.h"
#ifndef L1_LAUNCH_MAX_BATCH
#define L1_LAUNCH_MAX_BATCH 1
#endif
#ifndef L1_LAUNCH_PREFILL_TILE
#define L1_LAUNCH_PREFILL_TILE 16
#endif
#ifndef L1_LAUNCH_FUSED_QKV
#define L1_LAUNCH_FUSED_QKV 0
#endif
#if L1_LAUNCH_IS_QWEN
#include "../modelos_serve/qwen.h"
#endif
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
  memcpy(cb,node->constant,node->constant_bytes);
  /* CUDA launch ABI: blockDim/gridDim are supplied in cbuf0 as well as QMD. */
  cb[0x360/4]=(node->qmd[34]&0xffff);cb[0x364/4]=node->qmd[34]>>16;cb[0x368/4]=node->qmd[35]&255;
  cb[0x370/4]=node->qmd[39];cb[0x374/4]=node->qmd[40]&0xffff;cb[0x378/4]=node->qmd[41]&0xffff;
  *(NvU64 *)((char *)cb+0x380)=c->r->host.gpu;
#if L1_LAUNCH_IS_QWEN
  *(NvU64 *)((char *)cb+0x388)=c->r->weights.gpu;*(NvU64 *)((char *)cb+0x390)=c->r->state.gpu;*(NvU64 *)((char *)cb+0x398)=c->r->state.gpu+(IS_ENABLED(CONFIG_MODELOS_L1_TCP)?0x1e000000:0x3c00000);
  if(!strcmp(node->name,"modelos_upload"))*(NvU64 *)((char *)cb+0x390)=c->r->staging.gpu;
#endif
  cb[188]=0;cb[189]=0x7294;cb[190]=0;cb[191]=0x7293;cb[223]=0xfffdc0;store(c,ca-command_gpu(c),cb,sizeof(cb));
#ifdef CONFIG_MODELOS_L1_TCP
  if(n>16)return NV_ERR_INVALID_ARGUMENT;
  /* Immutable parameter banks per arena avoid modifying a cached CB0. */
  for(unsigned arena=1;arena<MODELOS_GPU_ARENAS;arena++){
   *(NvU64 *)((char *)cb+0x390)=c->r->state.gpu+arena*MODELOS_GPU_ARENA_BYTES;
   *(NvU64 *)((char *)cb+0x398)=c->r->state.gpu+arena*MODELOS_GPU_ARENA_BYTES+0x1e000000;
   store(c,ca-command_gpu(c)+arena*n*0x400,cb,sizeof(cb));
  }
#endif
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
  if(!strcmp(graph_nodes[root].name,"launch_geometry"))pr_info("L1_GEOMETRY block_x=%u grid_x=%u block_y=%u grid_y=%u cta_arrivals=%u last_cta=%u\n",q[90],q[91],q[92],q[93],q[94],q[128+169]);
  pr_info("L1_GRAPH_LAUNCH_PASS launch=%u nodes=3 host_submissions=1 words=32 first=%u last=%u elapsed_ns=%llu fe_timestamp=%llu\n",launch,READ_ONCE(q[16]),READ_ONCE(q[47]),ktime_get_ns()-start,counter(q,1090));
 }
 return NV_OK;
}
#if L1_LAUNCH_IS_OPTIM
#include "l1-optim-oracle.h"
/* Ordered float-bit distance avoids using host floating point in the kernel. */
static unsigned ordered(NvU32 x){return x&0x80000000U?~x:x^0x80000000U;}
static NvU32 integer_float(int value){NvU32 sign=value<0?0x80000000U:0,m=value<0?-value:value;if(!m)return 0;unsigned bit=fls(m)-1;return sign|((127+bit)<<23)|((m<<(23-bit))&0x7fffff);}
static NV_STATUS optimizations(struct executor *c)
{
 NvU32 *q=queue(c);NV_STATUS s;
 for(unsigned pass=0;pass<2;pass++){
  static const unsigned pages[2][4]={{2,0,3,1},{1,3,0,2}};
  for(unsigned i=0;i<4;i++)WRITE_ONCE(q[64+i],pages[pass][i]);WRITE_ONCE(q[69],pass+1);WRITE_ONCE(q[70],7);
  wmb();for(unsigned node=0;node<(graph_nodes[0].next<0?5:1);node++){s=wait_counter(release_ptr(c),submit(c,node,true));pr_info("L1_OPTIM_NODE node=%u status=0x%x\n",node,s);if(s!=NV_OK)return s;}
  for(unsigned i=0;i<256;i++){int sum=0;for(int k=0;k<16;k++)sum+=((int)((i/16+k)%3)-1)*((int)((k+i%16)%5)-2);if(READ_ONCE(q[256+i])!=integer_float(sum*(pass+1)))return NV_ERR_INVALID_DATA;}
  unsigned max_ulp=0;
  for(unsigned i=0;i<32;i++){
   unsigned got=ordered(READ_ONCE(q[700+i])),want=ordered(optim_expected[i]),delta=got>want?got-want:want-got;
   if(delta>128||READ_ONCE(q[700+i])!=READ_ONCE(q[800+i]))return NV_ERR_INVALID_DATA;max_ulp=max(max_ulp,delta);
  }
  pr_info("L1_OPTIM_CHECK_PASS pass=%u tensor_words=256 flash_words=32 paged_words=32 max_ulp=%u kernel_block_table=%u,%u,%u,%u\n",pass,max_ulp,q[64],q[65],q[66],q[67]);
 }
 if(!c->r->peer)return NV_ERR_INVALID_ARGUMENT;
 struct executor *peer=kvzalloc(struct_size(peer,nodes,ARRAY_SIZE(graph_nodes)),GFP_KERNEL);if(!peer)return NV_ERR_NO_MEMORY;
 peer->r=c->r->peer;peer->system=true;s=prepare(peer);if(s!=NV_OK)goto done;
 for(unsigned repeat=0;repeat<3;repeat++){
  NvU32 a,b;WRITE_ONCE(queue(c)[70],11);WRITE_ONCE(queue(peer)[70],29);wmb();
  a=submit(c,4,false);b=submit(peer,4,false);s=wait_counter(release_ptr(c),a);if(s!=NV_OK)goto done;s=wait_counter(release_ptr(peer),b);if(s!=NV_OK)goto done;
  for(unsigned i=0;i<32;i++)if(queue(c)[128+i]!=11000+i||queue(peer)[128+i]!=29000+i){pr_err("L1_OPTIM_STREAM_BAD index=%u a=%u b=%u\n",i,queue(c)[128+i],queue(peer)[128+i]);s=NV_ERR_INVALID_DATA;goto done;}
  NvU64 a0=counter(queue(c),12),a1=counter(queue(c),14),b0=counter(queue(peer),12),b1=counter(queue(peer),14);
  pr_info("L1_OPTIM_STREAM_TIMES a0=%llu a1=%llu b0=%llu b1=%llu\n",a0,a1,b0,b1);
  if(min(a1,b1)<=max(a0,b0)){s=NV_ERR_INVALID_DATA;goto done;}
  pr_info("L1_OPTIM_STREAM_PASS repeat=%u channels=2 same_tsg=1 same_vas=1 start0=%llu end0=%llu start1=%llu end1=%llu overlap_ns=%llu isolated_words=64\n",repeat,a0,a1,b0,b1,min(a1,b1)-max(a0,b0));
 }
 pr_info("L1_OPTIM_PASS tensor=1 flash=1 paged=1 multistream=1 cpl=0 mm=none\n");
done:kvfree(peer);return s;
}
#endif
#if L1_LAUNCH_IS_QWEN
static NV_STATUS upload_model(struct executor *c)
{
 NvU64 upload_start=ktime_get_ns();void *source=memremap(c->r->weights_phys,c->r->model_bytes,MEMREMAP_WB);if(!source)return NV_ERR_NO_MEMORY;
 if(memcmp(source,"MOSQ3B1",7)||(*(NvU64 *)((char *)source+48)!=c->r->model_bytes&&!(IS_ENABLED(CONFIG_MODELOS_L1_TCP)&&*(NvU64 *)((char *)source+48)==1192235008ULL))){memunmap(source);return NV_ERR_INVALID_DATA;}
#if L1_LAUNCH_FUSED_QKV
 const struct q_header *header=source;
 for(unsigned layer=0;layer<Q_LAYERS;layer++){const struct q_layer *l=&header->layer[layer];if(l->wk!=l->wq+Q_QDIM*Q_DIM*2ULL||l->wv!=l->wk+Q_KVDIM*Q_DIM*2ULL||l->wu!=l->wg+Q_FF*Q_DIM*2ULL){memunmap(source);return NV_ERR_INVALID_DATA;}}
#endif
 int upload=-1;NvU64 gpu_copy_ns=0,submit_ns=0;unsigned submissions=0;
 for(unsigned i=0;i<ARRAY_SIZE(graph_nodes);i++)if(!strcmp(graph_nodes[i].name,"modelos_upload"))upload=i;
 if(upload>=0&&c->r->staging.bytes){
  if(c->r->weights.bytes<c->r->model_bytes+4||c->r->model_bytes>U32_MAX||c->r->model_bytes%16){memunmap(source);return NV_ERR_INVALID_ARGUMENT;}
  writel(0,(void __iomem *)c->r->weights.cpu+c->r->model_bytes);if(readl((void __iomem *)c->r->weights.cpu+c->r->model_bytes)){memunmap(source);return NV_ERR_INVALID_DATA;}
  for(NvU64 off=0;off<c->r->model_bytes;off+=c->r->staging.bytes){
   unsigned bytes=min_t(NvU64,c->r->staging.bytes,c->r->model_bytes-off);memcpy(c->r->staging.cpu,(char *)source+off,bytes);
   NvU32 *q=queue(c);WRITE_ONCE(q[2],off);WRITE_ONCE(q[3],bytes);WRITE_ONCE(q[6],c->r->model_bytes);wmb();NvU64 start=ktime_get_ns();NvU32 completion=submit(c,upload,true);submit_ns+=ktime_get_ns()-start;
   NV_STATUS status=wait_counter(release_ptr(c),completion);if(status!=NV_OK){memunmap(source);return status;}gpu_copy_ns+=counter(q,14)-counter(q,12);submissions++;cond_resched();
  }
 }else for(NvU64 off=0;off<c->r->model_bytes;off+=0x10000){memcpy_toio((void __iomem *)c->r->weights.cpu+off,(char *)source+off,min_t(NvU64,0x10000,c->r->model_bytes-off));cond_resched();}
 pr_info("L1_MODEL_COPY mode=%s bytes=%llu staging_bytes=%llu submissions=%u gpu_ns=%llu submit_ns=%llu\n",upload>=0&&c->r->staging.bytes?"gpu-dma":"cpu-bar",c->r->model_bytes,c->r->staging.bytes,submissions,gpu_copy_ns,submit_ns);
 if(IS_ENABLED(CONFIG_MODELOS_L1_TCP)){
  NvU32 sequence=2048;NvU64 total=c->r->model_bytes,cosine=1192235008ULL,sine=1192759296ULL;
  memcpy_toio((void __iomem *)c->r->weights.cpu+40,&sequence,4);memcpy_toio((void __iomem *)c->r->weights.cpu+48,&total,8);
  memcpy_toio((void __iomem *)c->r->weights.cpu+72,&cosine,8);memcpy_toio((void __iomem *)c->r->weights.cpu+80,&sine,8);
  wmb();if(readl((void __iomem *)c->r->weights.cpu+40)!=2048){memunmap(source);return NV_ERR_INVALID_DATA;}
 }
 wmb();memunmap(source);pr_info("L1_GRAPH_QWEN_WEIGHTS bytes=%llu gpu=0x%llx upload_ns=%llu extended_header=%u\n",c->r->model_bytes,c->r->weights.gpu,ktime_get_ns()-upload_start,IS_ENABLED(CONFIG_MODELOS_L1_TCP));return NV_OK;
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
#if L1_LAUNCH_IS_PARALLEL
#include "l1-perf-golden.h"
static NV_STATUS parallel_shape(struct executor *c)
{
 struct shape_sample {NvU32 got;NvU64 cpu,gpu;};struct shape_sample *samples=kvcalloc(128,sizeof(*samples),GFP_KERNEL);if(!samples)return NV_ERR_NO_MEMORY;
 NvU32 *q=queue(c);NV_STATUS status;
 for(unsigned mode=0;mode<2;mode++){
  NvU32 result=0,completion=0;NvU64 decode_ns=0,gpu_ns=0,begin,first=0,end;unsigned matched=0;
  WRITE_ONCE(q[0],0);WRITE_ONCE(q[1],0);memset_io((void __iomem *)c->r->state.cpu+0x3b10000,0,82*4);wmb();
  if(mode)completion=submit(c,1,false);begin=ktime_get_ns();
  for(unsigned p=0;p<255;p++){
   NvU64 start=ktime_get_ns();WRITE_ONCE(q[2],p<128?l1_perf_cases[0].input[p]:result);WRITE_ONCE(q[3],p);WRITE_ONCE(q[0],p+1);wmb();
   if(!mode)completion=submit(c,0,true);status=wait_counter(mode?q+1:release_ptr(c),mode?p+1:completion);if(status!=NV_OK)return status;end=ktime_get_ns();result=READ_ONCE(q[4]);
   if(p>=127){unsigned index=p-127;samples[index]=(struct shape_sample){result,end-start,counter(q,14)-counter(q,12)};
    if(READ_ONCE(q[5])||result!=l1_perf_cases[0].output[index])return NV_ERR_INVALID_DATA;++matched;
    if(!index)first=end;else{decode_ns+=end-start;gpu_ns+=counter(q,14)-counter(q,12);}
   }
  }
  end=ktime_get_ns();if(mode){WRITE_ONCE(q[0],0xffffffffU);wmb();status=wait_counter(release_ptr(c),completion);if(status!=NV_OK)return status;}
  for(unsigned i=0;i<128;i++)pr_info("L1_PARALLEL_SHAPE_TOKEN mode=%s index=%u got=%u expected=%u cpu_ns=%llu gpu_ns=%llu\n",mode?"persistent":"graph",i,samples[i].got,l1_perf_cases[0].output[i],samples[i].cpu,samples[i].gpu);
  pr_info("L1_PARALLEL_SHAPE_PASS mode=%s input=128 output=%u steps=255 prefill_token_by_token=1 ttft_ns=%llu decode_steps=127 decode_cpu_ns=%llu decode_gpu_ns=%llu request_ns=%llu\n",mode?"persistent":"graph",matched,first-begin,decode_ns,gpu_ns,end-begin);
 }
 kvfree(samples);return NV_OK;
}
static NV_STATUS parallel_qwen(struct executor *c)
{
 struct decode_sample {unsigned id,index,position,result;NvU64 cpu,submit,gpu;};struct decode_sample *samples=kvcalloc(320,sizeof(*samples),GFP_KERNEL);if(!samples)return NV_ERR_NO_MEMORY;
 NvU32 *q=queue(c);NV_STATUS s=NV_OK;
 for(unsigned mode=0;mode<2;mode++){
  unsigned matched=0,serial=0;NvU32 result=0,completion=0;NvU64 total_ns=0,gpu_ns=0;unsigned output_steps=0;
  WRITE_ONCE(q[0],0);WRITE_ONCE(q[1],0);memset_io((void __iomem *)c->r->state.cpu+0x3b10000,0,82*4);wmb();
  if(mode)completion=submit(c,1,false);
  for(unsigned id=0;id<ARRAY_SIZE(l1_cases);id++){
   for(unsigned p=0;p<l1_cases[id].inputs+l1_cases[id].outputs-1;p++){
    NvU64 begin=ktime_get_ns(),submitted,done;
    WRITE_ONCE(q[2],p<l1_cases[id].inputs?l1_cases[id].input[p]:result);WRITE_ONCE(q[3],p);WRITE_ONCE(q[0],++serial);wmb();
    if(!mode)completion=submit(c,0,true);submitted=ktime_get_ns();
    s=wait_counter(mode?q+1:release_ptr(c),mode?serial:completion);if(s!=NV_OK)return s;done=ktime_get_ns();result=READ_ONCE(q[4]);
    if(READ_ONCE(q[5]))return NV_ERR_INVALID_DATA;
    if(p+1>=l1_cases[id].inputs){unsigned index=p+1-l1_cases[id].inputs;
     samples[matched]=(struct decode_sample){id,index,p,result,done-begin,submitted-begin,counter(q,14)-counter(q,12)};
     if(result!=l1_cases[id].output[index])return NV_ERR_INVALID_DATA;++matched;
     /* Exclude the final prefill step that supplies the first output token. */
     if(index){total_ns+=done-begin;gpu_ns+=counter(q,14)-counter(q,12);++output_steps;}
    }
   }
  }
  if(mode){WRITE_ONCE(q[0],0xffffffffU);wmb();s=wait_counter(release_ptr(c),completion);if(s!=NV_OK)return s;}
  for(unsigned i=0;i<matched;i++){struct decode_sample *t=samples+i;pr_info("L1_PARALLEL_TOKEN mode=%s case=%u index=%u position=%u got=%u expected=%u cpu_ns=%llu submit_ns=%llu gpu_ns=%llu\n",mode?"persistent":"graph",t->id,t->index,t->position,t->result,l1_cases[t->id].output[t->index],t->cpu,t->submit,t->gpu);}
  pr_info("L1_PARALLEL_MODE_PASS mode=%s tokens=%u steps=%u decode_steps=%u decode_cpu_ns=%llu decode_gpu_ns=%llu\n",mode?"persistent":"graph",matched,serial,output_steps,total_ns,gpu_ns);
 }
 kvfree(samples);s=parallel_shape(c);if(s!=NV_OK)return s;
 pr_info("L1_PARALLEL_PASS modes=2 cases=5 tokens_per_mode=320 cpl=0 mm=none\n");return s;
}
#endif
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
#ifdef CONFIG_MODELOS_L1_TCP
/* One full-grid decoder per step; four CPU request workers interleave at the
 * step boundary. Each admitted request owns a separate VRAM KV arena. */
static DEFINE_MUTEX(service_lock);
static struct executor *service;
static struct l1_launch_resources service_resources;
static unsigned service_owner[MODELOS_GPU_ARENAS];
static u64 service_gpu[MODELOS_GPU_ARENAS],service_submit[MODELOS_GPU_ARENAS],service_wait[MODELOS_GPU_ARENAS];
static u64 service_prefill[MODELOS_GPU_ARENAS],service_decode[MODELOS_GPU_ARENAS],service_fault_start[MODELOS_GPU_ARENAS];
static unsigned service_steps[MODELOS_GPU_ARENAS];
struct service_account {u32 id,steps,input,output;u64 gpu,submit,queue,ready_wait,send,alloc,free,compute,prefill,decode,gpu_prefill,gpu_decode,faults;};
static struct service_account accounts[1024];static unsigned account_count;
void modelos_gpu_domain_account(u32 id,u32 input,u32 output,u64 wait,u64 send,u64 alloc,u64 free,u64 compute,u64 prefill,u64 decode){
 mutex_lock(&service_lock);for(unsigned i=account_count;i>0;i--)if(accounts[i-1].id==id){struct service_account *a=&accounts[i-1];a->input=input;a->output=output;a->ready_wait=wait;a->send=send;a->alloc=alloc;a->free=free;a->compute=compute;a->prefill=prefill;a->decode=decode;break;}mutex_unlock(&service_lock);
}
bool modelos_l1_serving(void){return smp_load_acquire(&service)!=NULL;}
int modelos_gpu_prepare(void){return modelos_l1_serving()&&!current->mm?0:-ENODEV;}
int modelos_gpu_arena_acquire(unsigned arena,u32 request){
 if(arena>=MODELOS_GPU_ARENAS||!request||!modelos_l1_serving())return -EINVAL;
 mutex_lock(&service_lock);if(service_owner[arena]){mutex_unlock(&service_lock);return -EBUSY;}
 service_owner[arena]=request;service_gpu[arena]=service_submit[arena]=service_wait[arena]=0;service_steps[arena]=0;service_prefill[arena]=service_decode[arena]=0;service_fault_start[arena]=modelos_kernel_faults();
 memset_io((void __iomem *)service->r->state.cpu+arena*MODELOS_GPU_ARENA_BYTES+0x1d100000,0,82*4);wmb();
 for(unsigned j=0;j<16;j++){writel(0x51a70000U+arena*256+j,(void __iomem *)service->r->state.cpu+arena*MODELOS_GPU_ARENA_BYTES+0x1d000000+j*4);writel(0x93c80000U+arena*256+j,(void __iomem *)service->r->state.cpu+(arena+1)*MODELOS_GPU_ARENA_BYTES-0x1000+j*4);}wmb();
 mutex_unlock(&service_lock);return 0;
}
int modelos_gpu_arena_release(unsigned arena,u32 request){
 if(arena>=MODELOS_GPU_ARENAS)return -EINVAL;
 mutex_lock(&service_lock);if(service_owner[arena]!=request){mutex_unlock(&service_lock);return -EINVAL;}
 for(unsigned j=0;j<16;j++)if(readl((void __iomem *)service->r->state.cpu+arena*MODELOS_GPU_ARENA_BYTES+0x1d000000+j*4)!=0x51a70000U+arena*256+j||readl((void __iomem *)service->r->state.cpu+(arena+1)*MODELOS_GPU_ARENA_BYTES-0x1000+j*4)!=0x93c80000U+arena*256+j){mutex_unlock(&service_lock);return -EIO;}
 service_owner[arena]=0;
 if(account_count>=ARRAY_SIZE(accounts)){mutex_unlock(&service_lock);return -ENOSPC;}
 accounts[account_count++]=(struct service_account){.id=request,.steps=service_steps[arena],.gpu=service_gpu[arena],.submit=service_submit[arena],.queue=service_wait[arena],.gpu_prefill=service_prefill[arena],.gpu_decode=service_decode[arena],.faults=modelos_kernel_faults()-service_fault_start[arena]};
 mutex_unlock(&service_lock);return 0;
}
static void bind_arena(struct executor *c,unsigned arena){
 for(unsigned node=0;node<ARRAY_SIZE(graph_nodes);node++){
  NvU64 address=cb_address(c,node)+arena*ARRAY_SIZE(graph_nodes)*0x400;
  field(c->nodes[node].qmd,1375,1344,(NvU32)(address>>6));field(c->nodes[node].qmd,1394,1376,address>>38);
 }
}
#if L1_LAUNCH_MAX_BATCH > 1
struct decode_job {struct list_head link;struct completion done;unsigned worker,arena;u32 request,token,position,result;int status;u64 enqueued;};
static LIST_HEAD(decode_jobs);static DEFINE_SPINLOCK(decode_lock);
static DECLARE_WAIT_QUEUE_HEAD(decode_wait);static atomic_t decode_leader=ATOMIC_INIT(0);
static u64 batch_histogram[5];
static void dispatch_decode(void){
 struct decode_job *jobs[4];unsigned n=0;unsigned long flags;NV_STATUS status=NV_OK;
 mutex_lock(&service_lock);
 spin_lock_irqsave(&decode_lock,flags);
 while(n<4&&!list_empty(&decode_jobs)){jobs[n]=list_first_entry(&decode_jobs,struct decode_job,link);list_del(&jobs[n]->link);n++;}
 spin_unlock_irqrestore(&decode_lock,flags);
 if(!n){mutex_unlock(&service_lock);return;}
 u64 locked=ktime_get_ns(),submitted,gpu;
 for(unsigned i=0;i<n;i++)if(!service||service_owner[jobs[i]->arena]!=jobs[i]->request)status=NV_ERR_INVALID_STATE;
 if(status!=NV_OK)goto complete;
 NvU32 *q=queue(service);bind_arena(service,n==1?jobs[0]->arena:0);
 WRITE_ONCE(q[7],n==1?0:n);WRITE_ONCE(q[9],n==1?0:2);
 if(n==1){WRITE_ONCE(q[2],jobs[0]->token);WRITE_ONCE(q[3],jobs[0]->position);}
 else for(unsigned i=0;i<n;i++){WRITE_ONCE(q[2048+i*4],jobs[i]->token);WRITE_ONCE(q[2048+i*4+1],jobs[i]->position);WRITE_ONCE(q[2048+i*4+2],jobs[i]->arena);WRITE_ONCE(q[2048+i*4+3],jobs[i]->request);}
 WRITE_ONCE(q[0],service->completion+1);wmb();NvU32 completion=submit(service,0,true);submitted=ktime_get_ns();status=wait_counter(release_ptr(service),completion);
 if(status==NV_OK&&READ_ONCE(q[5]))status=NV_ERR_INVALID_DATA;
 if(status==NV_OK){
  gpu=counter(q,14)-counter(q,12);batch_histogram[n]++;
  for(unsigned i=0;i<n;i++){
   if(n>1&&READ_ONCE(q[68+i])!=jobs[i]->request){status=NV_ERR_INVALID_DATA;break;}
   jobs[i]->result=READ_ONCE(q[n==1?4:64+i]);unsigned arena=jobs[i]->arena;
   /* Charge shared work once in total; per-request wall time stays in core. */
   u64 share=gpu/n+(i<gpu%n);service_gpu[arena]+=share;service_decode[arena]+=share;service_submit[arena]+=(submitted-locked)/n;service_wait[arena]+=locked-jobs[i]->enqueued;service_steps[arena]++;
  }
 }
complete:
 for(unsigned i=0;i<n;i++)jobs[i]->status=status==NV_OK?0:-EIO;
 mutex_unlock(&service_lock);
 /* Do not access a job after completion: the waiting worker owns its stack. */
 for(unsigned i=0;i<n;i++)complete(&jobs[i]->done);
}
int modelos_gpu_step(unsigned worker,unsigned arena,u32 request,u32 token,u32 position,u32 *result){
 if(worker>=4||arena>=MODELOS_GPU_ARENAS||READ_ONCE(service_owner[arena])!=request)return -EINVAL;
 struct decode_job own={.worker=worker,.arena=arena,.request=request,.token=token,.position=position,.enqueued=ktime_get_ns()};unsigned long flags;init_completion(&own.done);
 spin_lock_irqsave(&decode_lock,flags);list_add_tail(&own.link,&decode_jobs);spin_unlock_irqrestore(&decode_lock,flags);wake_up_all(&decode_wait);
 while(!completion_done(&own.done)){
  if(atomic_cmpxchg(&decode_leader,0,1)==0){
   unsigned active=0;for(unsigned a=0;a<MODELOS_GPU_ARENAS;a++)active+=READ_ONCE(service_owner[a])!=0;
   if(active>1)udelay(20);dispatch_decode();atomic_set(&decode_leader,0);wake_up_all(&decode_wait);
  }else wait_event(decode_wait,completion_done(&own.done)||!atomic_read(&decode_leader));
 }
 *result=own.result;return own.status;
}
#else
int modelos_gpu_step(unsigned worker,unsigned arena,u32 request,u32 token,u32 position,u32 *result){
 u64 begin=ktime_get_ns(),locked,submitted;NV_STATUS status;NvU32 completion;
 if(worker>=4||arena>=MODELOS_GPU_ARENAS)return -EINVAL;
 mutex_lock(&service_lock);locked=ktime_get_ns();if(!service||service_owner[arena]!=request){mutex_unlock(&service_lock);return -EINVAL;}
 bind_arena(service,arena);
 NvU32 *q=queue(service);WRITE_ONCE(q[9],0);WRITE_ONCE(q[7],0);WRITE_ONCE(q[2],token);WRITE_ONCE(q[3],position);WRITE_ONCE(q[0],service->completion+1);wmb();
 completion=submit(service,0,true);submitted=ktime_get_ns();status=wait_counter(release_ptr(service),completion);
 if(status==NV_OK&&READ_ONCE(q[5]))status=NV_ERR_INVALID_DATA;
 if(status==NV_OK){*result=READ_ONCE(q[4]);service_gpu[arena]+=counter(q,14)-counter(q,12);service_decode[arena]+=counter(q,14)-counter(q,12);service_submit[arena]+=submitted-locked;service_wait[arena]+=locked-begin;service_steps[arena]++;}
 mutex_unlock(&service_lock);return status==NV_OK?0:-EIO;
}
#endif
int modelos_gpu_prefill(unsigned worker,unsigned arena,u32 request,const u32 *tokens,unsigned count,u32 *result){
 if(worker>=4||arena>=MODELOS_GPU_ARENAS||!count||count>1024)return -EINVAL;
 for(unsigned base=0;base<count;base+=L1_LAUNCH_PREFILL_TILE){
  u64 begin=ktime_get_ns(),locked,submitted;unsigned n=min((unsigned)L1_LAUNCH_PREFILL_TILE,count-base);NV_STATUS status;NvU32 completion;
  mutex_lock(&service_lock);locked=ktime_get_ns();if(!service||service_owner[arena]!=request){mutex_unlock(&service_lock);return -EINVAL;}
  bind_arena(service,arena);
  NvU32 *q=queue(service);WRITE_ONCE(q[9],0);for(unsigned i=0;i<n;i++)WRITE_ONCE(q[2048+i],tokens[base+i]);WRITE_ONCE(q[3],base);WRITE_ONCE(q[7],n);WRITE_ONCE(q[8],base+n==count);WRITE_ONCE(q[0],service->completion+1);wmb();
  completion=submit(service,0,true);submitted=ktime_get_ns();status=wait_counter(release_ptr(service),completion);
  if(status==NV_OK&&READ_ONCE(q[5]))status=NV_ERR_INVALID_DATA;
  if(status==NV_OK){*result=READ_ONCE(q[4]);service_gpu[arena]+=counter(q,14)-counter(q,12);service_prefill[arena]+=counter(q,14)-counter(q,12);service_submit[arena]+=submitted-locked;service_wait[arena]+=locked-begin;service_steps[arena]++;}
  mutex_unlock(&service_lock);if(status!=NV_OK)return -EIO;cond_resched();
 }return 0;
}
int modelos_gpu_stop(void){
 mutex_lock(&service_lock);if(!service){mutex_unlock(&service_lock);return -ENODEV;}
 for(unsigned i=0;i<MODELOS_GPU_ARENAS;i++)if(service_owner[i]){mutex_unlock(&service_lock);return -EBUSY;}
 struct executor *c=service;smp_store_release(&service,NULL);kvfree(c);mutex_unlock(&service_lock);
 for(unsigned i=0;i<account_count;i++){struct service_account *a=&accounts[i];pr_info("L1_TCP_ACCOUNT id=%u input=%u output=%u steps=%u gpu_ns=%llu submit_ns=%llu submit_queue_wait_ns=%llu scheduler_wait_ns=%llu send_ns=%llu alloc_ns=%llu release_ns=%llu compute_ns=%llu prefill_ns=%llu decode_ns=%llu gpu_prefill_ns=%llu gpu_decode_ns=%llu guest_pf_entries=%llu canaries=32\n",a->id,a->input,a->output,a->steps,a->gpu,a->submit,a->queue,a->ready_wait,a->send,a->alloc,a->free,a->compute,a->prefill,a->decode,a->gpu_prefill,a->gpu_decode,a->faults);}
#if L1_LAUNCH_MAX_BATCH > 1
 pr_info("L1_GPU_BATCH_HIST size1=%llu size2=%llu size3=%llu size4=%llu coalesce_us=20 shared_work_charged_once=1\n",batch_histogram[1],batch_histogram[2],batch_histogram[3],batch_histogram[4]);
#endif
 pr_info("L1_GPU_TCP_DRAIN release=1 owners=0 graph=1 no_libcuda=1\n");return modelos_l1_release_held();
}
#endif
NV_STATUS modelos_l1_launch(struct l1_launch_resources *r)
{
 struct executor *c=kvzalloc(struct_size(c,nodes,ARRAY_SIZE(graph_nodes)),GFP_KERNEL);NV_STATUS s;if(!c)return NV_ERR_NO_MEMORY;
 #ifdef CONFIG_MODELOS_L1_TCP
 service_resources=*r;c->r=&service_resources;
#else
 c->r=r;
#endif
 c->system=system_commands&&r->host.bytes>=0x100000;s=prepare(c);if(s!=NV_OK)goto out;
#if L1_LAUNCH_IS_QWEN
 s=upload_model(c);
#ifdef CONFIG_MODELOS_L1_TCP
 if(s==NV_OK){smp_store_release(&service,c);pr_info("L1_GPU_TCP_GRAPH_READY nodes=%zu ctas=170 arenas=8 no_libcuda=1 cpl=0 mm=none\n",ARRAY_SIZE(graph_nodes));return NV_OK;}
#endif
#if L1_LAUNCH_IS_PARALLEL
 if(s==NV_OK)s=parallel_qwen(c);
#else
 if(s==NV_OK)s=benchmark_mode?benchmark(c):qwen_graph(c);
#endif
#elif L1_LAUNCH_IS_OPTIM
 s=optimizations(c);
#else
 s=smoke(c,0,3);if(s==NV_OK)pr_info("L1_GRAPH_PASS launches=3 nodes=3 words=96 cpl=0 mm=none no_libcuda=1\n");
#endif
out:kvfree(c);return s;
}
