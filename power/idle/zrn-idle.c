// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-idle: CPU idle for the Exynos 9810 (Galaxy S9) under mainline.
 *
 * Without it the cores only ever pause in WFI: powered, clocked, caches on.
 * The firmware does offer per-core power-down through PSCI CPU_SUSPEND
 * (state 0x10000, as the stock device tree has it), but on this SoC the PMU
 * is helped by a small "flexible PMU" controller that has to be told first.
 * Samsung's kernel (exynos-powermode.c, cal-if pmucal_cpu.c) does, around
 * every core power-down:
 *
 *   before   CPU_INFORM[core] = C2 (1)            PMU 0x14060860 + 4 * core
 *            GRP2_INTR_BID_ENABLE |= bit           0x14070200
 *            if GRP1 pending, GRP1_INTR_BID_CLEAR  0x14070108 / 0x1407010c
 *            PMU_EVENT_INTERRUPT_ENABLE_GRP2 |= bit  PMU 0x14067f08
 *   after    CPU_INFORM[core] = 0, the three again with the bit cleared
 *            (GRP2 pending cleared instead: 0x14070208 / 0x1407020c)
 *
 * where core is cluster * 4 + cpu (little cluster 0; see zrn_core()) and bit is core's
 * interrupt line into the controller: 4..7 for the little cores, 0..3 for
 * the big ones. Without that, a core that powers down is not woken again,
 * which is also why an offlined core never comes back on this kernel.
 *
 * This registers a cpuidle driver per cluster: WFI, and the power-down state
 * with the stock latencies. psci_cpu_suspend_enter() is not exported, so its
 * address is passed in from /proc/kallsyms:
 *
 *   insmod zrn-cpuidle.ko suspend_enter=0x$(awk '$3=="psci_cpu_suspend_enter"{print $1}' /proc/kallsyms) cpus=0xff
 *
 * cpus is a mask of the CPUs to drive (default 0xff); the others keep WFI.
 */
#include <linux/cpu.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/cpu_pm.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/arm-smccc.h>
#include <linux/tick.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <asm/cputype.h>
#include <asm/memory.h>

#include "zrn-idle.h"

#define PMU_BASE		0x14060000
#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define PMU_EVENT_INT_EN_GRP2	0x7f08
#define BID_BASE		0x14070000
#define GRP1_PEND		0x0108
#define GRP1_CLEAR		0x010c
#define GRP2_ENABLE		0x0200
#define GRP2_PEND		0x0208
#define GRP2_CLEAR		0x020c
#define INFORM_C2		1
#define PSCI_CORE_POWERDOWN	0x10000

static unsigned long suspend_fn, resume_fn;
module_param_named(suspend_enter, suspend_fn, ulong, 0444);
MODULE_PARM_DESC(suspend_enter, "address of cpu_suspend (from /proc/kallsyms)");
module_param_named(resume, resume_fn, ulong, 0444);
MODULE_PARM_DESC(resume, "address of cpu_resume (from /proc/kallsyms)");
static unsigned int cpus = 0x0c;
module_param(cpus, uint, 0444);
MODULE_PARM_DESC(cpus, "mask of the CPUs to give the power-down state");

static int (*cpu_suspend_p)(unsigned long arg, int (*fn)(unsigned long));
static unsigned long cpu_resume_pa;
static void __iomem *pmu, *bid, *cluster_mailbox;
static bool s2idle_cpd, cluster_down;
static unsigned long cpd_attempts, cpd_entries;
static int cpd_last_error;
module_param(s2idle_cpd, bool, 0644);
MODULE_PARM_DESC(s2idle_cpd, "offer non-boot cluster power down during system suspend only");
module_param(cpd_attempts, ulong, 0444);
module_param(cpd_entries, ulong, 0444);
module_param(cpd_last_error, int, 0444);

/*
 * The finisher: one SMC, the function id Samsung's firmware implements
 * (from the stock device tree's psci node), with the state and the physical
 * address cpu_resume runs at. It must not touch the stack: cpu_suspend has
 * already saved the register frame it will be resumed into.
 */
static int zrn_suspend_finisher(unsigned long state)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0xC4000001, state, cpu_resume_pa, 0, 0, 0, 0, 0, &res);
	return (int)res.a0;
}
static DEFINE_RAW_SPINLOCK(pmu_lock);

/*
 * The PMU's core number, as Samsung's linear_phycpu(): cluster * 4 + core.
 * The little Cortex-A55s report a multithreading-style MPIDR (MT set, core
 * in Aff1, cluster in Aff2); the big cores the plain one (core in Aff0).
 */
static unsigned int zrn_core(u64 mpidr)
{
	int lvl = (mpidr & MPIDR_MT_BITMASK) ? 1 : 0;

	return (MPIDR_AFFINITY_LEVEL(mpidr, (lvl + 1)) << 2) | MPIDR_AFFINITY_LEVEL(mpidr, lvl);
}

static void rmw(void __iomem *reg, u32 mask, u32 value)
{
	writel_relaxed((readl_relaxed(reg) & ~mask) | value, reg);
}

static void core_off(unsigned int core)
{
	u32 bit = BIT((core + 4) & 7);

	if (WARN_ON_ONCE(core > 7))
		return;
	raw_spin_lock(&pmu_lock);
	writel_relaxed(INFORM_C2, pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, bit);
	if (readl_relaxed(bid + GRP1_PEND) & bit)
		rmw(bid + GRP1_CLEAR, bit, bit);
	rmw(pmu + PMU_EVENT_INT_EN_GRP2, bit, bit);
	raw_spin_unlock(&pmu_lock);
	wmb();
}

static void core_on(unsigned int core)
{
	u32 bit = BIT((core + 4) & 7);

	if (core > 7)
		return;
	raw_spin_lock(&pmu_lock);
	writel_relaxed(0, pmu + PMU_CPU_INFORM(core));
	rmw(bid + GRP2_ENABLE, bit, 0);
	if (readl_relaxed(bid + GRP2_PEND) & bit)
		rmw(bid + GRP2_CLEAR, bit, bit);
	rmw(pmu + PMU_EVENT_INT_EN_GRP2, bit, 0);
	raw_spin_unlock(&pmu_lock);
}

static int enter_wfi(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	dsb(sy);
	wfi();
	return idx;
}

/* Samsung flexpmu_cal_cpu_exynos9810.h: cluster1_off requests CPD through
 * MAILBOX_APM2AP bit 24; PSCI_CLUSTER_SLEEP packs to 0x01000000. Any big
 * CPU waking cancels the shared request before returning to its callers.
 * DFD-enabled firmware needs additional reset handling; refuse that case. */
static bool cluster_prepare(unsigned int core)
{
	unsigned long flags;
	bool enter = false;
	int cpu;

	if (core < 4 || !READ_ONCE(s2idle_cpd) ||
	    (readl_relaxed(pmu + 0x0500) & BIT(0)))
		return false;
	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	for_each_online_cpu(cpu) {
		if (cpu >= 4 && !test_bit(cpu, zrn_c2_mask))
			goto out;
	}
	if (!cluster_down) {
		cluster_down = true;
		rmw(cluster_mailbox + 0x10, BIT(24), BIT(24));
		writel_relaxed(2, pmu + PMU_CPU_INFORM(core));
		wmb();
		cpd_attempts++;
		enter = true;
	}
out:
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	return enter;
}

static int enter_powerdown_common(struct cpuidle_device *dev, struct cpuidle_driver *drv,
				 int idx, bool suspended)
{
	u64 mpidr = read_cpuid_mpidr();
	unsigned int core = zrn_core(mpidr);
	bool cpd;
	int ret;

	if (core > 7)	/* not a core the PMU knows: stay in WFI */
		return enter_wfi(dev, drv, 0);
	ret = cpu_pm_enter();
	if (ret)
		return -EBUSY;
	core_off(core);
	/* in C2 from here: SICD (zrn-sicd.c) looks for every online core set */
	zrn_c2_mark(core, true);
	cpd = suspended && cluster_prepare(core);
	if (!zrn_sicd_try(dev, core, suspended, &ret)) {
		/*
		 * Not psci_cpu_suspend_enter(): it reads bit 30 of the state
		 * word to tell retention from power-down, and Samsung's
		 * 0x10000 has it clear, so it would pass entry point 0 -- and
		 * a core that powers down resumes through no frame at all
		 * (seen: the first IRQ lands on a corrupted stack).
		 * cpu_suspend saves the frame on this stack and our finisher
		 * hands the firmware cpu_resume's physical address, exactly
		 * what Samsung's own idle path does.
		 */
		ret = cpu_suspend_p(cpd ? 0x01000000 : PSCI_CORE_POWERDOWN,
				    zrn_suspend_finisher);
	}
	if (cpd) {
		cpd_last_error = ret;
		if (!ret)
			cpd_entries++;
	}
	zrn_c2_mark(core, false);
	core_on(core);
	cpu_pm_exit();
	return ret ? -EBUSY : idx;
}

static int enter_powerdown(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	return enter_powerdown_common(dev, drv, idx, false);
}

static int enter_s2idle(struct cpuidle_device *dev, struct cpuidle_driver *drv, int idx)
{
	/* The PM core has frozen tasks, suspended devices and stopped ticks.
	 * cpu_suspend() still owns the RCU/context-tracking transition. */
	return enter_powerdown_common(dev, drv, idx, true);
}

static struct cpuidle_driver drivers[2] = {
	{
		.name = "zrn_idle_little",
		/*
		 * No .owner: with one, each registered CPU holds the module and
		 * it can never be unloaded. cpuidle_unregister() waits for the
		 * CPUs to leave our states before the module goes.
		 */
		.states = {
			{ .name = "WFI", .desc = "wait for interrupt", .enter = enter_wfi,
			  .exit_latency = 1, .target_residency = 1 },
			{ .name = "C2", .desc = "core power down", .enter = enter_powerdown,
			  .enter_s2idle = enter_s2idle,
			  /* cpu_suspend() owns the context-tracking transition. */
			  .flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			  .exit_latency = 160, .target_residency = 2000 },
		},
		.state_count = 2,
	},
	{
		.name = "zrn_idle_big",
		.states = {
			{ .name = "WFI", .desc = "wait for interrupt", .enter = enter_wfi,
			  .exit_latency = 1, .target_residency = 1 },
			{ .name = "C2", .desc = "core power down", .enter = enter_powerdown,
			  .enter_s2idle = enter_s2idle,
			  .flags = CPUIDLE_FLAG_TIMER_STOP | CPUIDLE_FLAG_RCU_IDLE,
			  .exit_latency = 220, .target_residency = 3500 },
		},
		.state_count = 2,
	},
};
static bool registered[2];

static void zrn_broadcast_exit(void);

static void zrn_idle_exit(void)
{
	/* off the idle states first: they are what needs the broadcast */
	for (int i = 0; i < 2; i++) {
		if (registered[i])
			cpuidle_unregister(&drivers[i]);
		kfree(drivers[i].cpumask);
	}
	zrn_broadcast_exit();
	if (bid)
		iounmap(bid);
	if (pmu)
		iounmap(pmu);
	if (cluster_mailbox)
		iounmap(cluster_mailbox);
}


/*
 * ------------------------------------------------- the MCT's global comparator
 *
 * A core that powers down also stops its arch timer, and this kernel has no
 * hardware broadcast timer: the only one registered is bc_hrtimer, the
 * software fallback, which schedules on a CPU that is itself about to stop
 * ticking. Anything sleeping on a timer on an idled core then never wakes
 * (measured: sshd hung while the phone still answered ping).
 *
 * The Exynos MCT (0x10040000) runs off the oscillator and never stops. Its
 * global comparator 0, GIC SPI 466 (from the stock device tree's mct-map),
 * becomes the broadcast device instead: timers of powered-down cores are
 * served by a G0 interrupt on cpu0, which IPIs the sleeping cores. Samsung's
 * kernel uses the MCT for exactly this.
 *
 * The rate is measured, not assumed: the comparator cycle count comes from
 * delta_ns * rate / NSEC_PER_SEC, and a wrong rate moves every broadcast
 * wakeup.
 */
#define MCT_PHYS		0x10040000UL
#define MCT_G_CNT_L		0x100
#define MCT_G_CNT_U		0x104
#define MCT_G_CNT_WSTAT		0x110
#define MCT_G_COMP0_L		0x200
#define MCT_G_COMP0_U		0x204
#define MCT_G_COMP1_L		0x210
#define MCT_G_COMP1_U		0x214
#define MCT_G_TCON		0x240
#define MCT_G_INT_CSTAT		0x244
#define MCT_G_INT_ENB		0x248
#define MCT_G_WSTAT		0x24c
#define MCT_TCON_START		(1U << 8)
#define MCT_TCON_COMP0_EN	(1U << 0)
#define MCT_G0_SPI		466
#define MCT_G1_SPI		467 /* Samsung DT mct-map entry 1 */

static void __iomem *mct;
static struct clock_event_device zrn_bc;
static unsigned int zrn_bc_irq = ~0U;
static bool zrn_bc_live;
static unsigned long zrn_mct_hz;
static unsigned long mct_late_events;
module_param(mct_late_events, ulong, 0444);
static void __iomem *gicd, *gicc, *uartbt;
/* log this many broadcast programs and G0 interrupts after each resume */
static unsigned int mct_trace, isr_trace, mct_trace_resume;
module_param(mct_trace_resume, uint, 0644);
static struct hrtimer watch_timer;
static int watch_left, watch_resume;
module_param(watch_resume, int, 0644);
static enum hrtimer_restart zrn_watch(struct hrtimer *t);
static unsigned long g0_irqs;
module_param(g0_irqs, ulong, 0444);
static DEFINE_RAW_SPINLOCK(mct_lock);
static unsigned int alarm_irq = ~0U, alarm_seconds;
static unsigned long alarm_fires;
module_param(alarm_fires, ulong, 0444);

static void mct_wr(u32 v, u32 off, u32 wstat_bit)
{
	u32 stat;

	writel_relaxed(v, mct + off);
	if (readl_poll_timeout_atomic(mct + MCT_G_WSTAT, stat,
				      stat & wstat_bit, 1, 100))
		pr_warn("zrn-idle: MCT write %#x did not land\n", off);
	writel_relaxed(wstat_bit, mct + MCT_G_WSTAT);	/* the status clears on a 1 */
}

static u64 mct_count(void)
{
	u32 hi, hi2, lo;

	hi = readl_relaxed(mct + MCT_G_CNT_U);
	do {
		lo = readl_relaxed(mct + MCT_G_CNT_L);
		hi2 = readl_relaxed(mct + MCT_G_CNT_U);
		if (hi == hi2)
			return ((u64)hi << 32) | lo;
		hi = hi2;
	} while (1);
}

static unsigned long mct_measure_rate(void)
{
	u64 a, b, ns;
	ktime_t t0 = ktime_get();

	a = mct_count();
	while (ktime_to_ns(ktime_sub(ktime_get(), t0)) < 200 * NSEC_PER_MSEC)
		cpu_relax();
	ns = ktime_to_ns(ktime_sub(ktime_get(), t0));
	b = mct_count();
	return ns ? (unsigned long)div64_u64((b - a) * NSEC_PER_SEC, ns) : 0;
}

static void mct_comp0_stop(void)
{
	u32 tcon = readl_relaxed(mct + MCT_G_TCON);

	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) & ~1U, mct + MCT_G_INT_ENB);
	tcon &= ~MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
	/* a match that landed before the stop stays latched; left there, the
	 * firmware sees G0 pending at the next SYS_SLEEP and refuses it */
	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
}

static int zrn_bc_set_next(unsigned long cycles, struct clock_event_device *evt)
{
	u64 comp;
	u32 tcon;
	unsigned long flags;
	int ret = 0;

	raw_spin_lock_irqsave(&mct_lock, flags);
	comp = mct_count() + cycles;
	if (mct_trace) {
		mct_trace--;
		pr_info("zrn-idle: bc set cpu%d cycles %lu comp %llx enb %08x gic en %08x pend %08x tgt %08x\n",
				smp_processor_id(), cycles, comp, readl_relaxed(mct + MCT_G_INT_ENB),
				gicd ? readl_relaxed(gicd + 0x100 + 15 * 4) : 0,
				gicd ? readl_relaxed(gicd + 0x200 + 15 * 4) : 0,
				gicd ? readl_relaxed(gicd + 0x800 + 498 / 4 * 4) : 0);
	}
	tcon = readl_relaxed(mct + MCT_G_TCON);

	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	tcon &= ~MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
	mct_wr((u32)comp, MCT_G_COMP0_L, 1U << 0);
	mct_wr((u32)(comp >> 32), MCT_G_COMP0_U, 1U << 1);
	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) | 1U, mct + MCT_G_INT_ENB);
	tcon |= MCT_TCON_COMP0_EN;
	mct_wr(tcon, MCT_G_TCON, 1U << 16);
	/* MCT compares for equality. If programming took longer than the
	 * requested interval, the event will never arrive. Returning success
	 * leaves tick_broadcast_force_mask set and every CPU polling forever.
	 * Let the clockevent core retry with its minimum-delta backoff. */
	if ((s64)(mct_count() - comp) >= 0) {
		mct_comp0_stop();
		mct_late_events++;
		ret = -ETIME;
	}
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return ret;
}

static int zrn_bc_stop(struct clock_event_device *evt)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&mct_lock, flags);
	if (zrn_bc_live)
		mct_comp0_stop();
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return 0;
}

/*
 * SYS_SLEEP takes the MCT down with the rest of the SoC. On the way back it
 * comes up stopped, so the count freezes; the late-event check above never
 * trips, a frozen count never meets the comparator, and the first broadcast
 * after resume never arrives (measured: hard lockup, cpu1 polling in
 * tick_check_broadcast_expired). Syscore resume runs before the tick resumes
 * and the clockevent core re-arms the comparator, so start the counter here.
 */
static u32 mct_saved_tcon;

/* GIC_DIST: G0 is SPI 466, hwirq 498: enable/pending word 15, bit 18 */
#define GICD_PHYS		0x10101000UL

static int zrn_mct_suspend(void *data)
{
	if (zrn_bc_live)
		mct_saved_tcon = readl_relaxed(mct + MCT_G_TCON);
	return 0;
}

static void zrn_mct_resume(void *data)
{
	u32 tcon;
	u64 a;

	if (!zrn_bc_live)
		return;
	tcon = readl_relaxed(mct + MCT_G_TCON);
	a = mct_count();
	if (!(tcon & MCT_TCON_START))
		mct_wr(tcon | MCT_TCON_START, MCT_G_TCON, 1U << 16);
	writel_relaxed(3, mct + MCT_G_INT_CSTAT);
	pr_info("zrn-idle: MCT on resume: tcon %08x (was %08x) int_enb %08x count %llx -> %llx\n",
		tcon, mct_saved_tcon, readl_relaxed(mct + MCT_G_INT_ENB), a, mct_count());
	if (gicd)
		pr_info("zrn-idle: GIC on resume: ctlr %08x G0 en %08x pend %08x act %08x tgt %08x cfg %08x\n",
			readl_relaxed(gicd), readl_relaxed(gicd + 0x100 + 15 * 4),
			readl_relaxed(gicd + 0x200 + 15 * 4), readl_relaxed(gicd + 0x300 + 15 * 4),
			readl_relaxed(gicd + 0x800 + 498 / 4 * 4), readl_relaxed(gicd + 0xc00 + 498 / 16 * 4));
	mct_trace = mct_trace_resume;
	isr_trace = mct_trace_resume;
	if (watch_resume) {
		watch_left = watch_resume;
		hrtimer_start(&watch_timer, ms_to_ktime(500), HRTIMER_MODE_REL_PINNED);
	}
}

/* After resume, a cpu0-pinned look at the broadcast device once a second:
 * cpu0 keeps its own arch tick, so this still runs when the broadcast is
 * stuck and the other cores have stopped ticking. */
static enum hrtimer_restart zrn_watch(struct hrtimer *t)
{
	pr_info("zrn-idle: watch now %lld bc next %lld state %d forced %u retries %lu min %llu | "
		"count %llx comp %08x%08x tcon %08x enb %08x cstat %08x g0 %lu late %lu\n",
		ktime_to_ns(ktime_get()), ktime_to_ns(zrn_bc.next_event),
		(int)zrn_bc.state_use_accessors, zrn_bc.next_event_forced,
		zrn_bc.retries, zrn_bc.min_delta_ns, mct_count(),
		readl_relaxed(mct + MCT_G_COMP0_U), readl_relaxed(mct + MCT_G_COMP0_L),
		readl_relaxed(mct + MCT_G_TCON), readl_relaxed(mct + MCT_G_INT_ENB),
		readl_relaxed(mct + MCT_G_INT_CSTAT), g0_irqs, mct_late_events);
	if (gicd) {
		struct irq_data *d = irq_get_irq_data(zrn_bc_irq);

		pr_info("zrn-idle: watch gic ctlr %08x en %08x pend %08x act %08x tgt %08x | irq dis %d mask %d prog %d cpu %u\n",
			readl_relaxed(gicd), readl_relaxed(gicd + 0x100 + 15 * 4),
			readl_relaxed(gicd + 0x200 + 15 * 4), readl_relaxed(gicd + 0x300 + 15 * 4),
			readl_relaxed(gicd + 0x800 + 498 / 4 * 4),
			d ? irqd_irq_disabled(d) : -1, d ? irqd_irq_masked(d) : -1,
			d ? irqd_irq_inprogress(d) : -1,
			d ? cpumask_first(irq_data_get_effective_affinity_mask(d)) : 99);
		/* every SPI word with an interrupt both enabled and pending: one
		 * stuck line, or delivery of all SPIs gone */
		for (int w = 1; w < 20; w++) {
			u32 ep = readl_relaxed(gicd + 0x100 + w * 4) & readl_relaxed(gicd + 0x200 + w * 4);

			if (ep)
				pr_info("zrn-idle: watch spi word %d en&pend %08x\n", w, ep);
		}
	}
	if (uartbt)	/* UART_BT, SPI 422: is it the line that never drops? */
		pr_info("zrn-idle: watch uartbt ucon %08x utrstat %08x uerstat %08x ufstat %08x uintp %08x uintsp %08x uintm %08x usi %08x/%08x\n",
			readl_relaxed(uartbt + 0x4), readl_relaxed(uartbt + 0x10),
			readl_relaxed(uartbt + 0x14), readl_relaxed(uartbt + 0x18),
			readl_relaxed(uartbt + 0x30), readl_relaxed(uartbt + 0x34),
			readl_relaxed(uartbt + 0x38), readl_relaxed(uartbt + 0xc4),
			readl_relaxed(uartbt + 0xc8));
	if (--watch_left <= 0)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ms_to_ktime(1000));
	return HRTIMER_RESTART;
}

static const struct syscore_ops zrn_mct_syscore_ops = {
	.suspend = zrn_mct_suspend,
	.resume = zrn_mct_resume,
};
static struct syscore zrn_mct_syscore = { .ops = &zrn_mct_syscore_ops };

/* Comparator 1 is independent of the broadcast clockevent (comparator 0).
 * It supplies an oscillator-backed, NO_SUSPEND test wakeup even when every
 * CPU tick and timekeeping have stopped. The IRQ mapping is in Samsung's
 * firmware DT mct-map: G0=466, G1=467. No alarm is armed at module load. */
static void alarm_stop_locked(void)
{
	writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) & ~2U, mct + MCT_G_INT_ENB);
	mct_wr(readl_relaxed(mct + MCT_G_TCON) & ~BIT(2), MCT_G_TCON, BIT(16));
	writel_relaxed(2, mct + MCT_G_INT_CSTAT);
}

static irqreturn_t alarm_interrupt(int irq, void *unused)
{
	unsigned long flags;

	if (!(readl_relaxed(mct + MCT_G_INT_CSTAT) & 2))
		return IRQ_NONE;
	raw_spin_lock_irqsave(&mct_lock, flags);
	alarm_stop_locked();
	alarm_seconds = 0;
	alarm_fires++;
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	pm_system_wakeup();
	pr_info("zrn-idle: MCT G1 wake alarm fired\n");
	return IRQ_HANDLED;
}

static int alarm_set(const char *value, const struct kernel_param *kp)
{
	unsigned int seconds;
	unsigned long flags;
	u64 target;
	int ret = kstrtouint(value, 0, &seconds);

	if (ret || seconds > 86400)
		return ret ? ret : -EINVAL;
	if (!mct || alarm_irq == ~0U)
		return -ENODEV;
	raw_spin_lock_irqsave(&mct_lock, flags);
	alarm_stop_locked();
	alarm_seconds = seconds;
	if (seconds) {
		target = mct_count() + (u64)seconds * zrn_mct_hz;
		/* Four write-status bits per comparator (L, U, ADD_INCR, pad). */
		mct_wr(lower_32_bits(target), MCT_G_COMP1_L, BIT(4));
		mct_wr(upper_32_bits(target), MCT_G_COMP1_U, BIT(5));
		writel_relaxed(readl_relaxed(mct + MCT_G_INT_ENB) | 2U, mct + MCT_G_INT_ENB);
		mct_wr(readl_relaxed(mct + MCT_G_TCON) | BIT(2), MCT_G_TCON, BIT(16));
	}
	raw_spin_unlock_irqrestore(&mct_lock, flags);
	return 0;
}

static const struct kernel_param_ops alarm_ops = {
	.set = alarm_set,
	.get = param_get_uint,
};
module_param_cb(alarm_seconds, &alarm_ops, &alarm_seconds, 0600);
MODULE_PARM_DESC(alarm_seconds, "one-shot hardware wake alarm, 1..86400 s; 0 cancels");

static void __init zrn_alarm_init(struct irq_fwspec *spec)
{
	int virq, ret;

	if (readl_relaxed(mct + MCT_G_TCON) & BIT(2)) {
		pr_warn("zrn-idle: MCT G1 already active; leaving it alone\n");
		return;
	}
	spec->param[1] = MCT_G1_SPI;
	virq = irq_create_fwspec_mapping(spec);
	if (virq <= 0)
		return;
	ret = request_irq(virq, alarm_interrupt, IRQF_NO_SUSPEND,
			  "zrn-mct-wake", &alarm_seconds);
	if (ret)
		goto dispose;
	ret = irq_set_irq_wake(virq, 1);
	if (ret) {
		free_irq(virq, &alarm_seconds);
		goto dispose;
	}
	irq_set_affinity(virq, cpumask_of(0));
	alarm_irq = virq;
	pr_info("zrn-idle: MCT G1 wake alarm on IRQ %d\n", virq);
	return;
dispose:
	irq_dispose_mapping(virq);
}

static irqreturn_t zrn_bc_isr(int irq, void *dev)
{
	struct clock_event_device *evt = dev;

	/* only comparator 0's flag is ours; a stray line (wrong SPI guess)
	 * shows up as spurious instead of waking cores at random */
	if (!(readl_relaxed(mct + MCT_G_INT_CSTAT) & 1))
		return IRQ_NONE;
	writel_relaxed(1, mct + MCT_G_INT_CSTAT);
	g0_irqs++;
	if (isr_trace) {
		isr_trace--;
		pr_info("zrn-idle: G0 irq on cpu%d count %llx\n",
				smp_processor_id(), mct_count());
	}
	evt->event_handler(evt);
	return IRQ_HANDLED;
}

/*
 * Register the comparator as a global oneshot clockevent. Rating 300: above
 * bc_hrtimer (0), so the framework installs it as the broadcast device, and
 * below the arch timers (~400), so the per-CPU tick devices are untouched.
 */
static int __init zrn_broadcast_init(void)
{
	struct device_node *gic;
	struct irq_fwspec spec = { };
	unsigned long rate;
	u32 tcon;
	int virq, ret;

	mct = ioremap(MCT_PHYS, 0x800);
	if (!mct)
		return -ENOMEM;
	gicd = ioremap(GICD_PHYS, 0x1000);
	gicc = ioremap(GICD_PHYS + 0x1000, 0x1000);	/* CPU interface, banked per core */
	uartbt = ioremap(0x10840000, 0x100);

	tcon = readl_relaxed(mct + MCT_G_TCON);
	if (!(tcon & MCT_TCON_START)) {	/* the counter is not even running */
		mct_wr(tcon | MCT_TCON_START, MCT_G_TCON, 1U << 16);
	}
	rate = mct_measure_rate();
	if (rate < 1000000) {
		pr_err("zrn-idle: MCT not ticking (rate %lu Hz): no broadcast, no C2\n", rate);
		ret = -ENODEV;
		goto err;
	}
	pr_info("zrn-idle: MCT G0 at %lu Hz is the broadcast timer\n", rate);
	zrn_mct_hz = rate;

	gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	if (!gic) {
		ret = -ENODEV;
		goto err;
	}
	spec.fwnode = of_fwnode_handle(gic);
	spec.param_count = 3;
	spec.param[0] = 0;			/* GIC_SPI */
	spec.param[1] = MCT_G0_SPI;
	spec.param[2] = IRQ_TYPE_LEVEL_HIGH;
	of_node_put(gic);
	virq = irq_create_fwspec_mapping(&spec);
	if (virq <= 0) {
		ret = -ENOENT;
		goto err;
	}

	zrn_bc = (struct clock_event_device){
		.name = "zrn-mct-g0",
		.features = CLOCK_EVT_FEAT_ONESHOT,
		.rating = 300,
		.irq = virq,
		.set_next_event = zrn_bc_set_next,
		.set_state_shutdown = zrn_bc_stop,
		.set_state_oneshot = zrn_bc_stop,
		.set_state_oneshot_stopped = zrn_bc_stop,
		.tick_resume = zrn_bc_stop,
		.cpumask = cpumask_of(0),
		.owner = THIS_MODULE,
	};
	ret = request_irq(virq, zrn_bc_isr, IRQF_TIMER | IRQF_IRQPOLL,
			  "zrn-mct-g0", &zrn_bc);
	if (ret)
		goto err_dispose;
	zrn_bc_irq = virq;
	/* Several synchronized MMIO writes precede arming the comparator.
	 * Fifteen cycles (<1 us) can expire before it is enabled. */
	clockevents_config_and_register(&zrn_bc, rate, DIV_ROUND_UP(rate, 5000), UINT_MAX);
	zrn_bc_live = true;
	hrtimer_setup(&watch_timer, zrn_watch, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
	register_syscore(&zrn_mct_syscore);
	zrn_alarm_init(&spec);
	return 0;

err_dispose:
	irq_dispose_mapping(virq);
err:
	iounmap(mct);
	mct = NULL;
	return ret;
}

static void zrn_broadcast_exit(void)
{
	/* clockevents cannot be unregistered from a module. While the comparator
	 * is the broadcast timer the framework holds it (dev->owner), so the
	 * module is pinned and this never runs; it only cleans up a device that
	 * registered but was never installed. */
	if (!zrn_bc_live)
		return;
	unregister_syscore(&zrn_mct_syscore);
	hrtimer_cancel(&watch_timer);
	zrn_bc_live = false;
	if (alarm_irq != ~0U) {
		unsigned long flags;

		raw_spin_lock_irqsave(&mct_lock, flags);
		alarm_stop_locked();
		raw_spin_unlock_irqrestore(&mct_lock, flags);
		irq_set_irq_wake(alarm_irq, 0);
		free_irq(alarm_irq, &alarm_seconds);
		irq_dispose_mapping(alarm_irq);
		alarm_irq = ~0U;
	}
	free_irq(zrn_bc_irq, &zrn_bc);
	irq_dispose_mapping(zrn_bc_irq);
	iounmap(mct);
	mct = NULL;
	if (gicd)
		iounmap(gicd);
	if (gicc)
		iounmap(gicc);
	if (uartbt)
		iounmap(uartbt);
	gicd = gicc = uartbt = NULL;
}

static int __init zrn_idle_init(void)
{
	int ret = 0;

	if (!suspend_fn) {
		pr_err("zrn-idle: give suspend_enter= (cpu_suspend from /proc/kallsyms)\n");
		return -EINVAL;
	}
	if (!resume_fn) {
		pr_err("zrn-idle: give resume= (cpu_resume from /proc/kallsyms)\n");
		return -EINVAL;
	}

	cpu_suspend_p = (void *)suspend_fn;
	if (resume_fn >= 0xffff000000000000UL)
		cpu_resume_pa = __pa_symbol(resume_fn);
	else
		cpu_resume_pa = resume_fn;

	pr_info("zrn-idle: cpu_suspend=%px, cpu_resume=%px (PA 0x%lx)\n",
		cpu_suspend_p, (void *)resume_fn, cpu_resume_pa);

	ret = zrn_broadcast_init();
	if (ret) {
		pr_err("zrn-idle: no broadcast timer: staying out of the kernel's way\n");
		return ret;
	}
	pmu = ioremap(PMU_BASE, 0x8000);
	bid = ioremap(BID_BASE, 0x1000);
	cluster_mailbox = ioremap(0x14100000, 0x1000);
	if (!pmu || !bid || !cluster_mailbox) {
		zrn_idle_exit();
		return -ENOMEM;
	}

	for (int i = 0; i < 2; i++) {
		struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
		unsigned int cpu;

		if (!mask) {
			ret = -ENOMEM;
			break;
		}
		drivers[i].cpumask = mask;
		/* CPUs 0-3 are the little cluster, 4-7 the big (MPIDR 0x00x, 0x10x) */
		for_each_possible_cpu(cpu) {
			if (cpu / 4 == i && (cpus & BIT(cpu)))
				cpumask_set_cpu(cpu, mask);
		}
		if (cpumask_empty(mask))
			continue;
		ret = cpuidle_register(&drivers[i], NULL);
		if (ret) {
			pr_err("zrn-idle: %s: %d\n", drivers[i].name, ret);
			break;
		}
		registered[i] = true;
		pr_info("zrn-idle: %s on CPUs %*pbl\n", drivers[i].name, cpumask_pr_args(mask));
	}
	if (ret)
		zrn_idle_exit();
	return ret;
}

/* ---- shared with zrn-sicd.o, the SICD state of the same module --------------- */

/* which cores are down in C2, for SICD's "every online core" gate (vendor:
 * exynos-powermode.c's c2_mask under c2_lock) */
DEFINE_RAW_SPINLOCK(zrn_c2_lock);
unsigned long zrn_c2_mask[BITS_TO_LONGS(NR_CPUS)];

void zrn_c2_mark(unsigned int core, bool down)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (down)
		set_bit(core, zrn_c2_mask);
	else {
		if (core >= 4 && cluster_down) {
			rmw(cluster_mailbox + 0x10, BIT(24), 0);
			wmb();
			cluster_down = false;
		}
		clear_bit(core, zrn_c2_mask);
	}
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
}

void __iomem *zrn_pmu_base(void) { return pmu; }
void __iomem *zrn_bid_base(void) { return bid; }
int zrn_cpu_suspend_call(unsigned long state, int (*finisher)(unsigned long))
{
	return cpu_suspend_p(state, finisher);
}
unsigned long zrn_cpu_resume_pa(void)
{
	return cpu_resume_pa;
}
u64 zrn_mct_count_now(void)
{
	return mct ? mct_count() : 0;
}
unsigned long zrn_mct_rate_now(void)
{
	return zrn_mct_hz;
}

module_init(zrn_idle_init);
module_exit(zrn_idle_exit);
MODULE_DESCRIPTION("Exynos 9810 per-core power-down idle state with the flexible PMU's hints");
MODULE_LICENSE("GPL");
