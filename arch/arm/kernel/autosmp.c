/*
 * arch/arm/kernel/autosmp.c
 *
 * automatically hotplugs the multiple cpu cores on and off 
 * based on cpu load and suspend state
 * 
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * major revision: rewrite to simplify and optimize
 * July 2013, http://goo.gl/cdGw6x
 * revision: further optimizations, generalize for any number of cores
 * September 2013, http://goo.gl/448qBz
 * revision: generalize for other arm arch, rename as autosmp.c
 * December 2013, http://goo.gl/x5oyhy
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU 
 * General Public License included with the Linux kernel or available 
 * at www.gnu.org/licenses
 */

#include <linux/moduleparam.h>
#include <linux/earlysuspend.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>
#include <linux/rq_stats.h>

#define DEBUG 0
#define STATS 0

#define DEFAULT_RQ_POLL_JIFFIES		1
#define DEFAULT_DEF_TIMER_JIFFIES	5

#define ASMP_TAG	"[ASMP]: "
#define ASMP_STARTDELAY	20000

struct asmp_cpudata_t {
	long long unsigned int times_hotplugged;
};

static struct delayed_work asmp_work;
static struct workqueue_struct *asmp_workq;
static DEFINE_PER_CPU(struct asmp_cpudata_t, asmp_cpudata);

static struct asmp_param_struct {
	unsigned int delay;
	bool scroff_single_core;
	unsigned int max_cpus;
	unsigned int min_cpus;
	unsigned int load_limit_up;
	unsigned int load_limit_down;
	unsigned int cycle_up;
	unsigned int cycle_down;
} asmp_param = {
	.delay = 100,
	.scroff_single_core = true,
	.max_cpus = CONFIG_NR_CPUS,
	.min_cpus = 1,
	.load_limit_up = 25,
	.load_limit_down = 5,
	.cycle_up = 1,
	.cycle_down = 5,
};

static unsigned int cycle;
static int enabled = 1;

unsigned int get_rq_avg(void) {
	unsigned long flags = 0;
	unsigned int rq = 0;

	spin_lock_irqsave(&rq_lock, flags);
	rq = rq_info.rq_avg;
	rq_info.rq_avg = 0;
	spin_unlock_irqrestore(&rq_lock, flags);
	return rq;
}

#if CONFIG_NR_CPUS > 2
static int get_slowest_cpu(void) {
	int cpu, slow_cpu = 1;
	unsigned long rate, slow_rate = ULONG_MAX;

	get_online_cpus(); 
	for_each_online_cpu(cpu)
		if (cpu > 0) {
			rate = cpufreq_get(cpu);
			if (rate < slow_rate) {
				slow_cpu = cpu;
				slow_rate = rate;
			}
		}
	put_online_cpus(); 
	return slow_cpu;
}
#endif

static void rq_work_fn(struct work_struct *work) {
	int64_t diff, now;

	now = ktime_to_ns(ktime_get());
	diff = now - rq_info.def_start_time;
	do_div(diff, 1000*1000);
	rq_info.def_interval = (unsigned int) diff;
	rq_info.def_timer_jiffies = msecs_to_jiffies(rq_info.def_interval);
	rq_info.def_start_time = now;
}

static void __cpuinit asmp_work_fn(struct work_struct *work) {
	unsigned int cpu = 1;
	int nr_cpu_online;
	unsigned int rq_avg;

	cycle++;
	
	rq_avg = get_rq_avg();
	nr_cpu_online = num_online_cpus();

	if ((nr_cpu_online < asmp_param.max_cpus) && 
	    (rq_avg >= asmp_param.load_limit_up)) {
		if (cycle >= asmp_param.cycle_up) {
#if CONFIG_NR_CPUS > 2
			cpu = cpumask_next_zero(0, cpu_online_mask);
#endif
			cpu_up(cpu);
			cycle = 0;
#if DEBUG
			pr_info(ASMP_TAG"CPU[%d] on\n", cpu);
#endif
		}
	} else if ((nr_cpu_online > asmp_param.min_cpus) &&
		   (rq_avg <= asmp_param.load_limit_down)) {
		if (cycle >= asmp_param.cycle_down) {
#if CONFIG_NR_CPUS > 2
			cpu = get_slowest_cpu();
#endif
			cpu_down(cpu);
			cycle = 0;
#if STATS
			per_cpu(asmp_cpudata, cpu).times_hotplugged += 1;
#endif
#if DEBUG
			pr_info(ASMP_TAG"CPU[%d] off\n", cpu);
#endif
		}
	}

	queue_delayed_work(asmp_workq, &asmp_work,
			   msecs_to_jiffies(asmp_param.delay));
}

static void asmp_early_suspend(struct early_suspend *h) {
	int cpu = 1;

	/* unplug cpu cores */
	if (asmp_param.scroff_single_core)
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			if (cpu_online(cpu)) 
				cpu_down(cpu);

	/* suspend main work thread */
	if (enabled)
		cancel_delayed_work_sync(&asmp_work);

	pr_info(ASMP_TAG"autosmp suspended.\n");
}

static void __cpuinit asmp_late_resume(struct early_suspend *h) {
	int cpu = 1;

	/* hotplug cpu cores */
	if (asmp_param.scroff_single_core)
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			if (!cpu_online(cpu)) 
				cpu_up(cpu);

	/* resume main work thread */
	if (enabled) {
		queue_delayed_work(asmp_workq, &asmp_work, 
				msecs_to_jiffies(asmp_param.delay));
	}
	pr_info(ASMP_TAG"autosmp resumed.\n");
}

static struct early_suspend __refdata asmp_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = asmp_early_suspend,
	.resume = asmp_late_resume,
};

static int __cpuinit set_enabled(const char *val, const struct kernel_param *kp) {
	int ret = 0;
	int cpu = 1;

	ret = param_set_bool(val, kp);
	if (enabled) {
		queue_delayed_work(asmp_workq, &asmp_work,
				msecs_to_jiffies(asmp_param.delay));
		pr_info(ASMP_TAG"autosmp enabled\n");
	} else {
		cancel_delayed_work_sync(&asmp_work);
#if CONFIG_NR_CPUS > 2
		for (cpu = 1; cpu < nr_cpu_ids; cpu++)
#endif
			if (!cpu_online(cpu)) 
				cpu_up(cpu);
		pr_info(ASMP_TAG"autosmp disabled\n");
	}
	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "hotplug cpu cores based on demand");

/***************************** SYSFS START *****************************/
#define define_one_global_ro(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define define_one_global_rw(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

struct kobject *asmp_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", asmp_param.object);			\
}
show_one(delay, delay);
show_one(scroff_single_core, scroff_single_core);
show_one(min_cpus, min_cpus);
show_one(max_cpus, max_cpus);
show_one(load_limit_up, load_limit_up);
show_one(load_limit_down, load_limit_down);
show_one(cycle_up, cycle_up);
show_one(cycle_down, cycle_down);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	asmp_param.object = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(delay, delay);
store_one(scroff_single_core, scroff_single_core);
store_one(max_cpus, max_cpus);
store_one(min_cpus, min_cpus);
store_one(load_limit_up, load_limit_up);
store_one(load_limit_down, load_limit_down);
store_one(cycle_up, cycle_up);
store_one(cycle_down, cycle_down);

static struct attribute *asmp_attributes[] = {
	&delay.attr,
	&scroff_single_core.attr,
	&min_cpus.attr,
	&max_cpus.attr,
	&load_limit_up.attr,
	&load_limit_down.attr,
	&cycle_up.attr,
	&cycle_down.attr,
	NULL
};

static struct attribute_group asmp_attr_group = {
	.attrs = asmp_attributes,
	.name = "conf",
};
#if STATS
static ssize_t show_times_hotplugged(struct kobject *a, 
					struct attribute *b, char *buf) {
	ssize_t len = 0;
	int cpu = 0;

	for_each_possible_cpu(cpu) {
		len += sprintf(buf + len, "%i %llu\n", cpu, 
			per_cpu(asmp_cpudata, cpu).times_hotplugged);
	}
	return len;
}
define_one_global_ro(times_hotplugged);

static struct attribute *asmp_stats_attributes[] = {
	&times_hotplugged.attr,
	NULL
};

static struct attribute_group asmp_stats_attr_group = {
	.attrs = asmp_stats_attributes,
	.name = "stats",
};
#endif
/****************************** SYSFS END ******************************/

static int __init asmp_init(void) {
	int cpu, rc, err = 0;

	rq_wq = create_singlethread_workqueue("rq_stats");
	BUG_ON(!rq_wq);
	INIT_WORK(&rq_info.def_timer_work, rq_work_fn);
	spin_lock_init(&rq_lock);
	rq_info.rq_poll_jiffies = DEFAULT_RQ_POLL_JIFFIES;
	rq_info.def_timer_jiffies = DEFAULT_DEF_TIMER_JIFFIES;
	rq_info.def_start_time = ktime_to_ns(ktime_get());
	rq_info.rq_poll_last_jiffy = 0;
	rq_info.def_timer_last_jiffy = 0;
	rq_info.hotplug_disabled = 0;
	rq_info.init = 1;

	cycle = 0;
	for_each_possible_cpu(cpu)
		per_cpu(asmp_cpudata, cpu).times_hotplugged = 0;

	asmp_workq = alloc_workqueue("asmp", WQ_HIGHPRI, 0);
	if (!asmp_workq)
		return -ENOMEM;
	INIT_DELAYED_WORK(&asmp_work, asmp_work_fn);
	if (enabled)
		queue_delayed_work(asmp_workq, &asmp_work,
				   msecs_to_jiffies(ASMP_STARTDELAY));

	register_early_suspend(&asmp_early_suspend_handler);

	asmp_kobject = kobject_create_and_add("autosmp", kernel_kobj);
	if (asmp_kobject) {
		rc = sysfs_create_group(asmp_kobject, &asmp_attr_group);
		if (rc)
			pr_warn(ASMP_TAG"sysfs: ERROR, could not create sysfs group");
#if STATS
		rc = sysfs_create_group(asmp_kobject, &asmp_stats_attr_group);
		if (rc)
			pr_warn(ASMP_TAG"sysfs: ERROR, could not create sysfs stats group");
#endif
	} else
		pr_warn(ASMP_TAG"sysfs: ERROR, could not create sysfs kobj");

	pr_info(ASMP_TAG"%s init complete.", __func__);
	return err;
}
late_initcall(asmp_init);
