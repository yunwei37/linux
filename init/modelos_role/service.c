// SPDX-License-Identifier: GPL-2.0
/* CPU qualification adapter: production HTTP/tokenizer, checked fixture engine.
 * COM2 carries HTTP bytes. There is no guest TCP stack or GPU computation. */
#include "role.h"
#include <linux/string.h>
#include "bridge.h"
#include "http-service.h"
#include "image.h"
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
static void serial_init(void)
{
 role_out(0x2f9,0);role_out(0x2fb,0x80);role_out(0x2f8,1);role_out(0x2f9,0);role_out(0x2fb,3);role_out(0x2fa,0xc7);role_out(0x2fc,0x0b);
 /* Remap the legacy PIC, unmask only COM2 IRQ3; IRQ0 stays masked. */
 role_out(0x20,0x11);role_out(0xa0,0x11);role_out(0x21,0x20);role_out(0xa1,0x28);role_out(0x21,4);role_out(0xa1,2);role_out(0x21,1);role_out(0xa1,1);role_out(0x21,0xf7);role_out(0xa1,0xff);
}
static int serial_read(void *unused,void *data,size_t bytes)
{
 u8 *p=data;size_t n=0;if(!bytes)return 0;
 for(;;){
  asm volatile("cli":::"memory");
  while(n<bytes&&(role_in(0x2fd)&1))p[n++]=role_in(0x2f8);
  if(n){role_out(0x2f9,0);return n;}
  role_out(0x2f9,1);
  /* STI shadows interrupts through HLT: no poll-to-sleep lost wakeup. */
  u64 parked_begin=role_tsc();
  asm volatile("sti; hlt":::"memory");
  parked_tsc+=role_tsc()-parked_begin;
 }
}
static int serial_write(void *unused,const void *data,size_t bytes)
{
 const u8 *p=data;size_t n;for(n=0;n<bytes;n++){while(!(role_in(0x2fd)&32))role_pause();role_out(0x2f8,p[n]);}return bytes;
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
static int transport_read(void *socket,void *data,size_t bytes){return socket==&fixture?fixture_read(data,bytes):serial_read(socket,data,bytes);}
static int transport_write(void *socket,const void *data,size_t bytes){return socket==&fixture?fixture_write(data,bytes):serial_write(socket,data,bytes);}
static void profile(unsigned op,unsigned long long out[6])
{
 if(op==1){profiling=true;request_cpu=0;}else if(op==0)profiling=false;
 mos_text_profile(op,out);out[4]=request_cpu;out[5]=0; /* COM2 has no TCP CPU time. */
}
static void complete(unsigned long long begin){if(profiling)request_cpu+=clock_ns()-begin;role_step_boundary();}
static const struct mos_http_ops operations={NULL,NULL,transport_read,transport_write,fixture_connect,fixture_close,mos_text_http_open,mos_text_http_event,mos_text_http_drop,profile,clock_ns,complete};
void __attribute__((noreturn)) role_service(void)
{
 u32 a=1,b,c,d;asm volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
 if(!(c&(1U<<30))){role_puts("ROLE_ERROR RDRAND required\n");role_halt();}
 int status=role_image_load();
 if(status){role_puts("ROLE_IMAGE_ERROR ");role_hex(status);role_puts("\n");role_halt();}
 serial_init();
 mos_text_set_entropy(entropy);mos_text_set_clock(clock_ns);
 status=mos_text_init(role_va(role_tokenizer_phys),role_tokenizer_bytes,128);
 if(status){role_puts("ROLE_TEXT_INIT_ERROR ");role_hex(status);role_halt();}
 role_puts("ROLE_HTTP_READY transport=COM2 tokenizer=0.23.2 engine=checked-fixture memory=static\n");
 for(;;){int ret=mos_http_serve((void *)1,&operations);if(ret)role_puts("ROLE_HTTP_REQUEST_ERROR\n");}
}
