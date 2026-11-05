// SPDX-License-Identifier: GPL-2.0
/* CPU qualification adapter: production HTTP/tokenizer, checked fixture engine.
 * The net lane smoltcp and polled NIC carry real TCP. No GPU computation. */
#include "role.h"
#include <linux/string.h>
#include "bridge.h"
#include "http-service.h"
#include "image.h"
#include "net.h"
void *mos_text_reserved_memory(unsigned int table);
void __attribute__((noreturn)) mos_text_kernel_trap(unsigned int code);
void *mos_text_reserved_memory(unsigned int table){return role_va(table?role_table_phys:role_linear_phys);}
void __attribute__((noreturn)) mos_text_kernel_trap(unsigned int code){role_puts("ROLE_TEXT_TRAP ");role_hex(code);role_puts("\n");role_halt();}
static int entropy(unsigned char *p,size_t n)
{
 while(n--){unsigned tries=100;u64 v;unsigned char ok;do{asm volatile("rdrand %0; setc %1":"=r"(v),"=qm"(ok));}while(!ok&&--tries);if(!ok)return -1;*p++=v;}
 return 0;
}
static u64 parked_tsc,request_cpu;
static bool profiling;
static unsigned long long clock_ns(void){u64 t=role_tsc()-parked_tsc;return t/role_tsc_hz*1000000000+(t%role_tsc_hz)*1000000000/role_tsc_hz;}
static void poll_network(void){if(mos_net_poll(role_now_ns()))mos_net_panic();}
static void wait_network(void)
{
 u64 now=role_now_ns();parked_tsc+=role_net_wait(mos_net_next_poll_ns(now));
}
static int stream_read(void *socket,void *data,size_t bytes)
{
 unsigned index=(u64)socket-1;u64 deadline=role_now_ns()+30000000000UL;
 for(;;){
  int n=mos_net_read(index,data,bytes);if(n!=-11)return n;
  poll_network();n=mos_net_read(index,data,bytes);if(n!=-11)return n;
  if(role_now_ns()>deadline)return -110;
  wait_network();
 }
}
static int stream_write(void *socket,const void *data,size_t bytes)
{
 unsigned index=(u64)socket-1;u64 deadline=role_now_ns()+30000000000UL;
 for(;;){
  int n=mos_net_write(index,data,bytes);if(n>=0){poll_network();return n;}if(n!=-11)return n;
  poll_network();n=mos_net_write(index,data,bytes);if(n>=0){poll_network();return n;}if(n!=-11)return n;
  if(role_now_ns()>deadline)return -110;
  wait_network();
 }
}
#define MAGIC 0x31534f4dU
struct fixture_request {u32 id,limit;};
static struct {unsigned handshake,count,request,token;struct fixture_request requests[64];} fixture;
static void *fixture_connect(unsigned short port){memset(&fixture,0,sizeof(fixture));return &fixture;}
static void fixture_close(void *unused){memset(&fixture,0,sizeof(fixture));}
static int fixture_write(const void *data,size_t bytes)
{
 const u32 *w=data;unsigned at=4,i,j;
 if(bytes<16||w[0]!=MAGIC)return -1;
 if(w[1]==5&&bytes==16){fixture.handshake=1;return bytes;}
 if(w[1]!=9||w[2]!=bytes-16||!w[3]||w[3]>64)return -1;
 fixture.count=w[3];fixture.request=fixture.token=0;
 for(i=0;i<fixture.count;i++){
  if(at+3>bytes/4)return -1;
  u32 id=w[at++],count=w[at++],limit=w[at++];
  if(count!=role_fixture_inputs||at+count>bytes/4||limit>role_fixture_outputs)return -1;
  for(j=0;j<count;j++)if(w[at+j]!=role_fixture_input[j])return -1;
  at+=count;fixture.requests[i]=(struct fixture_request){id,limit};
 }
 if(at!=bytes/4)return -1;
 role_puts("ROLE_FIXTURE_INPUT_PASS\n");return bytes;
}
static int fixture_read(void *data,size_t bytes)
{
 u32 *w=data;if(bytes!=16)return -1;w[0]=MAGIC;
 if(fixture.handshake){w[1]=17;w[2]=w[3]=0;fixture.handshake=0;return 16;}
 if(fixture.request>=fixture.count)return -1;
 struct fixture_request *r=&fixture.requests[fixture.request];w[2]=r->id;
 if(fixture.token<r->limit){w[1]=16;w[3]=role_fixture_output[fixture.token++];}
 else{w[1]=17;w[3]=0;fixture.token=0;fixture.request++;}
 return 16;
}
static int transport_read(void *socket,void *data,size_t bytes){return socket==&fixture?fixture_read(data,bytes):stream_read(socket,data,bytes);}
static int transport_write(void *socket,const void *data,size_t bytes){return socket==&fixture?fixture_write(data,bytes):stream_write(socket,data,bytes);}
static void profile(unsigned op,unsigned long long out[6])
{
 if(op==1){profiling=true;request_cpu=0;}else if(op==0)profiling=false;
 mos_text_profile(op,out);out[4]=request_cpu;out[5]=0; /* Protocol transport accounting is separate. */
}
static void complete(unsigned long long begin){if(profiling)request_cpu+=clock_ns()-begin;role_step_boundary();}
static const struct mos_http_ops operations={NULL,NULL,transport_read,transport_write,fixture_connect,fixture_close,mos_text_http_open,mos_text_http_event,mos_text_http_drop,profile,clock_ns,complete};
void __attribute__((noreturn)) role_service(void)
{
 u32 a=1,b,c,d;asm volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
 if(!(c&(1U<<24))){role_puts("ROLE_ERROR TSC deadline required\n");role_halt();}
 if(!(c&(1U<<30))){role_puts("ROLE_ERROR RDRAND required\n");role_halt();}
 int status=role_image_load();
 if(status){role_puts("ROLE_IMAGE_ERROR ");role_hex(status);role_puts("\n");role_halt();}

 mos_text_set_entropy(entropy);mos_text_set_clock(clock_ns);
 status=mos_text_init(role_va(role_tokenizer_phys),role_tokenizer_bytes,128);
 if(status){role_puts("ROLE_TEXT_INIT_ERROR ");role_hex(status);role_halt();}
 struct mos_net_config cfg={.ipv4={10,0,7,15},.gateway={10,0,7,2},.prefix_len=24,.port=8000};
 if(entropy((u8 *)&cfg.random_seed,sizeof(cfg.random_seed))||mos_nic_init(cfg.mac)||mos_net_init(&cfg,role_now_ns()))mos_net_panic();
 role_puts("ROLE_HTTP_READY transport=TCP smoltcp=0.12 tokenizer=0.23.2 engine=checked-fixture memory=objects\n");
 for(;;){
  poll_network();int index=mos_net_accept();
  if(index==-11){wait_network();continue;}if(index<0)mos_net_panic();
  int ret=mos_http_serve((void *)(u64)(index+1),&operations);
  if(ret){mos_net_abort(index);role_puts("ROLE_HTTP_REQUEST_ERROR\n");}else mos_net_close(index);
 }
}
