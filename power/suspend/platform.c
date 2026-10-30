// SPDX-License-Identifier: GPL-2.0-only
/* Exynos9810 SYS_SLEEP backend. Default load only maps and validates tables. */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <asm/memory.h>
#include "sequence.h"
#include "sleep-tables.h"
#include "init-memory.h"
#include "wdt-resume.h"

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
/* PMU_INTR_GEN GRP4 (0x14070408): ACPM's flexpmu reads it after the SoC
 * down sequence and gives up the sleep ("upend_wk") while it is not zero.
 * grp4_before is the word at the SMC; clear_grp4 clears what was latched. */
static unsigned int grp4_before, grp4_cleared;
static bool clear_grp4;
module_param(grp4_before, uint, 0444);
module_param(grp4_cleared, uint, 0444);
module_param(clear_grp4, bool, 0600);
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

/* What a TOP pin does while the SoC sleeps: CON_PDN (+0x10, 2 bits a pin:
 * 0 out 0, 1 out 1, 2 input, 3 hold the level it had) and PUD_PDN (+0x14,
 * 4 bits a pin: 0 none, 1 down, 3 up). SYS_SLEEP puts these banks into
 * power-down mode before ACPM's GRP4 check, so even a sleep the firmware
 * gives up runs on them. Left at the reset values (input, pull-down), pins
 * that power a chip let it go: gpg0-2, GPS_PWR_EN, and the BCM4773 sensor
 * hub came back from every sleep in ROM, without its patch (lhd's
 * OnCommunicationError, ESW:CRASHED on the next use; 29 Oct, 9 of 9).
 * Stock's DT holds them (samsung,pin-con-pdn = <3>). Mainline pinctrl saves
 * and restores these registers across suspend, and gpio_restore() above
 * touches only CON/DAT/PUD/DRV, so a write at load lasts the boot.
 * Bluetooth's own two pins stay in zrn-bt. One row a pin; later stages only
 * add rows. Not here, on purpose: gpg0-5 (WL_REG_ON: Wi-Fi is unbound for
 * every sleep), gpg1-0 (codec reset), gpg2-0 and gpg3-6 (zrn-bt's). */
static const struct pdn_pin {
	u32 base;		/* pin controller, one of gpio_banks[] */
	u8 bank, pin;		/* bank index in it (0x20 apart), pin in the bank */
	u8 con, pud;		/* CON_PDN, PUD_PDN */
	const char *name;
} pdn_pins[] = {
	{ 0x10430000, 4, 2, 3, 0, "gpg0-2" },	/* GPS_PWR_EN: the sensor hub's power */
	{ 0x10430000, 4, 1, 3, 0, "gpg0-1" },	/* pm-wrsti: the PMIC's WRSTBI, as stock */
};
static bool pdn = true;
module_param(pdn, bool, 0444);
MODULE_PARM_DESC(pdn, "apply the pin power-down table at load (0: leave the pins as found)");
/* bit 8 x bank + pin of PERIC0 (gpg0-2: bit 34) for each row applied */
static unsigned long pdn_applied;
/* PMU CHUB_STATUS (0x14064084) at the SMC: is the hub's block up */
static unsigned int chub_status_before;

static int param_get_hex(char *buffer, const struct kernel_param *parameter)
{
	return sysfs_emit(buffer, "%#lx\n", *(unsigned long *)parameter->arg);
}
static int param_get_hex32(char *buffer, const struct kernel_param *parameter)
{
	return sysfs_emit(buffer, "%#010x\n", *(unsigned int *)parameter->arg);
}
static const struct kernel_param_ops hex_ops = { .get = param_get_hex };
static const struct kernel_param_ops hex32_ops = { .get = param_get_hex32 };
module_param_cb(pdn_applied, &hex_ops, &pdn_applied, 0444);
module_param_cb(chub_status_before, &hex32_ops, &chub_status_before, 0444);

/* The sensor hub's host_req (gpa1-7, EINT bit 15) is masked by eint_wake
 * (zrn-sleepd's 0xffe7bffd). Stock's EINT wake mask follows enable_irq_wake
 * (exynos_wkup_irq_set_wake, 0x3e8e20 in stock's Image, clears the bit in
 * the mask that exynos-powermode.c writes to PMU 0x60c), and bcm_gps_spi
 * wake-enables host_req while lhd holds /dev/ttyBCM: on stock the hub wakes
 * the AP. Ours slept through a pending host_req while GNSS ran, and the hub
 * stopped answering 5 s after the wake (MCU_REQ_RESP timeout, J0c and J0d).
 * eint_follow: the EINT bits (8 x gpaN + pin, gpa0-3) whose wake follows
 * their IRQ's wake setting, as on stock. Resolved to IRQs at load; a bit
 * written in later that was not resolved then is left to eint_wake. While a
 * followed line is high (the hub is talking) the sleep is refused with
 * EBUSY: stock never sleeps through a pending host_req, as it would wake at
 * once. A latch with the line low is stale (the IRQ is level-triggered and
 * bcm_spi_suspend disables it with disable_irq_nosync): it is cleared, and a
 * new request then wakes the AP through the open bit. */
static unsigned int eint_follow = 0x8000;
module_param(eint_follow, uint, 0600);
MODULE_PARM_DESC(eint_follow, "EINT bits whose wake follows their IRQ's enable_irq_wake (default: host_req)");
static int follow_irq[32];	/* 0: that bit is not followed */
/* EINT bits (8 x gpaN + pin) whose pad is a plain input while the SoC
 * sleeps, as stock's sec_ts off_state pinctrl (attn-input) does. The
 * touchscreen's attn (gpa1-0, s6sy761_irq) is wake-masked but stays
 * asserted after the driver's application sleep; its level re-latches the
 * pending bit microseconds after the clear, ACPM finds GRP4 set and gives the
 * sleep up ("other" fw-abort, 30 Oct). Only bits masked in eint_wake and not
 * followed are switched; restore_platform() puts the EINT function back. */
static unsigned int eint_quiet = 0x100;
module_param(eint_quiet, uint, 0600);
MODULE_PARM_DESC(eint_quiet, "wake-masked EINT bits whose pad is a plain input during sleep (default: touch attn gpa1-0)");
static unsigned int quieted;		/* the bits switched for this sleep */
static u32 saved_quiet_con[4];
static unsigned long quiet_sleeps;
module_param(quiet_sleeps, ulong, 0444);

static unsigned int eint_effective;	/* the PMU 0x60c word of the last sleep */
module_param_cb(eint_effective, &hex32_ops, &eint_effective, 0444);
static unsigned long hub_refused, hub_pend_cleared, grp4_stale_clears;
module_param(hub_refused, ulong, 0444);
module_param(hub_pend_cleared, ulong, 0444);
module_param(grp4_stale_clears, ulong, 0444);
/* Clear GRP4 at each attempt once the followed lines are low: a latch the
 * hub's host_req left behind is stale by then. Not sticky: a live latch
 * comes back and the firmware gives the sleep up (zrn-sleepd's
 * FW_ABORT_LIMIT bounds that). */
static bool grp4_stale_clear = true;
module_param(grp4_stale_clear, bool, 0644);
static unsigned int grp4_at_smc;	/* GRP4 read last, just before the SMC */
module_param_cb(grp4_at_smc, &hex32_ops, &grp4_at_smc, 0444);

static void follow_resolve(void)
{
	for (unsigned int b = 0; b < ARRAY_SIZE(follow_irq); b++) {
		struct gpio_device *gdev;
		char label[8];
		int irq = -ENODEV;

		if (!(eint_follow & BIT(b)))
			continue;
		snprintf(label, sizeof(label), "gpa%u", b / 8);
		gdev = gpio_device_find_by_label(label);
		if (gdev) {
			struct gpio_desc *desc = gpio_device_get_desc(gdev, b % 8);

			if (!IS_ERR_OR_NULL(desc))
				irq = gpiod_to_irq(desc);
			gpio_device_put(gdev);
		}
		if (irq <= 0) {
			pr_warn("zrn-sleep-init: cannot follow eint bit %u\n", b);
			continue;
		}
		follow_irq[b] = irq;
		pr_info("zrn-sleep-init: eint bit %u (%s-%u) follows IRQ %d's wake setting\n",
			b, label, b % 8, irq);
	}
}

/* eint_wake with the followed bits whose IRQ is wake-enabled opened */
static unsigned int effective_wake(void)
{
	unsigned int eff = eint_wake;

	for (unsigned int b = 0; b < ARRAY_SIZE(follow_irq); b++) {
		struct irq_data *d;

		if (!(eint_follow & BIT(b)) || !follow_irq[b])
			continue;
		d = irq_get_irq_data(follow_irq[b]);
		if (d && irqd_is_wakeup_set(d))
			eff &= ~BIT(b);
	}
	return eff;
}

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

/* 0: off (the default). Seconds the cluster-1 watchdog runs, reset on,
 * from the tables' return to the watchdog driver's resume (wdt-resume.h);
 * 82 s at most. */
static unsigned int wdt_resume_s;
module_param(wdt_resume_s, uint, 0600);
MODULE_PARM_DESC(wdt_resume_s, "arm the cluster-1 watchdog this long over the resume window (0: off)");
static struct zrn_wdt_cover wdt_cover;
module_param_named(wdt_arms, wdt_cover.arms, uint, 0444);
module_param_named(wdt_handed, wdt_cover.handed, uint, 0444);
module_param_named(wdt_stopped, wdt_cover.stopped, uint, 0444);

static int wdt_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	if (event == PM_POST_SUSPEND)
		zrn_wdt_release(&wdt_cover);
	return NOTIFY_OK;
}
static struct notifier_block wdt_notifier = { .notifier_call = wdt_notify };

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
	/* the clocks are back: from here to the driver's resume, a hang resets */
	zrn_wdt_arm(&wdt_cover, wdt_resume_s);
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		writel_relaxed(saved_wake[i], pmu + wake_offsets[i]);
		writel_relaxed(saved_idle[i], pmu + 0x3f0 + 4 * i);
	}
	for (i = 0; i < 4; i++)
		if ((quieted >> (i * 8)) & 0xff)
			writel_relaxed(saved_quiet_con[i], mapped_base(0x14050000) + i * 0x20);
	quieted = 0;
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
	unsigned int eff;
	int ret, i;
	if (pm_suspend_target_state != PM_SUSPEND_MEM || dry_run)
		return 0;
	if (num_online_cpus() != 1 || raw_smp_processor_id() != 0)
		return -EBUSY;
	eff = eint_effective = effective_wake();
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		saved_usi[i] = readl_relaxed(mapped_base(usi_base[i]) + usi_offset[i]);
	gpio_save();
	for (i = 0; i < 3; i++)
		saved_wdt0[i] = readl_relaxed(mapped_base(0x10050000) + i * 4);
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		saved_eint_mask[i] = readl_relaxed(mapped_base(0x14050000) + 0x900 + eint_offset[i]);
		writel_relaxed(saved_eint_mask[i] | (i == 4 ? 0xff : ((eff >> (i * 8)) & 0xff)), mapped_base(0x14050000) + 0x900 + eint_offset[i]);
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
	writel_relaxed(eff, pmu + 0x60c);
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
	void __iomem *grp4 = mapped_base(0x14070000) + 0x408;
	unsigned long enable = mif_sleep;
	unsigned int b, low = 0, grp4_now;
	int hub_line = -1;
	int i, ret = prepare_platform();
	if (ret)
		return ret;
	if (prepare_only) {
		restore_platform();
		return -EAGAIN;
	}
	/* A followed line (host_req) that is wake-enabled and high: the hub is
	 * talking now. Going down would only wake at once (or sleep through it,
	 * with the bit masked): refuse, and zrn-sleepd tries again. DAT of gpaN
	 * is at 0x14050004 + N x 0x20. */
	for (b = 0; b < ARRAY_SIZE(follow_irq); b++) {
		u32 dat;

		if (!(eint_follow & BIT(b)) || !follow_irq[b] || (eint_effective & BIT(b)))
			continue;
		dat = readl_relaxed(mapped_base(0x14050000) + (b / 8) * 0x20 + 4);
		if (hub_line < 0)
			hub_line = (dat >> (b % 8)) & 1;
		if ((dat >> (b % 8)) & 1) {
			hub_refused++;
			pr_info("zrn-system-sleep: refused: host_req high (eint bit %u)\n", b);
			restore_platform();
			return -EBUSY;
		}
		low |= BIT(b);
	}
	/* A masked level line still latches its pending bit: make its pad a
	 * plain input (CON nibble 0) before the clear below, so it stays clear
	 * through ACPM's GRP4 check. CON of gpaN is at 0x14050000 + N x 0x20. */
	quieted = eint_quiet & eint_wake & ~eint_follow;
	for (i = 0; i < 4; i++) {
		void __iomem *con = mapped_base(0x14050000) + i * 0x20;
		u32 pins = (quieted >> (i * 8)) & 0xff, v;

		if (!pins)
			continue;
		saved_quiet_con[i] = v = readl_relaxed(con);
		for (b = 0; b < 8; b++)
			if (pins & BIT(b))
				v &= ~(0xfU << (b * 4));
		writel_relaxed(v, con);
	}
	if (quieted)
		quiet_sleeps++;
	wmb();
	/* Discard only latched events from explicitly masked diagnostic sources
	 * and the stale latches of followed lines that are low; preserve the
	 * power-key latch and all enabled wake sources. */
	for (i = 0; i < ARRAY_SIZE(eint_offset); i++) {
		u32 mask = i == 4 ? 0xff : (((eint_wake | low) >> (i * 8)) & 0xff);
		eint_before[i] = readl_relaxed(mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
		if (i < 4 && (eint_before[i] & (low >> (i * 8)) & 0xff))
			hub_pend_cleared++;
		writel_relaxed(eint_before[i] & mask, mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	}
	wmb();
	for (i = 0; i < 16; i++)
		pending_before[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	mif_requests_before = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
	grp4_before = readl_relaxed(grp4);
	chub_status_before = readl_relaxed(mapped_base(0x14060000) + 0x4084);
	grp4_cleared = 0;
	/* the followed lines are low by now (else refused above) */
	if (grp4_before && (clear_grp4 || grp4_stale_clear)) {
		/* write-one-to-clear, held until written back to zero (as
		 * pmucal_apm_pmu_init does) */
		writel_relaxed(grp4_before, mapped_base(0x14070000) + 0x40c);
		wmb();
		writel_relaxed(0, mapped_base(0x14070000) + 0x40c);
		wmb();
		grp4_cleared = readl_relaxed(grp4);
		if (grp4_stale_clear)
			grp4_stale_clears++;
	}
	grp4_now = readl_relaxed(grp4);
	/* Keep this before enabling the MIF gate: retained console records must
	 * distinguish a missing AP request from corruption of earlier log text. */
	pr_info("zrn-system-sleep: handoff mif_sleep=%u requests=%02x gate=%u grp4=%08x->%08x chub=%08x eint_eff=%08x hub_line=%d grp4_at_smc=%08x\n",
		mif_sleep, mif_requests_before,
		readl_relaxed(mapped_base(0x02040000) + 0xf95c), grp4_before, grp4_cleared,
		chub_status_before, eint_effective, hub_line, grp4_now);
	/* again, after the console: what ACPM will find (zrn-sleepd's fw-abort) */
	grp4_at_smc = readl_relaxed(grp4);
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

static void apply_pdn(void)
{
	for (int i = 0; i < ARRAY_SIZE(pdn_pins); i++) {
		const struct pdn_pin *p = &pdn_pins[i];
		void __iomem *bank = mapped_base(p->base) + p->bank * 0x20;
		u32 con = readl_relaxed(bank + 0x10), pud = readl_relaxed(bank + 0x14);
		u32 new_con = (con & ~(3U << 2 * p->pin)) | (u32)p->con << 2 * p->pin;
		u32 new_pud = (pud & ~(0xfU << 4 * p->pin)) | (u32)p->pud << 4 * p->pin;

		writel_relaxed(new_con, bank + 0x10);
		writel_relaxed(new_pud, bank + 0x14);
		new_con = readl_relaxed(bank + 0x10);
		new_pud = readl_relaxed(bank + 0x14);
		pr_info("zrn-sleep-init: pdn %s conpdn %08x->%08x pudpdn %08x->%08x\n",
			p->name, con, new_con, pud, new_pud);
		if (p->base == 0x10430000 &&
		    ((new_con >> 2 * p->pin) & 3) == p->con &&
		    ((new_pud >> 4 * p->pin) & 0xf) == p->pud)
			pdn_applied |= 1UL << (8 * p->bank + p->pin);
	}
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
	ret = map_base(0x10060000);
	if (ret)
		goto fail;
	ret = map_base(0x14060000);
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
		if (pdn)
			apply_pdn();
		follow_resolve();
		wdt_cover.wdt = mapped_base(0x10060000);
		wdt_cover.pmu = mapped_base(0x14060000);
		register_pm_notifier(&wdt_notifier);
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
	if (enable_deep)
		unregister_pm_notifier(&wdt_notifier);
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
