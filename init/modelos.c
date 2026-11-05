// SPDX-License-Identifier: GPL-2.0-only
#define pr_fmt(fmt) "modelos: " fmt
#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/mm_types.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>

#ifdef CONFIG_MODELOS_SERVE
int modelos_serve_prepare(void);
int modelos_serve_worker(unsigned int cpu);
int modelos_serve_network(void *unused);
#endif

static DEFINE_PER_CPU(struct completion, modelos_started);

static int modelos_worker(void *data)
{
	unsigned int cpu = (unsigned long)data;
	u64 iterations = 0, value = cpu + 1, start = ktime_get_ns(), next = start + 10ULL * NSEC_PER_SEC;
	unsigned short cs;

	pr_info("worker cpu=%u actual_cpu=%u pid=%d mm=%s online\n",
		cpu, raw_smp_processor_id(), current->pid,
		current->mm ? "present" : "none");
	complete(per_cpu_ptr(&modelos_started, cpu));
#ifdef CONFIG_MODELOS_SERVE
	return modelos_serve_worker(cpu);
#endif
	while (!kthread_should_stop()) {
		unsigned int i;
		for (i = 0; i < 65536; i++) { value = value * 6364136223846793005ULL + 1; barrier(); }
		iterations += 65536;
		if (ktime_get_ns() >= next) {
			asm volatile("mov %%cs,%0" : "=r" (cs));
			pr_info("MODELOS_BUSY cpu=%u elapsed_s=%llu iterations=%llu value=%llu cpl=%u mm=%s\n",
				cpu, (ktime_get_ns()-start)/NSEC_PER_SEC, iterations, value, cs & 3, current->mm ? "present" : "none");
			next += 10ULL * NSEC_PER_SEC;
		}
		cond_resched();
	}
	return 0;
}

/* Exercise real kthread return/stop after deleting user exit paths. */
static int modelos_lifecycle_probe(void *unused)
{

	complete(unused);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(1);
	return 37;
}

void __noreturn modelos_enter(void)
{
	unsigned int cpu, workers = 0;
	unsigned short cs;

	asm volatile("mov %%cs,%0" : "=r" (cs));
	pr_info("MODELOS_PRIVILEGE cpl=%u cs=0x%x active_mm_init=%u\n",
		cs & 3, cs, current->active_mm == &init_mm);
#ifdef CONFIG_MODELOS_SERVE
	if (modelos_serve_prepare())panic("modelos-serve: model preparation failed");
#endif

	{
		DECLARE_COMPLETION_ONSTACK(started);
		struct task_struct *probe;

		probe = kthread_run(modelos_lifecycle_probe, &started,
						      "modelos-exit-check");
		int result;

		if (IS_ERR(probe)) {
			result = PTR_ERR(probe);
		} else {
			wait_for_completion(&started);

			result = kthread_stop(probe);
		}
		pr_info("MODELOS_KTHREAD_EXIT result=%d expected=37\n", result);
	}

	for_each_online_cpu(cpu) {
		struct task_struct *task;

		init_completion(per_cpu_ptr(&modelos_started, cpu));
		task = kthread_create(modelos_worker, (void *)(unsigned long)cpu,
				      "modelos/%u", cpu);
		if (IS_ERR(task))
			panic("modelos: worker creation failed: %ld", PTR_ERR(task));
		kthread_bind(task, cpu);
		wake_up_process(task);
		if (!wait_for_completion_timeout(per_cpu_ptr(&modelos_started, cpu),
					  5 * HZ))
			panic("modelos: worker cpu=%u failed to start", cpu);
		workers++;
	}
	pr_info("MODELOS_READY workers=%u pid=%d pid1_mm=%s uptime_ns=%llu\n",
		workers, current->pid, current->mm ? "present" : "none",
		ktime_get_boottime_ns());
#ifdef CONFIG_MODELOS_SERVE
 {
  struct task_struct *network=kthread_run(modelos_serve_network,NULL,"modelos/listener");
  if(IS_ERR(network))panic("modelos-serve: network thread failed");
 }
#endif
	for (;;)
		schedule_timeout_uninterruptible(MAX_SCHEDULE_TIMEOUT);
}

#ifdef CONFIG_MODELOS_KERNEL_ONLY
void __set_task_comm(struct task_struct *tsk, const char *buf, bool exec)
{
	size_t len = min(strlen(buf), sizeof(tsk->comm) - 1);
	memcpy(tsk->comm, buf, len);
	memset(&tsk->comm[len], 0, sizeof(tsk->comm) - len);
}
#endif
