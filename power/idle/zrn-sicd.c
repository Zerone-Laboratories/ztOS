// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * zrn-sicd: SICD, Samsung's System Idle Clock Down, for the Exynos 9810.
 *
 * With every core in C2 and nothing due on a timer for a while, Samsung's
 * kernel asks the firmware to clock the whole system down: the PLLs, the
 * memory interface, the buses (the APM's soc/mif sequencers; nothing of that
 * runs in the kernel). It is the one level of idle this SoC has that mainline
 * does not. Actual SoC/MIF sequencer counters, together with wake tests,
 * are required to establish whether this firmware request succeeds.
 *
 * Samsung's way in is not a call of its own: cpuidle state 129,
 * PSCI_SYSTEM_IDLE, packs to the power-state word 0x00000001 (their layout
 * has the state id in bits 0..15, where the stock device tree's per-core
 * power-down is 0x10000), and goes through the same PSCI CPU_SUSPEND call
 * the C2 path already makes (SMC 0xC4000001, entry point cpu_resume). The
 * firmware tells the APM from the word: 0x1 is SICD, 0x10000 a core down.
 *
 * What the kernel writes before the call (exynos-powermode.c, cal-if):
 *
 *   IDLE_IP_MASK0..3   PMU 0x140603f0..	the idle-ip blocks that must be
 *   					idle; ours report nothing (all status
 *  					zero), so `relax=1` masks everything
 *  					out and the check passes
 *   EINT_WAKEUP_MASK   PMU 0x1406060c	which GPIOs may wake (the power
 *   					key: gpa2.4 / EINT20, `eint_wake`)
 *   WAKEUP_MASK(0..3)  PMU 0x14060610..	which interrupts may wake (the
 *   					stock SICD column: 0x400001e0, 0, 0, 0)
 *   CPU_INFORM[core]   PMU 0x14060860..	3, SICD (C2 writes 1 there first)
 *
 * and afterwards nothing: the APM runs its up-sequencers on the wake event
 * and releases the core at cpu_resume. If an interrupt lands before the WFI,
 * the call returns and only GRP4's pending bit needs clearing (0x1407040c).
 *
 * This is not a cpuidle state of its own: enter_powerdown() offers it the
 * moment every online core is already down in C2 (zrn_c2_mask) and the last
 * predicted idle period is at least `residency` us long. The kernel's
 * tick_nohz_get_next_hrtimer address is supplied from kallsyms, like the
 * cpu_suspend entry point. A previous residency cannot predict a deadline.
 *
 *	zrn-cpuidle sicd=1 relax=1	resume=... suspend_enter=... cpus=...
 *
 * Counters (this directory, /sys/module/zrn_cpuidle/parameters/): entries,
 * last_block (why the last attempt did not happen), last_sleep_us (from the
 * MCT, which runs off the oscillator and does not stop for SICD).
 */
#include <linux/arm-smccc.h>
#include <linux/bitmap.h>
#include <linux/cpuidle.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/stdarg.h>
#include <linux/string.h>

#include "zrn-idle.h"

#define PMU_IDLE_IP(n)		(0x03e0 + 4 * (n))
#define PMU_IDLE_IP_MASK(n)	(0x03f0 + 4 * (n))
#define PMU_EINT_WAKEUP_MASK	0x060c
#define GRP4_PEND		0x0408
#define GRP4_CLEAR		0x040c
#define PMU_CPU_INFORM(c)	(0x0860 + 4 * (c))
#define INFORM_SICD		3

/* the stock device tree's SYS_SICD column (twrp-live.dts, exynos-powermode) */
static const u32 stock_idle_ip_mask[4] = { 0x00000000, 0x00000000, 0xff9003c0, 0xfffffffe };
static const u32 wakeup_reg[4] = { 0x0610, 0x0614, 0x0618, 0x0644 };
static const u32 wakeup_mask_sicd[4] = { 0x400001e0, 0, 0, 0 };

/* Samsung's psci_power_state_pack(1, 0, 0) for PSCI_SYSTEM_IDLE (index 129) */
#define PSCI_SYSTEM_IDLE_STATE	0x00000001u

static bool sicd;
module_param(sicd, bool, 0644);
MODULE_PARM_DESC(sicd, "offer SICD when every online core is in C2");
static bool s2idle_sicd;
module_param(s2idle_sicd, bool, 0644);
MODULE_PARM_DESC(s2idle_sicd, "offer SICD only after PM core freezes tasks and suspends devices");
/*
 * s2idle only. Normally only a boot-cluster core (0..3) may commit SICD, so
 * the trial fires only when the core that happens to settle into C2 last is a
 * little one. Under s2idle every wake runs wake_up_all_idle_cpus() and all
 * eight cores re-descend together, so the last-man is effectively random; when
 * it is a big core the gate falls through core>=4 and the all-eight-in-C2
 * window is wasted with no SMC (last_block keeps showing whichever little core
 * was still re-descending, e.g. "cpu0 up"). This lets the true last-man of
 * either cluster issue the SICD SMC with all eight already informed C2, so the
 * boot cluster need not be the one that settles last. Mutually exclusive with
 * s2idle_cpd (cluster power-down): keep s2idle_cpd=0 when this is on.
 */
static bool s2idle_any_committer;
module_param(s2idle_any_committer, bool, 0644);
MODULE_PARM_DESC(s2idle_any_committer,
	"s2idle: let the last-man of either cluster commit SICD (needs s2idle_cpd=0)");

bool zrn_s2idle_any_committer(void) { return READ_ONCE(s2idle_any_committer); }
static bool relax;
module_param(relax, bool, 0644);
MODULE_PARM_DESC(relax, "mask every idle-ip block out (they report nothing on this phone)");
/* Mainline's gpio-keys power button is gpa2.4 (EINT20). Keep the PMIC
 * interrupt (EINT1) and MAX77705 cable/charger line (gpa1.6, EINT14) too. */
static unsigned int eint_wake = 0xffefbffd;
module_param(eint_wake, uint, 0644);
MODULE_PARM_DESC(eint_wake, "EINT_WAKEUP_MASK: power key EINT20, PMIC EINT1, charger EINT14");
static unsigned int residency = 10000;
module_param(residency, uint, 0644);
MODULE_PARM_DESC(residency, "least us of idle before SICD is offered (stock: 10000)");
static unsigned long next_event;
module_param(next_event, ulong, 0444);
MODULE_PARM_DESC(next_event, "address of tick_nohz_get_next_hrtimer (from kallsyms)");
static bool sicd_active; /* protected by zrn_c2_lock */

static unsigned long entries;
module_param(entries, ulong, 0444);
static unsigned long suspend_entries, attempts;
static int last_smc_result;
module_param(last_smc_result, int, 0444);
static unsigned long last_trial_us, max_trial_us;
module_param(last_trial_us, ulong, 0444);
module_param(max_trial_us, ulong, 0444);
static int sicd_cpu = -1;
static unsigned int last_core;
module_param(sicd_cpu, int, 0644);
MODULE_PARM_DESC(sicd_cpu, "limit trials to one boot-cluster CPU; -1 allows all");
module_param(last_core, uint, 0444);
static int last_error;
module_param(suspend_entries, ulong, 0444);
module_param(attempts, ulong, 0444);
module_param(last_error, int, 0444);
static unsigned long last_sleep_us;
module_param(last_sleep_us, ulong, 0444);
static unsigned long blocked_short, blocked_cpu, blocked_ip;
module_param(blocked_short, ulong, 0444);
module_param(blocked_cpu, ulong, 0444);
module_param(blocked_ip, ulong, 0444);
static char last_block[48];
module_param_string(last_block, last_block, sizeof(last_block), 0444);
static DEFINE_RAW_SPINLOCK(diagnostic_lock);

static void blocked(unsigned long *counter, const char *fmt, ...)
{
	va_list ap;
	unsigned long flags;

	raw_spin_lock_irqsave(&diagnostic_lock, flags);
	if (counter)
		(*counter)++;
	va_start(ap, fmt);
	vsnprintf(last_block, sizeof(last_block), fmt, ap);
	va_end(ap);
	raw_spin_unlock_irqrestore(&diagnostic_lock, flags);
}

static int zrn_sicd_finisher(unsigned long state)
{
	struct arm_smccc_res res;

	arm_smccc_smc(0xC4000001, state, zrn_cpu_resume_pa(), 0, 0, 0, 0, 0, &res);
	WRITE_ONCE(last_smc_result, (int)res.a0);
	return (int)res.a0;
}

/*
 * Offered from enter_powerdown(), with this core's C2 bit already set. Takes
 * the SICD path (returns true, *ret as cpu_suspend would give it) or leaves
 * the plain C2 call to the caller (false, with the reason in last_block).
 */
bool zrn_sicd_try(struct cpuidle_device *dev, unsigned int core, bool suspended, int *ret)
{
	void __iomem *pmu = zrn_pmu_base(), *bid = zrn_bid_base();
	unsigned long flags;
	u64 before, after;
	ktime_t predicted;
	u32 saved_idle[4], saved_wake[4], saved_eint;
	int cpu;

	*ret = 0;
	if (suspended ? !READ_ONCE(s2idle_sicd) : !READ_ONCE(sicd))
		return false;			/* not configured in: no reason logged */
	if (core >= 4 && !(suspended && READ_ONCE(s2idle_any_committer)))
		return false;		/* SICD is entered from the boot cluster,
					   unless s2idle_any_committer lets the
					   last-man of either cluster commit it */
	if (READ_ONCE(sicd_cpu) >= 0 && core != READ_ONCE(sicd_cpu))
		return false;
	if (!suspended && !next_event) {
		blocked(NULL, "next_event unavailable");
		return false;
	}
	/* Read the programmed deadline; the governor-only sleep-length API
	 * mutates tick state and must not run after tick_nohz_idle_stop_tick. */
	if (!suspended) {
		predicted = ktime_sub(((ktime_t (*)(void))next_event)(), ktime_get());
		if (predicted < (s64)READ_ONCE(residency) * NSEC_PER_USEC) {
			blocked(&blocked_short, "short %lld us", ktime_to_us(predicted));
			return false;
		}
	}

	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	if (sicd_active) {
		raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
		return false;
	}
	for_each_online_cpu(cpu) {
		if (!test_bit(cpu, zrn_c2_mask)) {
			raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
			blocked(&blocked_cpu, "cpu%d up", cpu);
			return false;
		}
	}
	/* Runtime idle needs per-device IDLE_IP reporters. The system-suspend
	 * callback instead runs only after the PM core has suspended devices,
	 * frozen userspace/freezable workers and disabled non-wakeup IRQs.
	 * Do not infer this quiescent state from a userspace screen flag. */
	if (!suspended && !relax) {
		/* every idle-ip block the mask leaves open must say idle */
		for (int n = 0; n < 4; n++) {
			u32 val = readl_relaxed(pmu + PMU_IDLE_IP(n));

			if ((val & ~stock_idle_ip_mask[n]) != ~stock_idle_ip_mask[n]) {
				raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
				blocked(&blocked_ip, "idleip%d=%#x", n, val);
				return false;
			}
		}
	}
	sicd_active = true;
	for (int n = 0; n < 4; n++) {
		saved_idle[n] = readl_relaxed(pmu + PMU_IDLE_IP_MASK(n));
		saved_wake[n] = readl_relaxed(pmu + wakeup_reg[n]);
		writel_relaxed((suspended || relax) ? ~0u : stock_idle_ip_mask[n], pmu + PMU_IDLE_IP_MASK(n));
	}
	saved_eint = readl_relaxed(pmu + PMU_EINT_WAKEUP_MASK);
	writel_relaxed(eint_wake, pmu + PMU_EINT_WAKEUP_MASK);
	for (int n = 0; n < 4; n++)
		writel_relaxed(wakeup_mask_sicd[n], pmu + wakeup_reg[n]);
	writel_relaxed(INFORM_SICD, pmu + PMU_CPU_INFORM(core));
	attempts++;
	last_core = core;
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	wmb();

	before = zrn_mct_count_now();
	*ret = zrn_cpu_suspend_call(PSCI_SYSTEM_IDLE_STATE, zrn_sicd_finisher);
	after = zrn_mct_count_now();
	raw_spin_lock_irqsave(&zrn_c2_lock, flags);
	last_error = *ret;
	if (zrn_mct_rate_now()) {
		last_trial_us = div64_u64((after - before) * 1000000ul, zrn_mct_rate_now());
		max_trial_us = max(max_trial_us, last_trial_us);
	}
	if (*ret == 0) {
		entries++;
		if (suspended)
			suspend_entries++;
		last_sleep_us = last_trial_us;
	} else if (readl_relaxed(bid + GRP4_PEND) & 1) {
		/* an interrupt before the WFI: the vendor clears GRP4's pending bit */
		writel_relaxed(1, bid + GRP4_CLEAR);
	}
	/* Do not leave a trial's wake policy installed after resume or abort. */
	for (int n = 0; n < 4; n++) {
		writel_relaxed(saved_idle[n], pmu + PMU_IDLE_IP_MASK(n));
		writel_relaxed(saved_wake[n], pmu + wakeup_reg[n]);
	}
	writel_relaxed(saved_eint, pmu + PMU_EINT_WAKEUP_MASK);
	wmb();
	sicd_active = false;
	raw_spin_unlock_irqrestore(&zrn_c2_lock, flags);
	return true;
}
