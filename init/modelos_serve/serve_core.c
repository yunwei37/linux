// SPDX-License-Identifier: MIT
#define pr_fmt(fmt) "modelos-serve: " fmt
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/in.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/mm_types.h>
#include <linux/net.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <asm/cpufeature.h>
#include <asm/fpu/api.h>
#include <net/net_namespace.h>
#include <net/sock.h>
#include <net/tcp.h>
#include "qwen.h"
#ifdef CONFIG_MODELOS_L1_TCP
#include <linux/modelos_gpu.h>
static bool service_stopping;
#endif

#define MODEL_PHYS 0x40000000ULL
#define MODEL_RESERVED (1280ULL << 20)
#define DOMAIN_MAX 8
#define PROMPT_MAX 128
#define BUDGET_MAX 128
#define WIRE_MAGIC 0x31534f4d
#define OP_SUBMIT 1
#define OP_CANCEL 2
#define OP_STATS 3
#define OUT_TOKEN 16
#define OUT_DONE 17
#define OUT_STATS 18
#define OUT_ERROR 19

struct wire_header { __le32 magic, op, id, arg; };
enum domain_state { ALLOCATING, READY, BUSY, DONE };
struct domain {
	struct socket *sock;
	struct completion finished;
	struct q_state *arena;
	u64 bytes;
	u32 id, prompt_len, budget, pos, generated, token, slot;
	u32 prompt[PROMPT_MAX];
	enum domain_state state;
	bool cancel;
};
#ifndef CONFIG_MODELOS_L1_TCP
static struct q_model model;
#endif
static struct domain *domains[DOMAIN_MAX];
static DEFINE_SPINLOCK(queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(work_wait);
static atomic_t ready_count = ATOMIC_INIT(0);
static atomic_t connections = ATOMIC_INIT(0);
static u32 cursor, live, active;
static u64 arena_bytes, admitted, released, peak_active, worker_steps[4];

static int send_bytes(struct socket *sock, const void *data, size_t bytes)
{
	while (bytes) {
		struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
		struct kvec vec = { .iov_base = (void *)data, .iov_len = bytes };
		int sent = kernel_sendmsg(sock, &msg, &vec, 1, bytes);
		if (sent <= 0)
			return sent ? sent : -EPIPE;
		data = (const u8 *)data + sent;
		bytes -= sent;
	}
	return 0;
}

static int recv_bytes(struct socket *sock, void *data, size_t bytes)
{
	while (bytes) {
		struct msghdr msg = {0};
		struct kvec vec = { .iov_base = data, .iov_len = bytes };
		int got = kernel_recvmsg(sock, &msg, &vec, 1, bytes, 0);
		if (got <= 0)
			return got ? got : -ECONNRESET;
		data = (u8 *)data + got;
		bytes -= got;
	}
	return 0;
}

static int frame(struct socket *sock, u32 op, u32 id, u32 arg)
{
	struct wire_header h = { cpu_to_le32(WIRE_MAGIC), cpu_to_le32(op),
		cpu_to_le32(id), cpu_to_le32(arg) };
	return send_bytes(sock, &h, sizeof(h));
}

static u64 user_tasks(void)
{
	struct task_struct *group, *task;
	u64 count = 0;
	rcu_read_lock();
	for_each_process_thread(group, task)
		if (READ_ONCE(task->mm))
			count++;
	rcu_read_unlock();
	return count;
}

static void send_stats(struct socket *sock, u32 id)
{
	__le64 data[10];
	u32 arenas;
	unsigned long flags;
	int i;
	u64 users = user_tasks();
	spin_lock_irqsave(&queue_lock, flags);
	arenas = active;
	data[0] = cpu_to_le64(live);
	data[1] = cpu_to_le64(arena_bytes);
	data[2] = cpu_to_le64(admitted);
	data[3] = cpu_to_le64(released);
	data[4] = cpu_to_le64(peak_active);
	data[5] = cpu_to_le64(users);
	for (i = 0; i < 4; i++)
		data[6+i] = cpu_to_le64(worker_steps[i]);
	spin_unlock_irqrestore(&queue_lock, flags);
	if (!frame(sock, OUT_STATS, id, arenas))
		send_bytes(sock, data, sizeof(data));
}

#ifndef CONFIG_MODELOS_L1_TCP
static bool valid_tensor(u64 off, u64 bytes)
{
	return off >= Q_HEADER_BYTES && !(off & 63) &&
		off <= model.h->bytes && bytes <= model.h->bytes - off;
}

#endif

int modelos_serve_prepare(void)
{
#ifdef CONFIG_MODELOS_L1_TCP
	if(modelos_gpu_prepare() || user_tasks())return -ENODEV;
	pr_info("SERVE_MODEL bytes=1192235008 BF16=1 layers=28 userspace_tasks=0 GPU=1\n");
	pr_info("SERVE_USER_ENTRY compiled_out=1 no_initramfs=1\n");
	return 0;
#else
	const struct q_header *h;
	int i;
	if (!boot_cpu_has(X86_FEATURE_AVX2) || !boot_cpu_has(X86_FEATURE_FMA))
		return -ENODEV;
	model.base = memremap(MODEL_PHYS, MODEL_RESERVED, MEMREMAP_WB);
	if (!model.base)
		return -ENOMEM;
	model.h = h = (const struct q_header *)model.base;
	if (memcmp(h->magic, "MOSQ3B1", 8) || h->version != 1 ||
	    h->dim != Q_DIM || h->hidden != Q_FF || h->layers != Q_LAYERS ||
	    h->heads != Q_HEADS || h->kv_heads != Q_KV_HEADS || h->head_dim != Q_HD ||
	    h->vocab != Q_VOCAB || h->max_seq != Q_MAX_SEQ || h->bytes > MODEL_RESERVED)
		return -EINVAL;
	if (!valid_tensor(h->embedding, (u64)Q_VOCAB * Q_DIM * 2) ||
	    !valid_tensor(h->norm, Q_DIM * 2) ||
	    !valid_tensor(h->rope_cos, Q_MAX_SEQ * (Q_HD / 2) * 4) ||
	    !valid_tensor(h->rope_sin, Q_MAX_SEQ * (Q_HD / 2) * 4))
		return -EINVAL;
	for (i = 0; i < Q_LAYERS; i++) {
		const struct q_layer *l = &h->layer[i];
		if (!valid_tensor(l->ln1, Q_DIM * 2) || !valid_tensor(l->ln2, Q_DIM * 2) ||
		    !valid_tensor(l->qn, Q_HD * 2) || !valid_tensor(l->kn, Q_HD * 2) ||
		    !valid_tensor(l->wq, (u64)Q_QDIM * Q_DIM * 2) ||
		    !valid_tensor(l->wk, (u64)Q_KVDIM * Q_DIM * 2) ||
		    !valid_tensor(l->wv, (u64)Q_KVDIM * Q_DIM * 2) ||
		    !valid_tensor(l->wo, (u64)Q_DIM * Q_QDIM * 2) ||
		    !valid_tensor(l->wg, (u64)Q_FF * Q_DIM * 2) ||
		    !valid_tensor(l->wu, (u64)Q_FF * Q_DIM * 2) ||
		    !valid_tensor(l->wd, (u64)Q_DIM * Q_FF * 2))
			return -EINVAL;
	}
	if (user_tasks())
		return -EINVAL;
	pr_info("SERVE_MODEL bytes=%llu BF16=1 layers=%u userspace_tasks=0\n", h->bytes, h->layers);
	pr_info("SERVE_USER_ENTRY compiled_out=1 no_initramfs=1\n");
	return 0;
#endif
}

static struct domain *claim_domain(void)
{
	struct domain *d = NULL;
	unsigned long flags;
	int i;
	spin_lock_irqsave(&queue_lock, flags);
	for (i = 0; i < DOMAIN_MAX; i++) {
		u32 slot = (cursor + i) % DOMAIN_MAX;
		if (domains[slot] && domains[slot]->state == READY) {
			d = domains[slot];
			d->state = BUSY;
			cursor = (slot + 1) % DOMAIN_MAX;
			atomic_dec(&ready_count);
			break;
		}
	}
	spin_unlock_irqrestore(&queue_lock, flags);
	return d;
}

static void finish_domain(struct domain *d, u32 reason)
{
	unsigned long flags;
	struct socket *sock = d->sock;
#ifdef CONFIG_MODELOS_L1_TCP
	if(modelos_gpu_arena_release(d->slot,d->id))panic("GPU arena release failed");
#else
	kvfree(d->arena);
#endif
	d->arena = NULL;
	spin_lock_irqsave(&queue_lock, flags);
	active--;
	arena_bytes -= d->bytes;
	released++;
	d->state = DONE;
	spin_unlock_irqrestore(&queue_lock, flags);
	pr_info("DOMAIN_RELEASE id=%u generated=%u reason=%u bytes=%llu\n",
		d->id, d->generated, reason, d->bytes);
	frame(sock, OUT_DONE, d->id, reason);
	kernel_sock_shutdown(sock, SHUT_RD);
	/* The connection owner may free d as soon as this completion is visible. */
	complete(&d->finished);
}

int modelos_serve_worker(unsigned int cpu)
{
	unsigned short cs;
	asm volatile("mov %%cs,%0" : "=r" (cs));
	if ((cs & 3) || current->mm || current->active_mm != &init_mm || cpu >= 4)
		return -EINVAL;
	pr_info("SERVE_WORKER cpu=%u actual_cpu=%u cpl=%u mm=none\n", cpu, raw_smp_processor_id(), cs & 3);
	while (!kthread_should_stop()) {
		struct domain *d;
		unsigned long flags;
		u32 next = 0;
#ifndef CONFIG_MODELOS_L1_TCP
		u32 layer;
#endif
		bool emit;
		wait_event_interruptible(work_wait, atomic_read(&ready_count) || kthread_should_stop());
		d = claim_domain();
		if (!d)
			continue;
		if (READ_ONCE(d->cancel)) {
			finish_domain(d, 1);
			continue;
		}

#ifdef CONFIG_MODELOS_L1_TCP
		/* Completion is the fence before cancellation can release this arena. */
		if(modelos_gpu_step(cpu,d->slot,d->id,d->pos<d->prompt_len?d->prompt[d->pos]:d->token,d->pos,&next))
			panic("persistent GPU decode failed");
#else
		kernel_fpu_begin();
		q_begin(&model, d->arena, d->pos < d->prompt_len ? d->prompt[d->pos] : d->token);
		kernel_fpu_end();
		for (layer = 0; layer < Q_LAYERS && !READ_ONCE(d->cancel); layer++) {
			kernel_fpu_begin();
			q_layer(&model, d->arena, layer, d->pos);
			kernel_fpu_end();
			cond_resched();
		}
#endif
		if (READ_ONCE(d->cancel)) {
			finish_domain(d, 1);
			continue;
		}
		emit = d->pos + 1 >= d->prompt_len;
		if (emit) {
#ifndef CONFIG_MODELOS_L1_TCP
			kernel_fpu_begin();
			next = q_logits(&model, d->arena);
			kernel_fpu_end();
#endif
			if (READ_ONCE(d->cancel)) {
				finish_domain(d, 1);
				continue;
			}
			d->token = next;
			d->generated++;
			if (frame(d->sock, OUT_TOKEN, d->id, next)) {
				finish_domain(d, 2);
				continue;
			}
		}
		d->pos++;
		spin_lock_irqsave(&queue_lock, flags);
		worker_steps[cpu]++;
		spin_unlock_irqrestore(&queue_lock, flags);
		if (d->generated == d->budget) {
			finish_domain(d, 0);
			continue;
		}
		spin_lock_irqsave(&queue_lock, flags);
		d->state = READY;
		atomic_inc(&ready_count);
		spin_unlock_irqrestore(&queue_lock, flags);
		wake_up_all(&work_wait);
		cond_resched();
	}
	return 0;
}

static int connection_main(void *arg)
{
	struct socket *sock = arg;
	struct domain *d = NULL;
	struct wire_header h;
	__le32 budget;
	unsigned long flags;
	u32 i, slot = DOMAIN_MAX, id = 0, count;
	int error = EINVAL;
	if (recv_bytes(sock, &h, sizeof(h)) || le32_to_cpu(h.magic) != WIRE_MAGIC)
		goto out;
	id = le32_to_cpu(h.id);
#ifdef CONFIG_MODELOS_L1_TCP
	if(le32_to_cpu(h.op)==4) { /* Test/gateway drain: reject new admissions. */
		int ret;
		spin_lock_irqsave(&queue_lock,flags);
		if(live||service_stopping){spin_unlock_irqrestore(&queue_lock,flags);frame(sock,OUT_ERROR,id,EBUSY);goto out;}
		service_stopping=true;
		spin_unlock_irqrestore(&queue_lock,flags);
		ret=modelos_gpu_stop();frame(sock,ret?OUT_ERROR:OUT_DONE,id,ret?-ret:0);goto out;
	}
#endif
	if (le32_to_cpu(h.op) == OP_STATS) {
		send_stats(sock, id);
		goto out;
	}
	count = le32_to_cpu(h.arg);
	if (le32_to_cpu(h.op) != OP_SUBMIT || !id || !count || count > PROMPT_MAX ||
	    recv_bytes(sock, &budget, sizeof(budget)))
		goto bad;
	if (!le32_to_cpu(budget) || le32_to_cpu(budget) > BUDGET_MAX ||
	    count + le32_to_cpu(budget) > Q_MAX_SEQ)
		goto bad;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d) {
		error = ENOMEM;
		goto bad;
	}
	d->id = id;
	d->sock = sock;
	d->prompt_len = count;
	d->budget = le32_to_cpu(budget);
#ifdef CONFIG_MODELOS_L1_TCP
	d->bytes = MODELOS_GPU_ARENA_BYTES;
#else
	d->bytes = q_arena_bytes(count + d->budget);
#endif
	init_completion(&d->finished);
	if (recv_bytes(sock, d->prompt, count * sizeof(u32)))
		goto bad;
	for (i = 0; i < count; i++) {
		d->prompt[i] = le32_to_cpu((__force __le32)d->prompt[i]);
		if (d->prompt[i] >= Q_VOCAB)
			goto bad;
	}
	spin_lock_irqsave(&queue_lock, flags);
	for (i = 0; i < DOMAIN_MAX; i++) {
		if (!domains[i])
			slot = i;
		else if (domains[i]->id == id) {
			slot = DOMAIN_MAX;
			break;
		}
	}
#ifdef CONFIG_MODELOS_L1_TCP
	if(service_stopping)slot=DOMAIN_MAX;
#endif
	if (slot != DOMAIN_MAX) {
		d->slot = slot;
		d->state = ALLOCATING;
		domains[slot] = d;
		live++;
	}
	spin_unlock_irqrestore(&queue_lock, flags);
	if (slot == DOMAIN_MAX) {
		error = EBUSY;
		goto bad;
	}
#ifdef CONFIG_MODELOS_L1_TCP
	if(modelos_gpu_arena_acquire(slot,id)) {
#else
	d->arena = kvzalloc(d->bytes, GFP_KERNEL);
	if (!d->arena) {
#endif
		spin_lock_irqsave(&queue_lock, flags);
		domains[slot] = NULL;
		live--;
		spin_unlock_irqrestore(&queue_lock, flags);
		error = ENOMEM;
		goto bad;
	}
#ifndef CONFIG_MODELOS_L1_TCP
	d->arena->capacity = count + d->budget;
#endif
	spin_lock_irqsave(&queue_lock, flags);
	d->state = READY;
	active++;
	admitted++;
	arena_bytes += d->bytes;
	if (active > peak_active)
		peak_active = active;
	atomic_inc(&ready_count);
	spin_unlock_irqrestore(&queue_lock, flags);
	pr_info("DOMAIN_ADMIT id=%u prompt=%u budget=%u bytes=%llu\n", id, count, d->budget, d->bytes);
	wake_up_all(&work_wait);
	while (!completion_done(&d->finished)) {
		if (recv_bytes(sock, &h, sizeof(h)))
			break;
		if (le32_to_cpu(h.magic) != WIRE_MAGIC || le32_to_cpu(h.id) != id ||
		    le32_to_cpu(h.op) != OP_CANCEL)
			break;
		WRITE_ONCE(d->cancel, true);
		wake_up_all(&work_wait);
	}
	WRITE_ONCE(d->cancel, true);
	wake_up_all(&work_wait);
	wait_for_completion(&d->finished);
	spin_lock_irqsave(&queue_lock, flags);
	domains[slot] = NULL;
	live--;
	spin_unlock_irqrestore(&queue_lock, flags);
	kfree(d);
	d = NULL;
	goto out;
bad:
	frame(sock, OUT_ERROR, id, error);
	kfree(d);
out:
	sock_release(sock);
	atomic_dec(&connections);
	return 0;
}

int modelos_serve_network(void *unused)
{
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(7373),
		.sin_addr.s_addr = htonl(INADDR_ANY) };
	struct socket *listener;
	int ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, IPPROTO_TCP, &listener);
	if (ret)
		return ret;
	ret = kernel_bind(listener, (struct sockaddr_unsized *)&address, sizeof(address));
	if (!ret)
		ret = kernel_listen(listener, DOMAIN_MAX);
	if (ret) {
		sock_release(listener);
		return ret;
	}
	pr_info("SERVE_READY port=7373 workers=4 max_domains=8 no_userspace=1\n");
	for (;;) {
		struct socket *sock;
		struct task_struct *task;
		ret = kernel_accept(listener, &sock, 0);
		if (ret) {
			msleep(20);
			continue;
		}
		if (atomic_inc_return(&connections) > 16) {
			atomic_dec(&connections);
			sock_release(sock);
			continue;
		}
		sock->sk->sk_sndtimeo = 10 * HZ;
		tcp_sock_set_nodelay(sock->sk);
		task = kthread_run(connection_main, sock, "modelos/connection");
		if (IS_ERR(task)) {
			atomic_dec(&connections);
			sock_release(sock);
		}
	}
}
