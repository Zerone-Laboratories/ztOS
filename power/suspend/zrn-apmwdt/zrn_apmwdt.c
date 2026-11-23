// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-apmwdt: keep the APM watchdog (WDT_APM, 0x14030000) running through
 * SYS_SLEEP on the Exynos 9810, so that a hang inside the sleep ends in a
 * watchdog reset instead of a key reset.
 *
 * Why: after a key (PIN) reset sboot prints only "FLEXPMU-DBG: PIN RESET".
 * After any other reset it prints the whole flexpmu state into its log ring
 * (0xfda10000): sequencer [UP]/[DOWN] running or done, the step it stopped
 * on, dram_init status, the sleep counters. That is the only record that can
 * tell "never woke" from "woke and hung in the MIF-up sequence".
 *
 * ACPM uses this watchdog itself: its framework ops table (ACPM 0x62dc) has
 * wdt_start at ACPM 0x4aa0 (WTCNT = 100000, WTCON = 0xe05 | 0x20) and
 * wdt_stop at ACPM 0x4ad8 (WTCON &= ~0x20, WTCLRINT = 1). The stop is what
 * undid zrn-system-sleep's wdt_apm_guard_s before the sleep. This module
 * makes both return at once (Thumb "bx lr" over their first instruction), so
 * a count armed from Linux stays armed. ACPM address A is SoC 0x02039000 + A.
 *
 * Nothing is written unless the two first instructions are the ones this was
 * written against. sboot loads ACPM again at every boot, so a reboot undoes
 * the patch; unloading the module puts the two instructions back.
 *
 * timeout_reset (default on): the flexpmu sequencer's wait helper (ACPM
 * 0xf098) gives up after 0x13d620 timer ticks, logs "r_w_val" and then sits
 * in "wfi; b ." at ACPM 0xf0ea for ever, which is a dead phone. The patch
 * makes that path write 1 to the PMU's SWRESET (0x14060400, ACPM 0xa4060400)
 * instead: a software reset, after which sboot prints which sequencer step
 * was waiting. The literal that held the "r_w_val" string pointer (ACPM
 * 0xf0f8) becomes the register address; the log call is given up.
 *
 * swreset_test: write that SWRESET bit from Linux now (the phone restarts),
 * to check the register before trusting the patch.
 *
 * arm_s: also arm the watchdog now for that many seconds (at most 82) and
 * never service it: the phone resets when it runs out. For checking, awake,
 * that the reset is routed and that sboot decodes it.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

#define SRAM_PAGE	0x0203d000	/* ACPM 0x4000..0x4fff */
#define WDT_START	0xaa0		/* movs r2, #1 */
#define WDT_STOP	0xad8		/* movs r1, #32 */
#define BX_LR		0x4770

#define SEQ_PAGE	0x02048000	/* ACPM 0xf000..0xffff */
#define SEQ_FAIL	0x0e2		/* movs r2, #15; ldr r0, =r_w_val; bl log; wfi; b . */
#define SEQ_LIT		0x0f8

static bool timeout_reset = true;
module_param(timeout_reset, bool, 0444);
static bool swreset_test;
module_param(swreset_test, bool, 0444);
static uint seq_patched;
module_param(seq_patched, uint, 0444);
static uint arm_s;
module_param(arm_s, uint, 0444);
static uint start_was, stop_was, start_now, stop_now;
module_param(start_was, uint, 0444);
module_param(stop_was, uint, 0444);
module_param(start_now, uint, 0444);
module_param(stop_now, uint, 0444);
static uint wtcon_before, wtcnt_before, wtcon_after, mask_before, mask_after, apm_rst_en;
module_param(wtcon_before, uint, 0444);
module_param(wtcnt_before, uint, 0444);
module_param(wtcon_after, uint, 0444);
module_param(mask_before, uint, 0444);
module_param(mask_after, uint, 0444);
module_param(apm_rst_en, uint, 0444);

static void __iomem *sram, *seq;
static const u16 seq_was[6] = { 0x220f, 0x4804, 0xf7ff, 0xff19, 0xbf30, 0xe7fd };
/* ldr r0, [pc, #20] (the literal); movs r1, #1; str r1, [r0]; wfi; b .-2 */
static const u16 seq_new[5] = { 0x4805, 0x2101, 0x6001, 0xbf30, 0xe7fd };

static void patch_timeout(void)
{
	int i;

	seq = ioremap(SEQ_PAGE, 0x1000);
	if (!seq)
		return;
	for (i = 0; i < 6; i++)
		if (readw(seq + SEQ_FAIL + 2 * i) != seq_was[i])
			break;
	if (i < 6 || readl(seq + SEQ_LIT) != 0x00011acc) {
		pr_err("zrn-apmwdt: wait-timeout path differs, not patched\n");
		return;
	}
	writel(0xa4060400, seq + SEQ_LIT);
	for (i = 4; i >= 0; i--)
		writew(seq_new[i], seq + SEQ_FAIL + 2 * i);
	seq_patched = 1;
	pr_info("zrn-apmwdt: a sequencer wait that times out now resets the SoC\n");
}

static int __init zrn_apmwdt_init(void)
{
	void __iomem *wdt, *pmu, *apm;

	sram = ioremap(SRAM_PAGE, 0x1000);
	if (!sram)
		return -ENOMEM;
	start_was = readw(sram + WDT_START);
	stop_was = readw(sram + WDT_STOP);
	if (start_was != 0x2201 || stop_was != 0x2120) {
		pr_err("zrn-apmwdt: ACPM image differs (start %04x stop %04x), nothing written\n",
		       start_was, stop_was);
		iounmap(sram);
		return -EINVAL;
	}
	writew(BX_LR, sram + WDT_STOP);
	writew(BX_LR, sram + WDT_START);
	start_now = readw(sram + WDT_START);
	stop_now = readw(sram + WDT_STOP);
	pr_info("zrn-apmwdt: ACPM wdt_start %04x -> %04x, wdt_stop %04x -> %04x\n",
		start_was, start_now, stop_was, stop_now);
	if (timeout_reset)
		patch_timeout();
	if (swreset_test) {
		void __iomem *p = ioremap(0x14060000, 0x1000);

		pr_emerg("zrn-apmwdt: writing PMU SWRESET now\n");
		mdelay(200);
		writel(1, p + 0x400);
		mdelay(2000);
		pr_emerg("zrn-apmwdt: SWRESET write did not reset the SoC\n");
	}
	if (!arm_s)
		return 0;

	wdt = ioremap(0x14030000, 0x1000);
	pmu = ioremap(0x14060000, 0x1000);
	apm = ioremap(0x14010000, 0x1000);
	if (!wdt || !pmu || !apm)
		return 0;
	wtcon_before = readl(wdt);
	wtcnt_before = readl(wdt + 8);
	mask_before = readl(pmu + 0x40c);
	apm_rst_en = readl(apm + 0x424);
	/* as zrn-system-sleep's wdt_apm_arm: prescaler 255, /128, about 793 ticks/s */
	writel(0, wdt);
	writel(min_t(u32, arm_s * 793, 0xffff), wdt + 4);
	writel(min_t(u32, arm_s * 793, 0xffff), wdt + 8);
	writel(readl(pmu + 0x408) & ~BIT(25), pmu + 0x408);
	writel(mask_before & ~BIT(25), pmu + 0x40c);
	writel(1, apm + 0x424);		/* ACPM's wdt_start writes this first */
	writel(0xff39, wdt);
	wtcon_after = readl(wdt);
	mask_after = readl(pmu + 0x40c);
	pr_emerg("zrn-apmwdt: WDT_APM armed for %u s and not serviced: the phone resets then (WTCON %08x, WTCNT %08x)\n",
		 arm_s, wtcon_after, readl(wdt + 8));
	return 0;
}

static void __exit zrn_apmwdt_exit(void)
{
	int i;

	if (seq_patched) {
		for (i = 0; i < 6; i++)
			writew(seq_was[i], seq + SEQ_FAIL + 2 * i);
		writel(0x00011acc, seq + SEQ_LIT);
	}
	writew(start_was, sram + WDT_START);
	writew(stop_was, sram + WDT_STOP);
	iounmap(sram);
}

module_init(zrn_apmwdt_init);
module_exit(zrn_apmwdt_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Keep the Exynos 9810 APM watchdog armed through SYS_SLEEP");
