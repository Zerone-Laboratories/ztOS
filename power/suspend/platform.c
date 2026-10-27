// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9810 SYS_SLEEP backend. Default load only maps and validates tables. */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/suspend.h>
#include <asm/memory.h>
#include "sequence.h"
#include "sleep-tables.h"
#include "init-memory.h"

static bool enable_deep, dry_run = true, usbl2, probe_save;
static bool prepare_only = true;
static bool mif_sleep;
static unsigned int init_changes, init_verified;
module_param(init_changes, uint, 0444);
module_param(init_verified, uint, 0444);
static unsigned int mif_gate_before, mif_requests_before;
module_param(mif_gate_before, uint, 0444);
module_param(mif_requests_before, uint, 0444);
module_param(prepare_only, bool, 0600);
MODULE_PARM_DESC(prepare_only, "prepare and restore inside saved CPU context without SMC");
static unsigned long suspend_enter, resume;
static unsigned int eint_wake = 0xffefbffd;
module_param(enable_deep, bool, 0444);

module_param(probe_save, bool, 0444);
module_param(suspend_enter, ulong, 0400);
module_param(resume, ulong, 0400);
/* On: PSCI 1.0 SYSTEM_SUSPEND (0xc400000e, x1 = physical resume address),
 * which stock's psci_system_suspend issues (found with REA). This phone's
 * firmware is PSCI 0.1 (the DT lists cpu_suspend 0xc4000001 and no
 * system_suspend) and answers it NOT_SUPPORTED (-1). Off, the default:
 * CPU_SUSPEND with the SYS_SLEEP state 0x03000000, as stock's index 133. */
static bool psci_system;
module_param(psci_system, bool, 0444);
module_param(eint_wake, uint, 0600);
MODULE_PARM_DESC(enable_deep, "register the platform backend (pins module until reboot)");
MODULE_PARM_DESC(dry_run, "refuse the firmware call and leave PMUCAL registers untouched");
static int set_dry_run(const char *value, const struct kernel_param *parameter)
{
	unsigned int flags = lock_system_sleep();
	int ret = param_set_bool(value, parameter);
	unlock_system_sleep(flags);
	return ret;
}
static const struct kernel_param_ops dry_run_ops = {
	.set = set_dry_run, .get = param_get_bool,
};
module_param_cb(dry_run, &dry_run_ops, &dry_run, 0600);
module_param_cb(usbl2, &dry_run_ops, &usbl2, 0600);
module_param_cb(mif_sleep, &dry_run_ops, &mif_sleep, 0600);
MODULE_PARM_DESC(mif_sleep, "permit MIF shutdown only at the final saved-context SMC handoff");

struct region { u32 physical; void __iomem *mapped; };
static struct region regions[128];
static unsigned int region_count, last_index;
static int last_error, last_smc, last_restore_error;
static unsigned long attempts, entries;
static unsigned int wake_stat, wake_stat2, wake_stat3, bid_pending;
module_param(wake_stat, uint, 0444);
module_param(wake_stat2, uint, 0444);
module_param(wake_stat3, uint, 0444);
module_param(bid_pending, uint, 0444);
module_param(region_count, uint, 0444);
module_param(last_index, uint, 0444);
module_param(last_error, int, 0444);
module_param(last_smc, int, 0444);
module_param(last_restore_error, int, 0444);
module_param(attempts, ulong, 0444);
module_param(entries, ulong, 0444);

struct table { struct zrn_seq *seq; unsigned int count; };
#define TABLE(n) { n, ARRAY_SIZE(n) }
static struct table modes[2][4] = {
	{ TABLE(enter_sleep), TABLE(save_sleep), TABLE(exit_sleep), TABLE(earlywkup_sleep) },
	{ TABLE(enter_sleep_usbl2), TABLE(save_sleep_usbl2), TABLE(exit_sleep_usbl2), TABLE(earlywkup_sleep_usbl2) },
};
static const u32 wake_offsets[] = { 0x610, 0x614, 0x618, 0x644 };
static u32 saved_wake[4], saved_idle[4], saved_eint, saved_eint2, saved_hint;
static bool prepared, early;
/* Boot setup configures USI outside the vendor clock save tables. Preserve
 * its I2C selection, reset and clock-request state across domain reset. */
static const u32 usi_base[] = { 0x10410000, 0x14210000, 0x104b0000, 0x104b0000, 0x14360000, 0x14360000 };
static const u32 usi_offset[] = { 0x101c, 0x2030, 0xc8, 0xc4, 0xc8, 0xc4 };
static u32 saved_usi[ARRAY_SIZE(usi_base)];
static u32 saved_eint_mask[5], saved_wdt0[3];
static unsigned int pending_before[16], pending_after[16];
static unsigned int pending_count = 16;
module_param_array(pending_before, uint, &pending_count, 0444);
module_param_array(pending_after, uint, &pending_count, 0444);
static const unsigned int eint_offset[] = { 0, 4, 8, 12, 36 };
static unsigned int eint_before[5];
static unsigned int eint_pending[5];
/* PERIC1's UART_BT gates and the UART's USI_OPTION, read at the firmware
 * call: what Bluetooth left open (mapped at load; no ioremap in suspend) */
static void __iomem *dbg_peric1, *dbg_uartbt, *dbg_mct;
static u32 dbg_gate[4], dbg_option, dbg_qch, dbg_uart[6], dbg_mct_regs[3];
static unsigned int eint_before_count = 5;
module_param_array(eint_before, uint, &eint_before_count, 0444);
static unsigned int eint_pending_count = ARRAY_SIZE(eint_pending);
module_param_array(eint_pending, uint, &eint_pending_count, 0444);

/* The TOP-domain pin controllers lose their registers in SYS_SLEEP and come
 * back at reset values (input, pull-down). The exit table releases pad
 * retention right after the firmware call, but mainline pinctrl only restores
 * the banks in noirq resume, some 200 ms later: in between every output pin
 * drops. The PCAL6524's RESETn (gpg3-6) did, and the expander came back
 * reset, Bluetooth's BT_EN with it. Stock restores pinctrl in syscore resume,
 * before PMUCAL releases the pads; do the same here: CON, DAT, PUD and DRV of
 * each bank back before the release. */
static const struct { u32 base; unsigned int banks; } gpio_banks[] = {
	{ 0x10430000, 7 },	/* PERIC0: gpp0-3, gpg0-2 */
	{ 0x10830000, 7 },	/* PERIC1: gpp4-6, gpc0-1, gpd0, gpg3 */
	{ 0x11050000, 1 },	/* FSYS0: gpf0 */
	{ 0x11430000, 2 },	/* FSYS1: gpf1-2 */
};
static u32 saved_gpio[ARRAY_SIZE(gpio_banks)][7][4];
static bool restore_gpio = true;
module_param(restore_gpio, bool, 0644);

static void __iomem *mapped_base(u32 base);

static void gpio_save(void)
{
	for (int c = 0; c < ARRAY_SIZE(gpio_banks); c++)
		for (int b = 0; b < gpio_banks[c].banks; b++)
			for (int r = 0; r < 4; r++)
				saved_gpio[c][b][r] = readl_relaxed(mapped_base(gpio_banks[c].base) + b * 0x20 + r * 4);
}

static void gpio_restore(void)
{
	/* DAT before CON, so a pin that turns back into an output starts at its
	 * level rather than at the reset value's 0 */
	static const int order[] = { 1, 2, 3, 0 };

	for (int c = 0; c < ARRAY_SIZE(gpio_banks); c++)
		for (int b = 0; b < gpio_banks[c].banks; b++)
			for (int i = 0; i < 4; i++)
				writel_relaxed(saved_gpio[c][b][order[i]],
					       mapped_base(gpio_banks[c].base) + b * 0x20 + order[i] * 4);
	wmb();
}

static void __iomem *mapped_base(u32 base)
{
	unsigned int i;
	for (i = 0; i < region_count; i++)
		if (regions[i].physical == base)
			return regions[i].mapped;
	return NULL; /* all addresses are validated and mapped at initialization */
}

static int map_base(u32 base)
{
	void __iomem *mapped;
	if (!base || mapped_base(base))
		return 0;
	if (region_count == ARRAY_SIZE(regions) || (base & 0xffff))
		return -EINVAL;
	mapped = ioremap(base, 0x10000);
	if (!mapped)
		return -ENOMEM;
	regions[region_count++] = (struct region) { base, mapped };
	return 0;
}

static u32 register_read(void *context, u32 base, u32 offset)
{
	return readl_relaxed(mapped_base(base) + offset);
}

static void register_write(void *context, u32 base, u32 offset, u32 value)
{
	writel_relaxed(value, mapped_base(base) + offset);
}

static void seq_delay(void *context, unsigned int usec) { udelay(usec); }
static const struct zrn_seq_io io = {
	.read = register_read, .write = register_write, .delay_us = seq_delay,
};

static int run_table(unsigned int index, enum zrn_seq_phase phase)
{
	struct table *table = &modes[usbl2][index];
	int ret = zrn_sequence_run(table->seq, table->count, phase, &io, &last_index);
	if (ret)
		pr_err("zrn-system-sleep: table=%u index=%u register=%s error=%d\n",
		       index, last_index, table->seq[last_index].name, ret);
	return ret;
}

static void restore_platform(void)
{
	void __iomem *pmu = mapped_base(0x14060000);
	int ret, i;
	if (!prepared)
		return;
	if (restore_gpio && !early)
		gpio_restore();	/* before the exit table lets the pads go */
	ret = run_table(early ? 3 : 2, ZRN_HANDLE);
	last_restore_error = run_table(1, ZRN_RESTORE);
	if (!last_restore_error)
		last_restore_error = ret;
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		writel_relaxed(saved_wake[i], pmu + wake_offsets[i]);
		writel_relaxed(saved_idle[i], pmu + 0x3f0 + 4 * i);
	}
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++)
		writel_relaxed(saved_eint_mask[i], mapped_base(0x14050000) + 0x900 + eint_offset[i]);
	/* The unbound cluster-0 watchdog must retain its boot-disabled state. */
	writel_relaxed(0, mapped_base(0x10050000));
	writel_relaxed(saved_wdt0[1], mapped_base(0x10050000) + 4);
	writel_relaxed(saved_wdt0[2], mapped_base(0x10050000) + 8);
	writel_relaxed(saved_wdt0[0], mapped_base(0x10050000));
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		writel_relaxed(saved_usi[i], mapped_base(usi_base[i]) + usi_offset[i]);
	writel_relaxed(saved_eint, pmu + 0x60c);
	writel_relaxed(saved_eint2, pmu + 0x61c);
	writel_relaxed(saved_hint, pmu + 0x860);
	wmb();
	prepared = false;
}

static int prepare_platform(void)
{
	void __iomem *pmu = mapped_base(0x14060000);
	int ret, i;
	if (pm_suspend_target_state != PM_SUSPEND_MEM || dry_run)
		return 0;
	if (num_online_cpus() != 1 || raw_smp_processor_id() != 0)
		return -EBUSY;
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		saved_usi[i] = readl_relaxed(mapped_base(usi_base[i]) + usi_offset[i]);
	gpio_save();
	for (i = 0; i < 3; i++)
		saved_wdt0[i] = readl_relaxed(mapped_base(0x10050000) + i * 4);
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		saved_eint_mask[i] = readl_relaxed(mapped_base(0x14050000) + 0x900 + eint_offset[i]);
		writel_relaxed(saved_eint_mask[i] | (i == 4 ? 0xff : ((eint_wake >> (i * 8)) & 0xff)), mapped_base(0x14050000) + 0x900 + eint_offset[i]);
	}
	saved_eint = readl_relaxed(pmu + 0x60c);
	saved_eint2 = readl_relaxed(pmu + 0x61c);
	saved_hint = readl_relaxed(pmu + 0x860);
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		saved_wake[i] = readl_relaxed(pmu + wake_offsets[i]);
		saved_idle[i] = readl_relaxed(pmu + 0x3f0 + 4 * i);
		/* Stock SYS_SLEEP idle-IP masks are all ones. RTC alarm is
		 * wake-status bit1 (SPI31); retain the independent timed wake. */
		writel_relaxed(~0U, pmu + 0x3f0 + 4 * i);
		writel_relaxed(i == 0 ? ((usbl2 ? 0xd00d7e7e : 0xd00f7e7e) & ~BIT(1)) :
			       (i == 3 ? 0 : 0xffff00ff), pmu + wake_offsets[i]);
	}
	writel_relaxed(eint_wake, pmu + 0x60c);
	writel_relaxed(~0U, pmu + 0x61c);
	writel_relaxed(usbl2 ? 5 : 4, pmu + 0x860);
	prepared = true;
	early = true;
	last_restore_error = 0;
	ret = run_table(1, ZRN_SAVE);
	if (!ret)
		ret = run_table(0, ZRN_HANDLE);
	wmb();
	if (ret)
		restore_platform();
	return ret;
}

/* The live ACPM image copies debug MIF_en (0x95c) into its runtime
 * enable field: a zero bypasses MIF sequencing (Thumb offsets 0x9190,
 * 0x949a, 0x93e0). Hold zero throughout Linux's suspend preparation.
 * No firmware request counts, owner flags or sequencer state are edited. */
static void hold_mif(void)
{
	writel_relaxed(0, mapped_base(0x02040000) + 0xf95c);
	dsb(sy);
}

static int begin_platform(void)
{
	mif_gate_before = readl_relaxed(mapped_base(0x02040000) + 0xf95c);
	if (mif_gate_before > 1)
		return -EINVAL;
	hold_mif();
	return 0;
}

static int sleep_finisher(unsigned long state)
{
	register unsigned long a0 asm("x0");
	register unsigned long a1 asm("x1");
	register unsigned long a2 asm("x2");
	register unsigned long a3 asm("x3");
	register unsigned long a4 asm("x4");
	register unsigned long a5 asm("x5");
	register unsigned long a6 asm("x6");
	register unsigned long a7 asm("x7");
	void __iomem *gate = mapped_base(0x02040000) + 0xf95c;
	unsigned long enable = mif_sleep;
	int i, ret = prepare_platform();
	if (ret)
		return ret;
	if (prepare_only) {
		restore_platform();
		return -EAGAIN;
	}
	/* Discard only latched events from explicitly masked diagnostic sources;
	 * preserve the power-key latch and all enabled wake sources. */
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		u32 mask = i == 4 ? 0xff : ((eint_wake >> (i * 8)) & 0xff);
		eint_before[i] = readl_relaxed(mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
		writel_relaxed(eint_before[i] & mask, mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	}
	wmb();
	for (i = 0; i < 16; i++)
		pending_before[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	mif_requests_before = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
	/* Keep this before enabling the MIF gate: retained console records must
	 * distinguish a missing AP request from corruption of earlier log text. */
	pr_info("zrn-system-sleep: handoff mif_sleep=%u requests=%02x gate=%u\n",
		mif_sleep, mif_requests_before,
		readl_relaxed(mapped_base(0x02040000) + 0xf95c));
	/* Keep the enable/SMC/abort-disable instructions in one cache line.
	 * x19 is preserved by the secure-call ABI. Do not call or delay in
	 * the interval where ACPM is permitted to remove DRAM power. */
	a0 = psci_system ? 0xc400000eUL : 0xc4000001UL;
	a1 = psci_system ? resume : state;
	a2 = psci_system ? 0 : resume;
	a3 = a4 = a5 = a6 = a7 = 0;
	asm volatile(
		"mov x19, %8\n"
		".balign 64\n"
		"str %w9, [x19]\n"
		"dsb sy\n"
		"smc #0\n"
		"str wzr, [x19]\n"
		"dsb sy\n"
		: "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
		  "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7)
		: "r"(gate), "r"(enable)
		: "x19", "x8", "x9", "x10", "x11", "x12", "x13",
		  "x14", "x15", "x16", "x17", "memory");
	last_smc = (int)a0;
	return last_smc;
}

static int enter_sleep_state(suspend_state_t state)
{
	int (*suspend_fn)(unsigned long, int (*)(unsigned long)) = (void *)suspend_enter;
	int ret, i;
	if (dry_run) {
		pr_info("zrn-system-sleep: dry run reached platform enter; no firmware call\n");
		return -EAGAIN;
	}
	ret = cpu_pm_enter();
	if (ret)
		return ret;
	ret = cpu_cluster_pm_enter();
	if (ret)
		goto cpu_exit;
	attempts++;
	if (dbg_peric1 && dbg_uartbt) {
		static const u32 g[] = { 0x20c4, 0x20c8, 0x201c, 0x2034 };
		for (i = 0; i < 4; i++)
			dbg_gate[i] = readl_relaxed(dbg_peric1 + g[i]);
		dbg_qch = readl_relaxed(dbg_peric1 + 0x3028);
		dbg_option = readl_relaxed(dbg_uartbt + 0xc8);
		dbg_uart[0] = readl_relaxed(dbg_uartbt + 0x0);	/* ULCON */
		dbg_uart[1] = readl_relaxed(dbg_uartbt + 0x4);	/* UCON */
		dbg_uart[2] = readl_relaxed(dbg_uartbt + 0xc);	/* UMCON */
		dbg_uart[3] = readl_relaxed(dbg_uartbt + 0x18);	/* UFSTAT */
		dbg_uart[4] = readl_relaxed(dbg_uartbt + 0x30);	/* UINTP */
		dbg_uart[5] = readl_relaxed(dbg_uartbt + 0x38);	/* UINTM */
	}
	if (dbg_mct) {	/* the broadcast comparator: a pending G0 refuses the sleep */
		dbg_mct_regs[0] = readl_relaxed(dbg_mct + 0x240);	/* G_TCON */
		dbg_mct_regs[1] = readl_relaxed(dbg_mct + 0x248);	/* G_INT_ENB */
		dbg_mct_regs[2] = readl_relaxed(dbg_mct + 0x244);	/* G_INT_CSTAT */
	}
	ret = suspend_fn(0x03000000, sleep_finisher);
	hold_mif();
	last_error = ret;
	for (i = 0; i < 16; i++)
		pending_after[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	wake_stat = readl_relaxed(mapped_base(0x14060000) + 0x600);
	wake_stat2 = readl_relaxed(mapped_base(0x14060000) + 0x604);
	wake_stat3 = readl_relaxed(mapped_base(0x14060000) + 0x608);
	bid_pending = readl_relaxed(mapped_base(0x14070000) + 0x408);
	for (i = 0; i < ARRAY_SIZE(eint_pending); i++)
		eint_pending[i] = readl_relaxed(mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	early = ret != 0;
	if (!ret)
		entries++;
	/* Into the console log at once: a device resume that hangs afterward
	 * (UFS did) loses the module parameters, ramoops keeps this. */
	pr_info("zrn-system-sleep: smc=%d wake=%08x/%08x/%08x bid=%08x eint=%08x %08x %08x %08x %08x\n",
		ret, wake_stat, wake_stat2, wake_stat3, bid_pending,
		eint_pending[0], eint_pending[1], eint_pending[2], eint_pending[3], eint_pending[4]);
	pr_info("zrn-system-sleep: at entry: UART_BT gates %08x %08x %08x %08x qch %08x option %08x\n",
		dbg_gate[0], dbg_gate[1], dbg_gate[2], dbg_gate[3], dbg_qch, dbg_option);
	pr_info("zrn-system-sleep: at entry: UART_BT ULCON %08x UCON %08x UMCON %08x UFSTAT %08x UINTP %08x UINTM %08x\n",
		dbg_uart[0], dbg_uart[1], dbg_uart[2], dbg_uart[3], dbg_uart[4], dbg_uart[5]);
	pr_info("zrn-system-sleep: at entry: MCT tcon %08x int_enb %08x cstat %08x\n",
		dbg_mct_regs[0], dbg_mct_regs[1], dbg_mct_regs[2]);
	for (i = 0; i < 16; i++)
		if (pending_after[i])
			pr_info("zrn-system-sleep: GIC pending word %d = %08x (SPI %d..)\n",
				i, pending_after[i], i * 32 - 32);
	/* Undo PMUCAL before CPU PM notifiers touch restored clocks. */
	restore_platform();
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
	return ret;
}
static const struct platform_suspend_ops platform_ops = {
	.valid = suspend_valid_only_mem, .prepare = begin_platform, .enter = enter_sleep_state,
};

static void unmap_all(void)
{
	while (region_count)
		iounmap(regions[--region_count].mapped);
}

/* Samsung's low-power init is distinct from its save/enter/exit tables.
 * Apply the audited subset once while CPUs and DRAM are fully awake. */
static int initialize_sleep_defaults(void)
{
	u32 original[ARRAY_SIZE(init_settings)];
	unsigned int i, j;
	for (i = 0; i < ARRAY_SIZE(init_settings); i++) {
		const struct zrn_init_setting *setting = &init_settings[i];
		void __iomem *reg = mapped_base(setting->base) + setting->offset;
		u32 old = readl_relaxed(reg);
		u32 value = (old & ~setting->mask) | setting->value;
		u32 required = setting->mask | setting->value;
		/* For MIF SYSREG DRCG, verify observed set bits and bit12; reserved bits
		 * read zero even after Samsung's all-ones write. Live S9
		 * readback is 0x80000e00, with the vendor-required bit12 clear. */
		if (setting->offset == 0x104 &&
		    (setting->base == 0x1b810000 || setting->base == 0x1b910000 ||
		     setting->base == 0x1ba10000 || setting->base == 0x1bb10000))
			required = 0x80001e00;
		original[i] = old;
		if (value != old) {
			writel_relaxed(value, reg);
			init_changes++;
		}
		if ((readl_relaxed(reg) & required) != (setting->value & required)) {
			pr_err("zrn-system-sleep: init verification failed: %s read=%08x expected=%08x mask=%08x\n", setting->name, readl_relaxed(reg), setting->value, required);
			for (j = i + 1; j > 0; j--) {
				const struct zrn_init_setting *undo = &init_settings[j - 1];
				writel_relaxed(original[j - 1], mapped_base(undo->base) + undo->offset);
			}
			dsb(sy);
			return -EIO;
		}
		init_verified++;
	}
	dsb(sy);
	pr_info("zrn-system-sleep: initialized %u Samsung prerequisites, changed %u registers\n",
		init_verified, init_changes);
	return 0;
}

static int __init system_sleep_init(void)
{
	unsigned int mode, table, index;
	int ret;
	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;
	if (probe_save && enable_deep)
		return -EINVAL;
#ifdef ZRN_TABLE_CHECK_ONLY
	if (enable_deep || !probe_save)
		return -EINVAL;
#endif
	if (enable_deep && (!suspend_enter || !resume))
		return -EINVAL;
	if (resume >= 0xffff000000000000UL)
		resume = __pa_symbol(resume);
	dbg_peric1 = ioremap(0x10800000, 0x4000);
	dbg_uartbt = ioremap(0x10840000, 0x100);
	dbg_mct = ioremap(0x10040000, 0x800);
	for (mode = 0; mode < ARRAY_SIZE(modes); mode++)
		for (table = 0; table < ARRAY_SIZE(modes[0]); table++)
			for (index = 0; index < modes[mode][table].count; index++) {
				struct zrn_seq *s = &modes[mode][table].seq[index];
				if (!s->base || s->offset >= 0x10000 ||
				    s->cond_offset >= 0x10000) {
					ret = -EINVAL;
					goto fail;
				}
				ret = map_base(s->base);
				if (!ret)
					ret = map_base(s->cond_base);
				if (ret)
					goto fail;
			}
	for (index = 0; index < ARRAY_SIZE(usi_base); index++) {
		ret = map_base(usi_base[index]);
		if (ret)
			goto fail;
	}
	for (index = 0; index < ARRAY_SIZE(init_settings); index++) {
		ret = map_base(init_settings[index].base);
		if (ret)
			goto fail;
	}
	ret = map_base(0x02040000);
	if (ret)
		goto fail;
	ret = map_base(0x10100000);
	if (ret)
		goto fail;
	ret = map_base(0x10050000);
	if (ret)
		goto fail;
	ret = map_base(0x14050000);
	if (ret)
		goto fail;
	for (index = 0; index < ARRAY_SIZE(gpio_banks); index++) {
		ret = map_base(gpio_banks[index].base);
		if (ret)
			goto fail;
	}
	pr_info("zrn-system-sleep: validated sleep tables, mapped %u regions; enabled=%d dry_run=%d\n",
		region_count, enable_deep, dry_run);
	if (probe_save) {
		unsigned int saved = 0;
		ret = run_table(1, ZRN_SAVE);
		if (ret)
			goto fail;
		for (index = 0; index < modes[usbl2][1].count; index++)
			saved += modes[usbl2][1].seq[index].valid;
		pr_info("zrn-system-sleep: read-only save probe completed, %u registers captured; no restore or writes\n",
			saved);
	}
	if (enable_deep) {
		/* suspend_set_ops() has no module owner and does not remove the
		 * advertised deep state on unregister. Pin until reboot. */
		hold_mif();
		ret = initialize_sleep_defaults();
		if (ret)
			goto fail;
		__module_get(THIS_MODULE);
		suspend_set_ops(&platform_ops);
	}
	return 0;
fail:
	unmap_all();
	return ret;
}
static void __exit system_sleep_exit(void)
{
	if (dbg_peric1)
		iounmap(dbg_peric1);
	if (dbg_uartbt)
		iounmap(dbg_uartbt);
	if (dbg_mct)
		iounmap(dbg_mct);
	unmap_all();
}
module_init(system_sleep_init);
module_exit(system_sleep_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exynos9810 SYS_SLEEP platform suspend backend");
