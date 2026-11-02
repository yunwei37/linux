// SPDX-License-Identifier: GPL-2.0
/* Same retained-DPS TCP application in full Linux and the kernel-only image. */
#include <linux/init.h>
#include <linux/kexec_handover.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/in.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/net_namespace.h>
#include <linux/modelos_dps_handoff.h>
#include <linux/modelos_kexec_tcp_request.h>
#include <linux/modelos_kexec_wire.h>
static struct dps_request_handoff *requests;
static volatile u32 *queue;
static u32 serial;
static bool failed;
static int io(struct socket *sock,void *data,size_t n,bool send_data)
{
 char *p=data;while(n){struct msghdr msg={.msg_flags=MSG_NOSIGNAL};struct kvec v={p,n};int got=send_data?kernel_sendmsg(sock,&msg,&v,1,n):kernel_recvmsg(sock,&msg,&v,1,n,0);if(got<=0)return got?got:-ECONNRESET;p+=got;n-=got;}return 0;
}
static int frame(struct socket *sock,u32 op,u32 id,u32 value)
{struct ktcp_frame f={KTCP_MAGIC,op,id,value};return io(sock,&f,sizeof(f),true);}
static int receive(struct socket *sock,struct ktcp_frame *f)
{return io(sock,f,sizeof(*f),false)||f->magic!=KTCP_MAGIC?-EPROTO:0;}
static void commit(void)
{requests->checksum=dps_request_checksum(requests);wmb();}
static int next_token(struct dps_request_cursor *r)
{
 u64 begin=ktime_get_ns(),deadline;u32 result;
 WRITE_ONCE(queue[128],r->last_token);WRITE_ONCE(queue[512],0xffffffffU);
 WRITE_ONCE(queue[2],1);WRITE_ONCE(queue[3],1);WRITE_ONCE(queue[4],r->next_pos);WRITE_ONCE(queue[6],0);WRITE_ONCE(queue[7],r->slot);wmb();WRITE_ONCE(queue[0],++serial);wmb();
 deadline=ktime_get_ns()+10000000000ULL;
 while(READ_ONCE(queue[1])!=serial){if(ktime_get_ns()>deadline){failed=true;return -ETIMEDOUT;}udelay(10);cond_resched();}
 rmb();result=READ_ONCE(queue[512]);if(READ_ONCE(queue[5])||result>=151936){failed=true;return -EIO;}
 r->output[r->emitted++]=result;r->last_token=result;r->next_pos++;r->budget--;commit();
 pr_info("KEXEC_TCP_GPU id=%u seq=%u serial=%u elapsed_ns=%llu\n",r->id,r->emitted-1,serial,ktime_get_ns()-begin);return 0;
}
static void connection(struct socket *sock)
{
 struct ktcp_frame f;struct dps_request_cursor *r;u32 generation;int ret;
 if(receive(sock,&f)||f.op!=KTCP_HELLO||frame(sock,KTCP_DONE,0,2))return;
 if(receive(sock,&f)||f.op!=KTCP_RESUME||!f.id||f.id>6||io(sock,&generation,4,false))goto bad;
 r=&requests->request[f.id-1];
 if(failed||generation!=r->generation||f.value<r->acked||f.value>r->emitted){
  pr_info("KEXEC_TCP_RESUME_REJECT id=%u generation=%u ack=%u serial=%u\n",f.id,generation,f.value,serial);frame(sock,KTCP_ERROR,f.id,ESTALE);return;
 }
 r->acked=f.value;commit();
 while(r->acked<r->total){
  u32 seq=r->acked,token;
  if(seq==r->emitted){ret=next_token(r);if(ret){frame(sock,KTCP_ERROR,r->id,-ret);return;}}
  token=r->output[seq];
  if(frame(sock,KTCP_TOKEN_SEQ,r->id,seq)||io(sock,&token,4,true))return;
  if(receive(sock,&f)||f.op!=KTCP_ACK||f.id!=r->id||f.value!=seq+1)return;
  r->acked=seq+1;commit();
 }
 frame(sock,KTCP_DONE,r->id,0);pr_info("KEXEC_TCP_COMPLETE id=%u emitted=%u acked=%u budget=%u\n",r->id,r->emitted,r->acked,r->budget);return;
bad:frame(sock,KTCP_ERROR,0,EINVAL);
}
static int server(void *unused)
{
 struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(KTCP_PORT),.sin_addr.s_addr=htonl(INADDR_ANY)};struct socket *listener;int ret;
 ret=sock_create_kern(&init_net,AF_INET,SOCK_STREAM,IPPROTO_TCP,&listener);if(ret)return ret;
 ret=kernel_bind(listener,(struct sockaddr_unsized *)&addr,sizeof(addr));if(!ret)ret=kernel_listen(listener,8);
 if(ret){sock_release(listener);pr_err("KEXEC_TCP_LISTEN_FAIL error=%d\n",ret);return ret;}
 pr_info("KEXEC_TCP_READY mode=%s port=7373 mm=%s serial=%u\n",IS_ENABLED(CONFIG_MODELOS_KERNEL_ONLY)?"modelos":"linux",current->mm?"present":"none",serial);
 for(;;){struct socket *sock;ret=kernel_accept(listener,&sock,0);if(ret){msleep(10);continue;}sock->sk->sk_rcvtimeo=10*HZ;sock->sk->sk_sndtimeo=10*HZ;tcp_sock_set_nodelay(sock->sk);connection(sock);sock_release(sock);}
}
static int __init consume(void)
{
 phys_addr_t start,bytes;struct dps_handoff *h;struct pci_dev *dev=NULL;u16 command;
 if(!is_kho_boot()||!reserve_mem_find_by_name(DPS_NAME,&start,&bytes)||bytes<DPS_REQUEST_OFFSET+sizeof(*requests))goto reject;
 h=phys_to_virt(start);requests=(void *)h+DPS_REQUEST_OFFSET;
 if(memcmp(h->magic,DPS_MAGIC,8)||h->version!=1||h->bytes!=sizeof(*h)||h->checksum!=dps_checksum(h)||h->probe_status!=1||h->copy_mode||h->serial!=DPS_REQUEST_TARGET_SERIAL||(h->queue_phys&4095)||!pfn_valid(h->queue_phys>>PAGE_SHIFT)||!PageReserved(pfn_to_page(h->queue_phys>>PAGE_SHIFT)))goto reject;
 queue=phys_to_virt(h->queue_phys);serial=h->serial;
 if(queue[0]!=serial||queue[1]!=serial||queue[17]!=serial||queue[5]||memcmp(requests->magic,DPS_REQUEST_MAGIC,8)||requests->version!=2||requests->bytes!=sizeof(*requests)||requests->count!=6||requests->split!=32||requests->reserved||requests->checksum!=dps_request_checksum(requests))goto reject;
 for(unsigned c=0;c<6;c++){
  struct dps_request_cursor *r=&requests->request[c];
  if(r->id!=c+1||r->generation!=1||r->slot!=c||r->case_id!=c||r->emitted!=32||r->input_count<1||r->input_count>128||r->total<=32||r->total>128||r->input_count+r->total>256||r->acked>r->emitted||r->next_pos!=r->input_count+r->emitted-1||r->last_token!=r->output[r->emitted-1]||r->budget!=r->total-r->emitted)goto reject;
 }
 while((dev=pci_get_device(PCI_VENDOR_ID_NVIDIA,PCI_ANY_ID,dev)))if((dev->class>>16)==PCI_BASE_CLASS_DISPLAY)break;
 if(!dev)goto reject;
 pci_read_config_word(dev,PCI_COMMAND,&command);pci_dev_put(dev);
 if(!(command&PCI_COMMAND_MASTER))goto reject;
 if(IS_ERR(kthread_run(server,NULL,"modelos/kexec-tcp")))goto reject;
 return 0;
reject:pr_err("KEXEC_TCP_REJECT\n");return -EINVAL;
}
late_initcall(consume);
