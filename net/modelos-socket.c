// SPDX-License-Identifier: GPL-2.0-only
/* Kernel sockets for ModelOS: no sockfs, file descriptors or user socket ABI. */
#include <linux/net.h>
#include <linux/net_tstamp.h>
#include <linux/stringhash.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/security.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <linux/ptp_classify.h>
static const struct net_proto_family __rcu *families[NPROTO];
static DEFINE_SPINLOCK(family_lock);
struct socket *sock_alloc(void)
{
 struct socket_alloc *a=kzalloc(sizeof(*a),GFP_KERNEL);
 if(!a)return NULL;
 init_waitqueue_head(&a->socket.wq.wait);a->socket.state=SS_UNCONNECTED;
 a->vfs_inode.i_mode=S_IFSOCK|S_IRWXUGO;a->vfs_inode.i_uid=current_fsuid();a->vfs_inode.i_gid=current_fsgid();
 return &a->socket;
}
void sock_release(struct socket *s)
{if(s->ops){s->ops->release(s);module_put(s->ops->owner);}kfree(container_of(s,struct socket_alloc,socket));}
int sock_register(const struct net_proto_family *f)
{
 int error=0;if(f->family>=NPROTO)return -ENOBUFS;
 spin_lock(&family_lock);if(rcu_access_pointer(families[f->family]))error=-EEXIST;else rcu_assign_pointer(families[f->family],f);spin_unlock(&family_lock);return error;
}
void sock_unregister(int family)
{spin_lock(&family_lock);RCU_INIT_POINTER(families[family],NULL);spin_unlock(&family_lock);synchronize_rcu();}
int __sock_create(struct net *net,int family,int type,int protocol,struct socket **out,int kern)
{
 const struct net_proto_family *f;struct socket *s;int error;
 if(family<0||family>=NPROTO||type<0||type>=SOCK_MAX)return -EAFNOSUPPORT;
 error=security_socket_create(family,type,protocol,1);if(error)return error;
 s=sock_alloc();if(!s)return -ENOMEM;s->type=type;
 rcu_read_lock();f=rcu_dereference(families[family]);if(!f||!try_module_get(f->owner)){rcu_read_unlock();sock_release(s);return -EAFNOSUPPORT;}rcu_read_unlock();
 error=f->create(net,s,protocol,1);module_put(f->owner);
 if(error){s->ops=NULL;sock_release(s);return error;}
 if(!try_module_get(s->ops->owner)){sock_release(s);return -EAFNOSUPPORT;}
 error=security_socket_post_create(s,family,type,protocol,1);if(error){sock_release(s);return error;}
 *out=s;return 0;
}
int sock_create_kern(struct net *n,int f,int t,int p,struct socket **s){return __sock_create(n,f,t,p,s,1);}
int sock_create_lite(int f,int t,int p,struct socket **out){*out=sock_alloc();if(!*out)return -ENOMEM;(*out)->type=t;return 0;}
int sock_sendmsg(struct socket *s,struct msghdr *m){int e=security_socket_sendmsg(s,m,msg_data_left(m));return e?e:s->ops->sendmsg(s,m,msg_data_left(m));}
int kernel_sendmsg(struct socket *s,struct msghdr *m,struct kvec *v,size_t n,size_t size){iov_iter_kvec(&m->msg_iter,ITER_SOURCE,v,n,size);return sock_sendmsg(s,m);}
int sock_recvmsg(struct socket *s,struct msghdr *m,int flags){int e=security_socket_recvmsg(s,m,msg_data_left(m),flags);return e?e:s->ops->recvmsg(s,m,msg_data_left(m),flags);}
int kernel_recvmsg(struct socket *s,struct msghdr *m,struct kvec *v,size_t n,size_t size,int flags){m->msg_control_is_user=false;iov_iter_kvec(&m->msg_iter,ITER_DEST,v,n,size);return sock_recvmsg(s,m,flags);}
int kernel_bind(struct socket *s,struct sockaddr_unsized *a,int len){return s->ops->bind(s,a,len);}
int kernel_listen(struct socket *s,int backlog){return s->ops->listen(s,backlog);}
int kernel_accept(struct socket *s,struct socket **out,int flags)
{
 struct proto_accept_arg arg={.flags=flags,.kern=true};struct socket *n=sock_alloc();int e;
 if(!n)return -ENOMEM;n->type=s->type;n->ops=s->ops;__module_get(n->ops->owner);
 e=s->ops->accept(s,n,&arg);if(e){sock_release(n);return e;}*out=n;return 0;
}
int kernel_connect(struct socket *s,struct sockaddr *a,int len,int flags){return s->ops->connect(s,a,len,flags);}
int kernel_sock_shutdown(struct socket *s,enum sock_shutdown_cmd how){return s->ops->shutdown(s,how);}
int kernel_getsockname(struct socket *s,struct sockaddr *a){return s->ops->getname(s,a,0);}
int kernel_getpeername(struct socket *s,struct sockaddr *a){return s->ops->getname(s,a,1);}
static int __init modelos_sock_init(void){int e=net_sysctl_init();if(e)return e;skb_init();ptp_classifier_init();return 0;}
core_initcall(modelos_sock_init);
#ifdef CONFIG_NET_RX_BUSY_POLL
unsigned int sysctl_net_busy_read __read_mostly;
unsigned int sysctl_net_busy_poll __read_mostly;
#endif
bool sock_is_registered(int family){return family>=0&&family<NPROTO&&rcu_access_pointer(families[family]);}
void __sock_tx_timestamp(__u32 flags,__u8 *out)
{
 if(flags&SOF_TIMESTAMPING_TX_HARDWARE)*out|=SKBTX_HW_TSTAMP_NOBPF;
 if(flags&SOF_TIMESTAMPING_TX_SOFTWARE)*out|=SKBTX_SW_TSTAMP;
 if(flags&SOF_TIMESTAMPING_TX_SCHED)*out|=SKBTX_SCHED_TSTAMP;
 if(flags&SOF_TIMESTAMPING_TX_COMPLETION)*out|=SKBTX_COMPLETION_TSTAMP;
}

unsigned int full_name_hash(const void *salt,const char *name,unsigned int len)
{
 unsigned long h=init_name_hash(salt);
 while(len--)h=partial_name_hash((unsigned char)*name++,h);
 return end_name_hash(h);
}
