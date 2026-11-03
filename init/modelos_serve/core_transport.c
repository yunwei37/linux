// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) "modelos-core: " fmt
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/mm_types.h>
#include <linux/mutex.h>
#include <linux/net.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <linux/modelos_gpu.h>
#include "core.h"

#define MAGIC 0x31534f4d
#define MAX_PROMPT 1024
struct wire_header {__le32 magic,op,id,arg;};
/* Transport state only: no weights, KV, cursor, engine or scheduling state. */
struct connection {
 struct socket *sock;struct completion finished;struct mos_handle handle;
 u64 session;u32 id;bool persistent;
};
static DEFINE_MUTEX(core_lock);
static DECLARE_WAIT_QUEUE_HEAD(work_wait);
static struct connection *connections[8];
static atomic_t sockets=ATOMIC_INIT(0),pending=ATOMIC_INIT(0);
static u64 next_session;
static bool rr;
static int __init core_policy(char *s) {rr=!strcmp(s,"round-robin");return 1;}
__setup("modelos_core_policy=",core_policy);

/* Instance-private AlpinDale binding: only called by the Rust Engine. */
static u32 prompts[8][MAX_PROMPT];
int mos_engine_load(void) {return modelos_gpu_prepare();}
int mos_engine_reserve(const struct mos_work *w,const u32 *prompt) {
 int ret=modelos_gpu_arena_acquire(w->handle.slot,w->request);
 if(!ret)memcpy(prompts[w->handle.slot],prompt,w->count*sizeof(u32));
 return ret;
}
int mos_engine_batch(u32 phase,const struct mos_work *w,u32 *out) {
 unsigned n=phase?0:min(64U,w->count-w->position);
 return modelos_gpu_run_phase(w->handle.slot,w->request,w->position,w->token,
     phase?NULL:prompts[w->handle.slot]+w->position,n,!phase&&w->position+n==w->count,out);
}
int mos_engine_release(const struct mos_work *w) {return modelos_gpu_arena_release(w->handle.slot,w->request);}
int mos_engine_unload(void) {return modelos_gpu_stop();}
void __noreturn mos_panic(void) {panic("model object core invariant");}

static int send_bytes(struct socket *sock,const void *data,size_t bytes) {
 while(bytes){struct msghdr msg={.msg_flags=MSG_NOSIGNAL};struct kvec vec={.iov_base=(void *)data,.iov_len=bytes};
  int n=kernel_sendmsg(sock,&msg,&vec,1,bytes);if(n<=0)return n?n:-EPIPE;data=(const u8 *)data+n;bytes-=n;}return 0;
}
static int recv_bytes(struct socket *sock,void *data,size_t bytes) {
 while(bytes){struct msghdr msg={};struct kvec vec={.iov_base=data,.iov_len=bytes};
  int n=kernel_recvmsg(sock,&msg,&vec,1,bytes,0);if(n<=0)return n?n:-ECONNRESET;data=(u8 *)data+n;bytes-=n;}return 0;
}
static int frame(struct socket *sock,u32 op,u32 id,u32 arg) {
 struct wire_header h={cpu_to_le32(MAGIC),cpu_to_le32(op),cpu_to_le32(id),cpu_to_le32(arg)};
 return send_bytes(sock,&h,sizeof(h));
}
static u64 user_tasks(void) {
 struct task_struct *g,*t;u64 n=0;rcu_read_lock();for_each_process_thread(g,t)if(READ_ONCE(t->mm))n++;rcu_read_unlock();return n;
}
int modelos_serve_prepare(void) {
 int ret;mutex_lock(&core_lock);ret=mos_init(rr);mutex_unlock(&core_lock);
 if(ret||user_tasks())return -ENODEV;
 pr_info("MODEL_OBJECTS ready=1 rust=1 image=alpin-hybrid max_batch=1 policy=%s\n",rr?"round-robin":"deadline");
 pr_info("SERVE_USER_ENTRY compiled_out=1 no_initramfs=1\n");return 0;
}
static void send_stats(struct socket *sock,u32 id) {
 struct mos_accounting a;__le64 data[10];
 mutex_lock(&core_lock);mos_stats(&a);mutex_unlock(&core_lock);
 data[0]=cpu_to_le64(a.live);data[1]=cpu_to_le64(a.live*MODELOS_GPU_ARENA_BYTES);
 data[2]=cpu_to_le64(a.admitted);data[3]=cpu_to_le64(a.released);data[4]=cpu_to_le64(a.peak);data[5]=cpu_to_le64(user_tasks());
 data[6]=cpu_to_le64(a.steps);data[7]=data[8]=data[9]=0;
 if(!frame(sock,18,id,a.live))send_bytes(sock,data,sizeof(data));
}
static void finished(struct connection *c,u32 reason) {
 struct socket *sock=c->sock;bool persistent=c->persistent;
 /* sendmsg copies these small frames; it does not retain a Request arena. */
 frame(sock,17,c->id,reason);
 mutex_lock(&core_lock);
 if(mos_ack(c->session,c->handle,ktime_get_ns()))pr_err("Request quarantine slot=%u\n",c->handle.slot);
 connections[c->handle.slot]=NULL;atomic_dec(&pending);
 mutex_unlock(&core_lock);
 if(persistent){struct sock *sk=sock->sk;sock_hold(sk);complete(&c->finished);sk->sk_data_ready(sk);sock_put(sk);}
 else {kernel_sock_shutdown(sock,SHUT_RD);complete(&c->finished);}
}
int modelos_serve_worker(unsigned int cpu) {
 unsigned short cs;asm volatile("mov %%cs,%0":"=r"(cs));
 if((cs&3)||current->mm||current->active_mm!=&init_mm||cpu>=4)return -EINVAL;
 pr_info("SERVE_WORKER cpu=%u cpl=%u mm=none object_owner=%u\n",cpu,cs&3,cpu==0);
 while(!kthread_should_stop()) {
  struct mos_event e;struct connection *c;int ret;
  wait_event_interruptible(work_wait,(cpu==0&&atomic_read(&pending))||kthread_should_stop());
  if(cpu||kthread_should_stop())continue;
  mutex_lock(&core_lock);ret=mos_advance(&e);
  c=ret==1?connections[e.handle.slot]:NULL;mutex_unlock(&core_lock);
  if(ret<0){pr_err("Instance failed; arenas quarantined\n");
   for(unsigned i=0;i<8;i++){mutex_lock(&core_lock);c=connections[i];connections[i]=NULL;mutex_unlock(&core_lock);
    if(c){frame(c->sock,19,c->id,EIO);frame(c->sock,17,c->id,2);kernel_sock_shutdown(c->sock,SHUT_RD);complete(&c->finished);}}
   atomic_set(&pending,0);continue;}
  if(!c){cond_resched();continue;}
  if(e.cancelled){finished(c,1);continue;}
  ret=frame(c->sock,16,c->id,e.token);
  mutex_lock(&core_lock);
  if(ret)mos_cancel(c->session,c->handle);else ret=mos_ack(c->session,c->handle,ktime_get_ns());
  mutex_unlock(&core_lock);
  if(!ret&&e.terminal)finished(c,0);
  cond_resched();
 }
 return 0;
}
static int connection_main(void *arg) {
 struct socket *sock=arg;struct connection *c=NULL;struct wire_header h;__le32 budget;
 u32 *prompt=NULL,id,count;bool persistent=false,lost=false;size_t buffered=0;int error=EINVAL;
next:
 c=NULL;prompt=NULL;lost=false;error=EINVAL;
 if(buffered<sizeof(h)&&recv_bytes(sock,(char *)&h+buffered,sizeof(h)-buffered))goto out;
 buffered=0;if(le32_to_cpu(h.magic)!=MAGIC)goto out;id=le32_to_cpu(h.id);
 if(le32_to_cpu(h.op)==5){if(persistent||id||le32_to_cpu(h.arg))goto bad;
  persistent=true;if(frame(sock,17,0,0))goto out;goto next;}
 if(le32_to_cpu(h.op)==3){send_stats(sock,id);goto out;}
 if(le32_to_cpu(h.op)==4){mutex_lock(&core_lock);error=mos_stop();mutex_unlock(&core_lock);
  frame(sock,error?19:17,id,error?-error:0);goto out;}
 count=le32_to_cpu(h.arg);
 if(le32_to_cpu(h.op)!=1||!id||!count||count>MAX_PROMPT||recv_bytes(sock,&budget,4))goto bad;
 c=kzalloc(sizeof(*c),GFP_KERNEL);prompt=kmalloc_array(count,sizeof(u32),GFP_KERNEL);
 if(!c||!prompt){error=ENOMEM;goto bad;}
 if(recv_bytes(sock,prompt,count*4))goto bad;
 for(unsigned i=0;i<count;i++)prompt[i]=le32_to_cpu((__force __le32)prompt[i]);
 c->sock=sock;c->id=id;c->persistent=persistent;init_completion(&c->finished);
 mutex_lock(&core_lock);
 if(next_session==U64_MAX){error=-EOVERFLOW;}else{
  c->session=++next_session;
  error=mos_admit(c->session,id,prompt,count,le32_to_cpu(budget),ktime_get_ns(),&c->handle);
 }
 if(!error){connections[c->handle.slot]=c;atomic_inc(&pending);}
 mutex_unlock(&core_lock);
 kfree(prompt);prompt=NULL;
 if(error){error=-error;goto bad;}
 wake_up_all(&work_wait);
 if(persistent){
  while(!completion_done(&c->finished)){
   struct msghdr msg={};struct kvec vec={.iov_base=(char *)&h+buffered,.iov_len=sizeof(h)-buffered};
   int got=kernel_recvmsg(sock,&msg,&vec,1,vec.iov_len,MSG_DONTWAIT);
   if(got>0){buffered+=got;if(buffered<sizeof(h))continue;
    if(le32_to_cpu(h.magic)==MAGIC&&le32_to_cpu(h.id)==id&&le32_to_cpu(h.op)==2){
     buffered=0;mutex_lock(&core_lock);mos_cancel(c->session,c->handle);mutex_unlock(&core_lock);wake_up_all(&work_wait);
    }else if(le32_to_cpu(h.magic)==MAGIC&&le32_to_cpu(h.op)==1){break;}else{lost=true;break;}
   }else if(got==-EAGAIN||got==-EWOULDBLOCK){
    wait_event_interruptible(*sk_sleep(sock->sk),completion_done(&c->finished)||!skb_queue_empty_lockless(&sock->sk->sk_receive_queue)||(READ_ONCE(sock->sk->sk_shutdown)&RCV_SHUTDOWN));
   }else{lost=true;break;}
  }
 }else{
  while(!completion_done(&c->finished)){
   if(recv_bytes(sock,&h,sizeof(h)))break;
   if(le32_to_cpu(h.magic)!=MAGIC||le32_to_cpu(h.id)!=id||le32_to_cpu(h.op)!=2)break;
   mutex_lock(&core_lock);mos_cancel(c->session,c->handle);mutex_unlock(&core_lock);wake_up_all(&work_wait);
  }
 }
 /* A buffered next request is legal only after the terminal frame. */
 if(!buffered||lost){mutex_lock(&core_lock);mos_cancel(c->session,c->handle);mutex_unlock(&core_lock);wake_up_all(&work_wait);}
 wait_for_completion(&c->finished);kfree(c);c=NULL;
 if(persistent&&!lost)goto next;goto out;
bad:
 frame(sock,19,id,error);kfree(prompt);kfree(c);
out:
 sock_release(sock);atomic_dec(&sockets);return 0;
}
int modelos_serve_network(void *unused) {
 struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(7373),.sin_addr.s_addr=htonl(INADDR_ANY)};
 struct socket *listener;int ret=sock_create_kern(&init_net,AF_INET,SOCK_STREAM,IPPROTO_TCP,&listener);
 if(ret)return ret;ret=kernel_bind(listener,(struct sockaddr_unsized *)&address,sizeof(address));
 if(!ret)ret=kernel_listen(listener,8);if(ret){sock_release(listener);return ret;}
 pr_info("SERVE_READY port=7373 workers=1 max_domains=8 no_userspace=1 objects=1\n");
 for(;;){struct socket *sock;struct task_struct *task;ret=kernel_accept(listener,&sock,0);
  if(ret){msleep(20);continue;}
  if(atomic_inc_return(&sockets)>16){atomic_dec(&sockets);sock_release(sock);continue;}
  sock->sk->sk_sndtimeo=10*HZ;tcp_sock_set_nodelay(sock->sk);
  task=kthread_run(connection_main,sock,"modelos/transport");
  if(IS_ERR(task)){atomic_dec(&sockets);sock_release(sock);}
 }
}
