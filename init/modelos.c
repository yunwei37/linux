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

static DEFINE_PER_CPU(struct completion, modelos_started);

static int modelos_worker(void *data)
{
	unsigned int cpu = (unsigned long)data;

	pr_info("worker cpu=%u actual_cpu=%u pid=%d mm=%s online\n",
		cpu, raw_smp_processor_id(), current->pid,
		current->mm ? "present" : "none");
	complete(per_cpu_ptr(&modelos_started, cpu));
	while (!kthread_should_stop())
		schedule_timeout_interruptible(HZ);
	return 0;
}

void __noreturn modelos_enter(void)
{
	unsigned int cpu, workers = 0;
	unsigned short cs;

	asm volatile("mov %%cs,%0" : "=r" (cs));
	pr_info("MODELOS_PRIVILEGE cpl=%u cs=0x%x active_mm_init=%u\n",
		cs & 3, cs, current->active_mm == &init_mm);

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
	for (;;)
		schedule_timeout_uninterruptible(MAX_SCHEDULE_TIMEOUT);
}
