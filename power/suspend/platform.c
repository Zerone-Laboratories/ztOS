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
#include <linux/soc/samsung/s9p-acpm.h>	/* s9p_acpm_dvfs_sleep: MIF sleep-release */
#include <linux/suspend.h>
#include <linux/sysfs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>
#include "twrp-regdump/zrn-snapshot.h"

/* Register snapshot to compare with Samsung's kernel (zrn_stock_snapshot.ko
 * in TWRP): /proc/zrn-sleep-snapshot shows one taken at load and one at the
 * firmware handoff when snapshot_abort is set (that sleep is then given up). */
static bool snapshot_abort;
module_param(snapshot_abort, bool, 0644);
static struct zrn_snap snap_awake, snap_sleep;
static int zrn_snapshot_show(struct seq_file *m, void *v)
{
	ZRN_SNAP_SHOW(m, &snap_awake, "ztos-awake");
	ZRN_SNAP_SHOW(m, &snap_sleep, "ztos-sleep-enter");
	return 0;
}
static int zrn_snapshot_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, zrn_snapshot_show, NULL, 1 << 20);
}
static const struct proc_ops zrn_snapshot_ops = {
	.proc_open = zrn_snapshot_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
static void zrn_snapshot_setup(void)
{
	int ret;

	snap_awake.sram = vmalloc(ZRN_SNAP_SRAM_SIZE);
	snap_sleep.sram = vmalloc(ZRN_SNAP_SRAM_SIZE);
	ret = zrn_snap_map();
	if (ret || !snap_awake.sram || !snap_sleep.sram) {
		pr_warn("zrn-sleep-init: snapshot unavailable (%d)\n", ret);
		snapshot_abort = false;
		return;
	}
	zrn_snap_take(&snap_awake);
	proc_create("zrn-sleep-snapshot", 0444, NULL, &zrn_snapshot_ops);
}
#include <asm/memory.h>
#include <linux/memblock.h>	/* for_each_mem_range (sleep_clean_ram) */
#include <asm/arch_timer.h>	/* __arch_counter_get_cntvct / arch_timer_get_cntfrq */
#include <asm/cacheflush.h>	/* dcache_clean_poc (sleep_clean_ram) */
#include "sequence.h"
#include "sleep-tables.h"
#include "init-memory.h"
#include "wdt-resume.h"

static bool enable_deep, dry_run = true, usbl2, probe_save;
static bool prepare_only = true;
static bool mif_sleep;
static bool sicd_mode;
static unsigned int init_changes, init_verified;
module_param(init_changes, uint, 0444);
module_param(init_verified, uint, 0444);
static unsigned int mif_gate_before, mif_requests_before;
module_param(mif_gate_before, uint, 0444);
module_param(mif_requests_before, uint, 0444);
/* DID_MIF_COUNT_SLEEP (acpm_get_mifdn_count) before/after the SMC. Equal =>
 * MIF never latched down (a master in mif_requests_before was still voting). */
static unsigned int mifdn_before, mifdn_after;
module_param(mifdn_before, uint, 0444);
module_param(mifdn_after, uint, 0444);
static uint cp_settle_ms = 300, cp_settle_waited_us;
module_param(cp_settle_ms, uint, 0644);
module_param(cp_settle_waited_us, uint, 0444);
/* GRP4 trace: start of prepare, after wake masks, after tables; then the
 * first poll index (0.5 us steps) at which a cleared GRP4 latched again. */
/* Turn the GIC CPU interface off before the GRP4 clear: its wake request is
 * WAKEUP_STAT bit 31, which SYS_SLEEP masks; masked and asserted, it latches
 * GRP4 and ACPM gives the sleep up (upend_wk). */
static bool gicc_off;
static uint gicc_saved;
module_param(gicc_off, bool, 0644);
module_param(gicc_saved, uint, 0444);
/* gicd_off: disable the whole GIC distributor (GICD_CTLR 0x10101000) across the
 * sleep SMC. gicc_off (CPU interface) alone does not gate the SoC wake request;
 * orphan level SPIs keep bit 31 asserted. The distributor off stops all SPI
 * forwarding; the RTC-alarm wake is PMU-direct, so it still wakes. */
static bool gicd_off;
static uint gicd_saved;
module_param(gicd_off, bool, 0644);
module_param(gicd_saved, uint, 0444);
/* mct_quiet: the global MCT (0x10040000) broadcast comparator keeps its interrupt
 * armed across this custom sleep path -- the tick framework hands the wake to a
 * G_COMP that is scheduled a few ms out, and if it matches in the brief MIF-down
 * window it raises a GIC SPI that latches PMU WAKEUP_STAT bit 31, so ACPM gives
 * the sleep up (smc=-1, wake=0x80000000) regardless of gicc/gicd-off. Save
 * G_INT_ENB, disable every global-comparator interrupt and ack G_INT_CSTAT just
 * before the SMC; restore on resume. The RTC-alarm wake is PMU-direct, so a
 * disabled MCT interrupt does not lose it. */
static bool mct_quiet;
static uint mct_int_enb_saved;
module_param(mct_quiet, bool, 0644);
module_param(mct_int_enb_saved, uint, 0444);
/* mct_park: the RIGHT way to stop the MCT global tick from waking us. Rather
 * than mask its interrupt (mct_quiet -- ACPM reads MCT state and DECLINES the
 * sleep when its G_INT_ENB is cleared), leave the interrupt enable untouched and
 * move the active comparator G_COMP0 (0x200/0x204) far past the RTC alarm, so it
 * simply never matches during the sleep. G_COMP0 is the kernel tick/next-hrtimer
 * comparator (ADD_INCR=0, one-shot); ACPM does not read the comparator VALUE, so
 * this does not trip the decline. Saved and restored across the SMC; the
 * clockevents tick_resume reprograms it on wake regardless. The RTC-alarm wake is
 * PMU-direct. Offset 0x4_0000_0000 counts is >160s at the 26 MHz MCT, past any
 * trial alarm. */
static bool mct_park;
static uint mct_comp0_l_saved, mct_comp0_u_saved;
module_param(mct_park, bool, 0644);
module_param(mct_comp0_l_saved, uint, 0444);
module_param(mct_comp0_u_saved, uint, 0444);
/* wake_capture: name exactly what woke the sleep. Reads GICD ISPENDR (hwirq
 * 0..511, 16 words) the instant the SMC returns, before any restore clears it;
 * plus GICD ISACTIVER0, GICC_HPPIR and an early PMU WAKEUP_STAT snapshot. A set
 * ISPENDR bit at word w bit b means hwirq (w*32+b) was pending at wake. */
static bool wake_capture;
module_param(wake_capture, bool, 0644);
/* sleep_pstate: the CPU_SUSPEND power-state arg (SMC a1) for the full SYS_SLEEP.
 * Our long-standing value 0x03000000 is affinity-3 STANDBY in ARM PSCI original
 * format (StateType bit16=0), not a power-down -- and it equals the cmucal
 * PLL_TYPE constant, suggesting it was mis-copied. The vendor exynos-pm.c passes
 * suspend_psci_idx=0x85 (133) through arm_cpuidle_suspend to the firmware
 * CPU_SUSPEND. A wrong power-state makes the firmware do a transient power-down
 * and return in ~4ms (the self-exit we see). Tunable to find the value that
 * makes the firmware actually hold the SoC asleep. Default keeps old behavior. */
static unsigned int sleep_pstate = 0x03000000;
module_param(sleep_pstate, uint, 0644);
MODULE_PARM_DESC(sleep_pstate, "CPU_SUSPEND power-state arg for SYS_SLEEP (default 0x03000000; vendor suspend_psci_idx=0x85)");
/* abox_syspower_off: zero ABOX_SYSPOWER_CTRL (0x17c50010) right before the SMC so
 * ABOX drops its MIF/DRAM vote (acpm_get_mif_request bit4) -- the RE-confirmed
 * holder, asserted while ABOX is runtime-active, independent of the DSP WFI.
 * Saved and restored across the SMC. Brute (ABOX re-asserts while active); for
 * proving MIF latches down. Default off. */
static bool abox_syspower_off;
module_param(abox_syspower_off, bool, 0644);
static unsigned int abox_syspower_saved, abox_vote_before, abox_vote_after;
module_param(abox_syspower_saved, uint, 0444);
module_param(abox_vote_before, uint, 0444);
module_param(abox_vote_after, uint, 0444);
static unsigned int wake_ispend[16];
static unsigned int wake_ispend_count = 16;
module_param_array(wake_ispend, uint, &wake_ispend_count, 0444);
static unsigned int wake_isactive0, wake_hppir, wake_stat_early;
module_param(wake_isactive0, uint, 0444);
module_param(wake_hppir, uint, 0444);
module_param(wake_stat_early, uint, 0444);
/* --- ported 9810 deep-sleep arming (from the 9820 deep driver), all default OFF --- */
/* apsoc_sleep: the ACPM firmware's own AP/SOC sequence-down count for the
 * SYS_SLEEP power mode, read across the SMC -- the proof that the clusters
 * actually power-collapsed (vs STANDBY). 9810 flexpmu-dbg SRAM base is
 * 0x0204f800, DATA_LINE 16; DID_AP_COUNT_SLEEP (enum index 22) holds the
 * suspend AP/SOC counter, whose down-count word is at +0x16c = 0x0204f96c and
 * whose early-wakeup word is at +0x168 = 0x0204f968. This is the exact 9810
 * analog of the 9820 FLEXPMU_APSOC_DOWN (its flexpmu base 0x0204bc00 + 0x16c).
 * Verified by the same stride: DID_MIF_COUNT_SLEEP (23) down-count lands at
 * +0x17c = 0x0204f97c, which is our existing mifcntSL. If apsoc_after >
 * apsoc_before across a sleep the APM ran SOC_SLEEP_DOWN; if it does not, the
 * firmware declined and did STANDBY. */
static unsigned int apsoc_before, apsoc_after;
module_param(apsoc_before, uint, 0444);
module_param(apsoc_after, uint, 0444);
/* clean_ram: clean all of DRAM to the point of coherency by VA immediately
 * before the firmware SMC (ported from the 9820 driver's sleep_clean_ram).
 * cpu_suspend already cleans the resume context, but the APM's SOC_SLEEP_DOWN
 * does not write every CPU's caches back for this state, so once the clusters
 * actually power-collapse the last writes made as the other CPUs went offline
 * (the vmalloc / hrtimer / printk-ring lines) are lost and resume reads them
 * stale. A set/way clean reaches only this CPU and may be trapped at EL2; by
 * VA reaches every coherent cache. Harmless while the SoC only reaches STANDBY
 * (the caches stay live), so it is safe to A/B against the power number.
 * Default off. NOTE: dcache_clean_poc may not be exported to modules -- see
 * sleep_clean_ram(); a kernel rebuild (EXPORT_SYMBOL) or a built-in build may
 * be needed. */
static bool clean_ram;
module_param(clean_ram, bool, 0644);
MODULE_PARM_DESC(clean_ram, "clean all DRAM to PoC by VA before the SMC (resume correctness once the clusters power-collapse); default off");
static unsigned int clean_ram_us;
module_param(clean_ram_us, uint, 0444);
/* cpu_shortstop: apply the two full-width CPU-cluster SHORTSTOP writes the
 * vendor pmucal_lpm_init makes (flexpmu_cal_system_exynos9810.h:61-62) and that
 * init-memory.h deliberately leaves out -- CPUCL0_SHORTSTOP 0x1D000000+0x820
 * and CPUCL1_SHORTSTOP 0x1D100000+0x820, both 0xFFFFFFFF. These configure the
 * CPU clusters' clock short-stop for the power-down: the 9810 analog of the
 * 9820 driver's qch_sleep. The 9810 has no kernel-side QCH_CON release; the
 * cluster power-collapse itself is run by the APM soc_sequencer_sleep_down, so
 * this boot-time clock-mux setup is the only CPU-cluster arming the kernel
 * owns. Excluded since 16 Oct because the reserved-bit readback masks are
 * unknown; a plain write with no readback verify avoids that. Always-on CMU
 * block, so applied once at init. Default off. */
static bool cpu_shortstop;
module_param(cpu_shortstop, bool, 0444);
MODULE_PARM_DESC(cpu_shortstop, "apply full-width CPUCL0/1 SHORTSTOP (0x1D000000/0x1D100000 +0x820 = 0xffffffff) at init -- the 9810 qch-analog from pmucal_lpm_init; default off");
/* gic_clear_late: clear EVERY pending GIC SPI (GICD ICPENDR 0x10100000+0x1000
 * +0x280, words 1..31) immediately before the SMC -- a superset of display_quiet,
 * which only clears the DSIM/DPP/IDMA lines. Catches any transient SPI asserted
 * after the gic_pend read that would otherwise keep WAKEUP_STAT bit 31 high. The
 * RTC-alarm wake is PMU-direct, so clearing SPI pending here does not lose it. */
static bool gic_clear_late;
module_param(gic_clear_late, bool, 0644);
/* grp4_bid_off: disable GRP4_INTR_BID_ENABLE (0x14070400) bit0 + the mirror
 * PMU_EVENT_INTERRUPT_ENABLE_GRP4 (0x14060000+0x7f10) bit0 before clearing the
 * GRP4 pending, per the vendor flexpmu sleep-up order. Probes whether the
 * residual (non-GIC, non-EINT) GRP4 bit0 source can be silenced from the AP. */
static bool grp4_bid_off;
module_param(grp4_bid_off, bool, 0644);
/* grp4_smc_clear: write-1-then-0 the GRP4 pending (0x1407040c) one last time
 * immediately before the SMC, to drop a transient bit0 pulse the firmware's
 * upend check would otherwise catch. AP-safe (CLEAR reg only). Default on. */
/* DEFAULT OFF: writing the GRP4 clear right before the SMC pokes an APM-owned
 * register and makes ACPM DECLINE the sleep (smc=-1). GRP4 bit0 being set at the
 * SMC does NOT prevent a clean sleep (observed smc=0 with grp4_at_smc=1), so this
 * clear is unnecessary as well as harmful. Left as an off-by-default diagnostic. */
static bool grp4_smc_clear;
module_param(grp4_smc_clear, bool, 0644);
/* grp_lo_clear: before the SMC, clear GRP0..GRP3 UPEND (read status +0x08,
 * write-1-to-clear +0x0c) the way the stock exynos CPU-PM notifier does per-CPU.
 * ztOS only ever cleared GRP4; an uncleared GRP0/1 UPEND is a NON-GIC source
 * (gicc_off/gicd_off can't touch it) that aggregates into the firmware's wake
 * decision and aborts the MIF-down sleep (empty ACPM log, bit31 early wake). */
static bool grp_lo_clear;
module_param(grp_lo_clear, bool, 0644);
/* mif_ap_clear: in the IRQs-off finisher, directly clear the AP's MIF
 * req_Master bit (DID_SW_FLAG 0x0204f8ac bit 16+4 = 0x00100000). The DVFS
 * release IPC (s9p_acpm_dvfs_sleep) returns 0 but LEAVES this bit set -- it
 * updates the DVFS min-freq constraint, not the flexpmu SW_FLAG. A direct
 * write clears it and the firmware does not re-raise it in the IRQs-off
 * window (live-proven: sticks 0.5s+), so ACPM sees no AP MIF vote and
 * completes MIF-down. Pairs with the ABOX SYSPOWER fix that drops bit1 (AUD). */
static bool mif_ap_clear;
module_param(mif_ap_clear, bool, 0644);
/* mif_force_release: in the IRQs-off finisher (ACPM ISR quiesced), force the
 * firmware MIF-request refcount struct to 0 so ent_mif reaches mif_down_callback
 * (the only path to MIF-down). The gate is req_count 0x0204b264==0; granted
 * 0x0204b268 is the authoritative source (req_count/bitmap are derived), so
 * clear granted + bitmap 0x0204b26c + count, keep MIF_state 0x0204b260=1. The
 * diag mirror 0x0204f8ac and the DVFS release IPC cannot move the count (RE:
 * AP=word bit20/internal bit4, AUD=word bit17/internal bit1; AUD held by live
 * audio). Direct write is safe only with the ISR off = this IRQs-off window. */
static bool mif_force_release;
module_param(mif_force_release, bool, 0644);
/* display_quiet: the DECON pipeline keeps running at screen-off; ztOS s9p-decon
 * does not claim the vendor DSIM (hwirq 221) / DPP+IDMA (hwirq 229/236) IRQs, so
 * they stay asserted, aggregate into PMU GRP4 bit0, and ACPM aborts every deep
 * sleep (wake_stat bit31 = firmware-abort). Mask DSIM INTMSK + the DPP/IDMA IRQ
 * enables and clear their GIC pending just before the SMC; restore on resume.
 * The driver only polls DSIM_INTSRC, never uses these IRQ lines, so this is safe. */
static bool display_quiet;
static uint dq_dsim_intmsk;		/* saved DSIM INTMSK (0x16080054) */
static uint dq_dpp_irq, dq_idma_irq;	/* saved DPP/IDMA IRQ ctrl */
static uint dq_decon_inten;		/* saved DECON_INTERRUPT_ENABLE (0x16030040) */
static uint dq_uart_uintm;		/* saved UART UINTM (0x10440038) */
static uint gic_pend;			/* count of SPIs still pending at the SMC */
module_param(display_quiet, bool, 0644);
module_param(dq_dsim_intmsk, uint, 0444);
module_param(gic_pend, uint, 0444);
/* Freeze guard: WDT_APM (0x14030000) still runs from the APM's RC oscillator
 * in MIF-down. Armed just before the SMC with reset routed (PMU 0x408/0x40c
 * bit 25, RST_STAT bit 25 seen on its bite), disarmed as soon as the AP runs
 * again. A hang anywhere in ACPM's sleep then resets the phone after about
 * wdt_apm_guard_s seconds (prescaler 255, /128: about 793 ticks/s). */
static void __iomem *mapped_base(u32 physical);
static uint wdt_apm_guard_s;
static uint wdt_apm_saved[5], wdt_apm_armed;
module_param(wdt_apm_guard_s, uint, 0644);
module_param(wdt_apm_armed, uint, 0444);
static void wdt_apm_arm(void)
{
	void __iomem *w = mapped_base(0x14030000), *p = mapped_base(0x14060000);
	u32 n;
	if (!wdt_apm_guard_s || !w)
		return;
	n = min_t(u32, wdt_apm_guard_s * 793, 0xffff);
	wdt_apm_saved[0] = readl_relaxed(w);
	wdt_apm_saved[1] = readl_relaxed(w + 4);
	wdt_apm_saved[2] = readl_relaxed(w + 8);
	wdt_apm_saved[3] = readl_relaxed(p + 0x408);
	wdt_apm_saved[4] = readl_relaxed(p + 0x40c);
	writel_relaxed(0, w);
	writel_relaxed(n, w + 4);
	writel_relaxed(n, w + 8);
	writel_relaxed(wdt_apm_saved[3] & ~BIT(25), p + 0x408);
	writel_relaxed(wdt_apm_saved[4] & ~BIT(25), p + 0x40c);
	writel_relaxed(0xff39, w);
	dsb(sy);
	wdt_apm_armed = 1;
}
static void wdt_apm_disarm(void)
{
	void __iomem *w = mapped_base(0x14030000), *p = mapped_base(0x14060000);
	if (!wdt_apm_armed)
		return;
	writel_relaxed(0, w);
	writel_relaxed(wdt_apm_saved[3], p + 0x408);
	writel_relaxed(wdt_apm_saved[4], p + 0x40c);
	writel_relaxed(wdt_apm_saved[1], w + 4);
	writel_relaxed(wdt_apm_saved[2], w + 8);
	writel_relaxed(wdt_apm_saved[0], w);
	dsb(sy);
	wdt_apm_armed = 0;
}
static uint g4_trace[4];
static int g4_relatch = -1;
module_param_array(g4_trace, uint, NULL, 0444);
module_param(g4_relatch, int, 0444);
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

static void __iomem *mapped_base(u32 base);

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

/* wake_src capture: the full wake-source state at the SMC ([*_smc]) and at
 * resume ([*_wake]). WAKEUP_STAT (0x600), WAKEUP_STAT2 (0x604), the five
 * PMU_INTR_GEN GRPn status (0x14070000 + n*0x100 + 0x08) and pend (+0x0c), and
 * the five ALIVE EINT_PEND words. Names exactly which bit/source is asserted
 * just before the SMC and which is set at wake. All 0444. */
static unsigned int ws_stat_smc, ws_stat2_smc, ws_stat_wake, ws_stat2_wake;
module_param_cb(ws_stat_smc,   &hex32_ops, &ws_stat_smc,   0444);
module_param_cb(ws_stat2_smc,  &hex32_ops, &ws_stat2_smc,  0444);
module_param_cb(ws_stat_wake,  &hex32_ops, &ws_stat_wake,  0444);
module_param_cb(ws_stat2_wake, &hex32_ops, &ws_stat2_wake, 0444);
static unsigned int ws_grp_stat_smc[5],  ws_grp_pend_smc[5];
static unsigned int ws_grp_stat_wake[5], ws_grp_pend_wake[5];
static unsigned int ws_eint_smc[5],      ws_eint_wake[5];
static unsigned int ws_grp_count = 5, ws_eint_count = 5;
module_param_array(ws_grp_stat_smc,  uint, &ws_grp_count,  0444);
module_param_array(ws_grp_pend_smc,  uint, &ws_grp_count,  0444);
module_param_array(ws_grp_stat_wake, uint, &ws_grp_count,  0444);
module_param_array(ws_grp_pend_wake, uint, &ws_grp_count,  0444);
module_param_array(ws_eint_smc,      uint, &ws_eint_count, 0444);
module_param_array(ws_eint_wake,     uint, &ws_eint_count, 0444);
static void wake_src_read(unsigned int *stat, unsigned int *stat2,
			  unsigned int grp_stat[5], unsigned int grp_pend[5],
			  unsigned int eint[5]);

/* EINT filters of the ALIVE wake-up banks gpa0-4: FLTCON at 0x14050800 +
 * the bank's fltcon offset (vendor exynos9810_pin_banks0: gpa0 0x00, gpa1
 * 0x08, gpa2 0x10, gpa3 0x18, gpa4 0x20), one byte a pin: bit 7 enable,
 * bit 6 digital (0: analog delay), bits 5:0 width. FLTCON1 (+4) holds pins
 * 4-7; gpa4 has two pins and only FLTCON0. Stock's exynos_pinctrl_suspend
 * (vendor pinctrl-exynos.c:722) sets every pin of every wake-up bank to
 * enable+analog (0x80) for sleep and back to enable+digital on resume; the
 * digital filter counts a clock that SYS_SLEEP stops. Mainline's 9810 banks
 * have no fltcon offset and never touch these. eint_filter_analog: save the
 * nine words in prepare_platform, write 0x80 to each pin as stock does, put
 * the saved words back in restore_platform. Off by default. */
static const u16 fltcon_offset[] = { 0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c, 0x820 };
static bool eint_filter_analog;
module_param(eint_filter_analog, bool, 0644);
MODULE_PARM_DESC(eint_filter_analog, "set the gpa0-4 EINT filters to enable+analog for the sleep, as stock suspend does (default off)");
static u32 fltcon_before[ARRAY_SIZE(fltcon_offset)], fltcon_after[ARRAY_SIZE(fltcon_offset)];
static unsigned int fltcon_count = ARRAY_SIZE(fltcon_offset);
module_param_array(fltcon_before, uint, &fltcon_count, 0444);
module_param_array(fltcon_after, uint, &fltcon_count, 0444);
static bool fltcon_saved;
/* pd_log: a second handoff line with the PMU power-domain STATUS words
 * (0x14064004 + 8 n up to 0x14064094; AUD 0x4004 ... CHUB 0x4084 in the
 * vendor flexpmu_cal_local_exynos9810.h) and the FLTCON words. Off by default. */
static bool pd_log;
module_param(pd_log, bool, 0644);
MODULE_PARM_DESC(pd_log, "log PMU power-domain status and EINT FLTCON words at the handoff (default off)");
#define PD_STATUS_COUNT ((0x4094 - 0x4004) / 8 + 1)
static u32 pd_status[PD_STATUS_COUNT];
static unsigned int pd_status_count = PD_STATUS_COUNT;
module_param_array(pd_status, uint, &pd_status_count, 0444);

static void fltcon_read(u32 *out)
{
	for (int i = 0; i < ARRAY_SIZE(fltcon_offset); i++)
		out[i] = readl_relaxed(mapped_base(0x14050000) + fltcon_offset[i]);
}

static void fltcon_analog(void)
{
	fltcon_read(fltcon_before);
	for (int i = 0; i < ARRAY_SIZE(fltcon_offset); i++)
		/* 0x820 is gpa4: two pins, bytes 0 and 1 */
		writel_relaxed(fltcon_offset[i] == 0x820 ? 0x8080 : 0x80808080,
			       mapped_base(0x14050000) + fltcon_offset[i]);
	wmb();
	fltcon_saved = true;
	fltcon_read(fltcon_after);
	pr_info("zrn-system-sleep: fltcon %08x %08x %08x %08x %08x %08x %08x %08x %08x -> %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
		fltcon_before[0], fltcon_before[1], fltcon_before[2], fltcon_before[3], fltcon_before[4],
		fltcon_before[5], fltcon_before[6], fltcon_before[7], fltcon_before[8],
		fltcon_after[0], fltcon_after[1], fltcon_after[2], fltcon_after[3], fltcon_after[4],
		fltcon_after[5], fltcon_after[6], fltcon_after[7], fltcon_after[8]);
}

static void fltcon_restore(void)
{
	if (!fltcon_saved)
		return;
	for (int i = 0; i < ARRAY_SIZE(fltcon_offset); i++)
		writel_relaxed(fltcon_before[i], mapped_base(0x14050000) + fltcon_offset[i]);
	fltcon_saved = false;
}

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

/* Fill one wake-source set from the already-mapped PMU/INTR_GEN/ALIVE bases. */
static void wake_src_read(unsigned int *stat, unsigned int *stat2,
			  unsigned int grp_stat[5], unsigned int grp_pend[5],
			  unsigned int eint[5])
{
	void __iomem *pmu = mapped_base(0x14060000);
	void __iomem *gen = mapped_base(0x14070000);
	void __iomem *alv = mapped_base(0x14050000);
	int n;

	*stat  = pmu ? readl_relaxed(pmu + 0x600) : 0;
	*stat2 = pmu ? readl_relaxed(pmu + 0x604) : 0;
	for (n = 0; n < 5; n++) {
		grp_stat[n] = gen ? readl_relaxed(gen + n * 0x100 + 0x08) : 0;
		grp_pend[n] = gen ? readl_relaxed(gen + n * 0x100 + 0x0c) : 0;
	}
	for (n = 0; n < (int)ARRAY_SIZE(eint_offset); n++)
		eint[n] = alv ? readl_relaxed(alv + 0xa00 + eint_offset[n]) : 0;
}

/* abox_syspower_test: write 1 to zero ABOX_SYSPOWER_CTRL now (no sleep) and log
 * the MIF vote + SYSPOWER_STATUS before/after -- the live idle confirm that the
 * ABOX syspower write clears bit4. Breaks audio until a reboot. */
static int abox_syspower_test_set(const char *val, const struct kernel_param *kp)
{
	void __iomem *abox = mapped_base(0x17c50000);
	void __iomem *mif = mapped_base(0x02040000);
	u32 v0, v1, s0, s1;

	if (!abox || !mif)
		return -ENODEV;
	v0 = (readl_relaxed(mif + 0xf8ac) >> 16) & 0xff;
	s0 = readl_relaxed(abox + 0x14);
	writel_relaxed(0, abox + 0x10);
	wmb();
	mdelay(5);
	v1 = (readl_relaxed(mif + 0xf8ac) >> 16) & 0xff;
	s1 = readl_relaxed(abox + 0x14);
	pr_info("zrn-system-sleep: abox_syspower_test: vote 0x%02x->0x%02x syspower_status 0x%x->0x%x\n",
		v0, v1, s0, s1);
	return 0;
}
static const struct kernel_param_ops abox_syspower_test_ops = { .set = abox_syspower_test_set };
module_param_cb(abox_syspower_test, &abox_syspower_test_ops, NULL, 0200);

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

/* mif_release: ask the ACPM firmware to drop the AP's MIF DVFS vote before the
 * suspend SMC (DVFS channel 1, DEVFREQ_MIF via s9p_acpm_dvfs_sleep) so MIF can
 * latch down, and re-acquire it on resume. A DIFFERENT lever from
 * mif_gate_zero / the 0x0204f95c DID_MIF_ALWAYS_ON gate: both may be needed
 * together. The release IPC polls the APM ring and sleeps, so it runs in the
 * .prepare hook (begin_platform, IRQs on) -- never in the IRQs-off finisher,
 * which would hang -- and the re-acquire runs in the PM_POST_SUSPEND notifier
 * below (also IRQs on, process context). Default off. */
static bool mif_release;
module_param(mif_release, bool, 0644);
MODULE_PARM_DESC(mif_release, "ask ACPM to drop the AP MIF DVFS vote before the sleep SMC and re-acquire on resume (default off)");

static int wdt_notify(struct notifier_block *nb, unsigned long event, void *data)
{
	if (event == PM_POST_SUSPEND) {
		zrn_wdt_release(&wdt_cover);
		/* Re-acquire the MIF DVFS vote now the system is fully resumed
		 * (IRQs on, sleepable). The release in begin_platform() is paired
		 * with exactly this call; both gated by mif_release. */
		if (mif_release) {
			int ret = s9p_acpm_dvfs_sleep(false);

			pr_info("zrn-system-sleep: MIF re-acquire (dvfs_sleep false) = %d\n",
				ret);
		}
	}
	return NOTIFY_OK;
}
static struct notifier_block wdt_notifier = { .notifier_call = wdt_notify };

/* The S2MPS18's IRQ (gpa0-1, active low) as a wake source for the sleep only:
 * stock's s2mps18 driver makes it an EINT and enable_irq_wake()s it; ours has
 * no PMIC driver, so the pin is a plain input. Set it up with interrupts off,
 * just for the sleep, and put it back before Linux can see an unhandled EINT.
 * gpa0 CON 0x20, EINT_CON 0x700, EINT_MASK 0x900, EINT_PEND 0xa00. */
static bool pmic_eint;
static u32 pmic_saved[3], pmic_pend_after;
static bool pmic_set;
module_param(pmic_eint, bool, 0644);
module_param(pmic_pend_after, uint, 0444);
/* Stock requests the S2MPS18 IRQ IRQF_TRIGGER_LOW (vendor s2mps18_irq.c:311),
 * so EINT_CON is level-low (0), not falling edge (2). Level-low wakes at once
 * when the line is already low; falling edge never fires then. */
static bool pmic_eint_level;
module_param(pmic_eint_level, bool, 0644);
static unsigned long pmic_refused;
module_param(pmic_refused, ulong, 0444);
/* Stock DT gives the PMIC IRQ pin gpa0-1 pin-pud = <3> (pull-up); ours has
 * no pull (gpa0 PUD 0x14050028 pin-1 field read 0 on 2026-11-08), so the
 * open line could float once the SoC sleeps. pmic_eint_pull: pull-up for the
 * sleep, put back after. */
static bool pmic_eint_pull;
module_param(pmic_eint_pull, bool, 0644);
static u32 pmic_saved_pud;
static void pmic_eint_on(void)
{
	void __iomem *g = mapped_base(0x14050000);
	if (!pmic_eint)
		return;
	pmic_saved[0] = readl_relaxed(g + 0x20);
	pmic_saved[1] = readl_relaxed(g + 0x700);
	pmic_saved[2] = readl_relaxed(g + 0x900);
	pmic_saved_pud = readl_relaxed(g + 0x28);
	if (pmic_eint_pull)
		writel_relaxed((pmic_saved_pud & ~(0xfU << 4)) | (0x3U << 4), g + 0x28);
	writel_relaxed((pmic_saved[1] & ~(0x7U << 4)) | ((pmic_eint_level ? 0x0U : 0x2U) << 4), g + 0x700);
	writel_relaxed((pmic_saved[0] & ~(0xfU << 4)) | (0xfU << 4), g + 0x20);
	writel_relaxed(0x2, g + 0xa00);
	writel_relaxed(pmic_saved[2] & ~0x2U, g + 0x900);
	wmb();
	pmic_set = true;
}
static void pmic_eint_off(void)
{
	void __iomem *g = mapped_base(0x14050000);
	if (!pmic_set)
		return;
	pmic_pend_after = readl_relaxed(g + 0xa00);
	writel_relaxed(pmic_saved[2] | 0x2U, g + 0x900);
	writel_relaxed(pmic_saved[0], g + 0x20);
	writel_relaxed(pmic_saved_pud, g + 0x28);
	writel_relaxed(pmic_saved[1], g + 0x700);
	writel_relaxed(0x2, g + 0xa00);
	writel_relaxed(pmic_saved[2], g + 0x900);
	wmb();
	pmic_set = false;
}

/* stock_masks: PMU WAKEUP_MASK 0x610 exactly as the stock DT's SYS_SLEEP
 * column (0xd00f7e7e, USBL2 0xd00d7e7e: SoC RTC alarm bit 1 stays masked),
 * and EINT_WAKEUP_MASK2 0x61c left alone (exynos-powermode.c:633-644 writes
 * only 0x60c and the DT wakeup-masks). */
static bool stock_masks;
module_param(stock_masks, bool, 0644);
/* mux_osc: put the PERIC1 and FSYS0 user muxes on the oscillator for the
 * sleep, as they are when stock sleeps. */
static bool mux_osc;
module_param(mux_osc, bool, 0644);
/* eint_pend_clear=0: leave EINT_PEND as found at the SMC, as stock does */
static bool eint_pend_clear = true;
module_param(eint_pend_clear, bool, 0644);
/* apm_clk_stock: force the APM CMU's BUS_COMPONENT_DRCG_EN to its stock value
 * BEFORE the sleep SMC, so the APM always-on alive clock (OSCCLK) behaves
 * through MIF-down exactly as it does on stock.
 *
 * Root cause: full SYS_SLEEP with MIF powered down hangs on wake because the
 * ACPM firmware's MIF wake op-list blocks at WAIT TCXO_STATUS (PMU 0x140682e4
 * bit31) and WAIT EXT_REGULATOR_MIF_STATUS (0x14063624 bit0). Both status bits
 * are driven by PMU stabilization counters clocked by the APM alive clock
 * (OSCCLK), not TCXO. If that alive clock is not in its stock configuration
 * when MIF drops, it gates, the status bits never assert, and the firmware's
 * own SysTick timeout (same dead clock) never fires, so it spins forever.
 *
 * Stock keeps the APM CMU alive-clock config correct by writing 0xFFFFFFFF to
 * CMU_APM BUS_COMPONENT_DRCG_EN (0x14010000 + 0x104 = 0x14010104) at boot in
 * pmucal_lpm_init (flexpmu_cal_system_exynos9810.h:64) and again on every wake
 * in exit_sleep (flexpmu_cal_system_exynos9810.h:1241). CMU_APM is always-on,
 * so the boot write persists across stock sleeps. Our kernel never runs
 * pmucal_lpm_init, and our copy of this write lives only in the exit_sleep
 * table (sleep-tables.h:1126, applied on WAKE via run_table(2)) -- too late
 * for the firmware's own pre-handoff wake poll, and zero on a fresh boot until
 * the first sleep. Applying the exact stock write pre-SMC closes that gap.
 *
 * Minimal and unconditional-rows-only on purpose: only CMU_APM (0x14010000) is
 * always-on and on the alive path; the other DRCG CMUs belong to blocks that
 * are powered down in MIF-down sleep. Do NOT widen this to init_extra[] --
 * that table froze the phone at load. */
static bool apm_clk_stock;
module_param(apm_clk_stock, bool, 0644);
MODULE_PARM_DESC(apm_clk_stock,
	"write CMU_APM BUS_COMPONENT_DRCG_EN (0x14010104=0xFFFFFFFF) pre-SMC to keep OSCCLK alive for the ACPM TCXO_STATUS wake poll (0x140682e4)");

/* osc_pin: diagnostic. Pin the PMU oscillator on just before the SMC handoff --
 * OSC_CON0_OSC (0x14060b00) bit31 and OSCCLK_SLEEP_CONFIGURATION (0x14062d60)
 * bit0 -- the exact oscillator the firmware's MIF-down DOWN sequence gates
 * (root cause of the wake-poll clock death). Tests whether holding the osc on
 * keeps the stabilization counters / APM core clocked. The firmware's own DOWN
 * op may overwrite it, so it is a probe, not a fix. Default off; composes
 * independently with the other pre-SMC levers. */
static bool osc_pin;
module_param(osc_pin, bool, 0644);
MODULE_PARM_DESC(osc_pin,
	"pin the PMU oscillator (OSC_CON0_OSC 0x14060b00 bit31 + OSCCLK_SLEEP_CONFIGURATION 0x14062d60 bit0) pre-SMC to probe the firmware DOWN-sequence clock-gating (diagnostic, default off)");

static void restore_platform(void)
{
	void __iomem *pmu = mapped_base(0x14060000);
	int ret, i;
	if (!prepared)
		return;
	pmic_eint_off();
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
	fltcon_restore();
	/* The unbound cluster-0 watchdog must retain its boot-disabled state. */
	writel_relaxed(0, mapped_base(0x10050000));
	writel_relaxed(saved_wdt0[1], mapped_base(0x10050000) + 4);
	writel_relaxed(saved_wdt0[2], mapped_base(0x10050000) + 8);
	writel_relaxed(saved_wdt0[0], mapped_base(0x10050000));
	for (i = 0; i < ARRAY_SIZE(usi_base); i++)
		writel_relaxed(saved_usi[i], mapped_base(usi_base[i]) + usi_offset[i]);
	writel_relaxed(saved_eint, pmu + 0x60c);
	if (!stock_masks)
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
	g4_trace[0] = readl_relaxed(mapped_base(0x14070000) + 0x408);
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
	if (eint_filter_analog)
		fltcon_analog();
	saved_eint = readl_relaxed(pmu + 0x60c);
	saved_eint2 = readl_relaxed(pmu + 0x61c);
	saved_hint = readl_relaxed(pmu + 0x860);
	for (i = 0; i < ARRAY_SIZE(wake_offsets); i++) {
		/* Samsung's SYS_SICD column (exynos-powermode.c / zrn-sicd.c):
		 * idle-IP {0,0,0xff9003c0,0xfffffffe}, WAKEUP_MASK
		 * {0x400001e0,0,0,0}. EINT wakes (power key, PMIC RTC) stay on
		 * the separate EINT_WAKEUP_MASK 0x60c written below. */
		static const u32 sicd_idle[4] = { 0, 0, 0xff9003c0, 0xfffffffe };
		static const u32 sicd_wake[4] = { 0x400001e0, 0, 0, 0 };
		saved_wake[i] = readl_relaxed(pmu + wake_offsets[i]);
		saved_idle[i] = readl_relaxed(pmu + 0x3f0 + 4 * i);
		/* Stock SYS_SLEEP idle-IP masks are all ones. RTC alarm is
		 * wake-status bit1 (SPI31); retain the independent timed wake. */
		writel_relaxed(sicd_mode ? sicd_idle[i] : ~0U, pmu + 0x3f0 + 4 * i);
		writel_relaxed(sicd_mode ? sicd_wake[i] :
			       (i == 0 ? ((usbl2 ? 0xd00d7e7e : 0xd00f7e7e) & (stock_masks ? ~0U : ~BIT(1))) :
			       (i == 3 ? 0 : 0xffff00ff)), pmu + wake_offsets[i]);
	}
	writel_relaxed(eff, pmu + 0x60c);
	if (!stock_masks)
		writel_relaxed(~0U, pmu + 0x61c);
	writel_relaxed(sicd_mode ? 3 : (usbl2 ? 5 : 4), pmu + 0x860);
	g4_trace[1] = readl_relaxed(mapped_base(0x14070000) + 0x408);
	prepared = true;
	early = true;
	last_restore_error = 0;
	pmic_eint_on();
	ret = run_table(1, ZRN_SAVE);
	if (!ret && mux_osc) {
		/* Stock's drivers have these user muxes on the oscillator when it
		 * sleeps (2026-11-12 snapshot: all five read 0, ours 0x10). The
		 * save table above has their running values and puts them back. */
		static const struct { u32 base, off; } m[] = {
			{ 0x10800000, 0x100 }, { 0x10800000, 0x120 },	/* PERIC1 bus, ip */
			{ 0x11000000, 0x100 }, { 0x11000000, 0x180 },	/* FSYS0 bus, UFS */
			{ 0x11000000, 0x1e0 },				/* FSYS0 USB30DRD */
		};

		for (i = 0; i < ARRAY_SIZE(m); i++) {
			void __iomem *r = mapped_base(m[i].base) + m[i].off;

			writel_relaxed(readl_relaxed(r) & ~BIT(4), r);
		}
		udelay(100);
	}
	if (!ret && apm_clk_stock) {
		/* Stock CMU_APM BUS_COMPONENT_DRCG_EN pre-SMC. This mirrors the
		 * exact unconditional write our exit_sleep table applies on wake
		 * (sleep-tables.h:1126: base 0x14010000, off 0x104, value and
		 * mask both 0xFFFFFFFF -> a plain word write) and the stock
		 * pmucal_lpm_init/exit_sleep rows, but applied before the handoff
		 * so OSCCLK is in its stock state for the firmware's own wake
		 * poll on TCXO_STATUS (0x140682e4). See apm_clk_stock above. */
		writel_relaxed(0xFFFFFFFF, mapped_base(0x14010000) + 0x104);
		dsb(sy);
	}
	if (!ret && osc_pin) {
		/* Pin the PMU oscillator the firmware DOWN sequence would gate
		 * (OSC_CON0_OSC 0x14060b00), to test the clock-death mechanism.
		 * Both offsets are inside the already-mapped 0x14060000 region.
		 * The firmware's own DOWN op may overwrite it: diagnostic only. */
		writel_relaxed(readl_relaxed(mapped_base(0x14060000) + 0xb00) | BIT(31),
			       mapped_base(0x14060000) + 0xb00);	/* OSC_CON0_OSC bit31=1 */
		writel_relaxed(readl_relaxed(mapped_base(0x14060000) + 0x2d60) | BIT(0),
			       mapped_base(0x14060000) + 0x2d60);	/* OSCCLK_SLEEP_CONFIGURATION bit0=1 */
		dsb(sy);
	}
	if (!ret)
		ret = run_table(0, ZRN_HANDLE);
	wmb();
	g4_trace[2] = readl_relaxed(mapped_base(0x14070000) + 0x408);
	if (ret)
		restore_platform();
	return ret;
}

/* The live ACPM image copies debug MIF_en (0x95c) into its runtime
 * enable field: a zero bypasses MIF sequencing (Thumb offsets 0x9190,
 * 0x949a, 0x93e0). Hold zero throughout Linux's suspend preparation.
 * No firmware request counts, owner flags or sequencer state are edited. */
/* keep_mif_en: leave MIF_en at 1 throughout, as stock does (its boot value);
 * our zero hold may make ACPM skip DRAM retention preparation. */
static bool keep_mif_en;
module_param(keep_mif_en, bool, 0644);
/* sicd_mode: drive Samsung SICD (System Idle Clock Down) instead of SYS_SLEEP
 * through this backend's reliable last-core path. The SRAM flexpmu trace shows
 * SICD clocks MIF down and wakes every idle cycle; it does NOT run the DRAM
 * self-refresh teardown (dramini) that hangs SYS_SLEEP. PSCI state 0x00000001
 * (vs SYS_SLEEP 0x03000000), the SICD PMU column (idle-IP + WAKEUP_MASK
 * 0x400001e0 + CPU_INFORM 3), and MIF_en left at its boot value so ACPM runs
 * its native SICD MIF sequence rather than the gated teardown. */
module_param(sicd_mode, bool, 0644);
/* keep_mif_en with mif_sleep: never write MIF_en at all, as stock never does
 * (its boot value 1 stays; begin_platform still refuses anything but 0/1).
 * The SMC sequence's two stores then go to mif_en_dummy instead. */
static u32 mif_en_dummy;
/* mif_gate_zero: THE FIX (RE-confirmed). 0x0204f95c is ACPM DID_MIF_ALWAYS_ON:
 * 0 = ACPM runs enter_mif (latches MIF down AND clears the AP's own MIF vote,
 * bit4); 1 = mif_always_on = ACPM SKIPS enter_mif (no MIF-down, bit4 stuck).
 * Stock leaves it at ACPM boot value 0 and never writes it. ztOS's SMC inline
 * asm wrote enable=mif_sleep(=1) to it right before the SMC, forcing always-on =
 * the single cause of mifdn=0 and bit4 stuck. When set: hold_mif writes 0, the
 * pre-SMC asm store writes 0 (enable forced 0), and the post-SMC keep_mif_en=1
 * restore is skipped -- so the real gate is 0 through the whole SMC, matching
 * stock. Default off. */
static bool mif_gate_zero;
module_param(mif_gate_zero, bool, 0644);
MODULE_PARM_DESC(mif_gate_zero, "force MIF_en 0x0204f95c to 0 at the SMC so ACPM runs enter_mif (RE-confirmed MIF-down fix); default off");
static bool mif_en_untouched(void)
{
	if (mif_gate_zero)
		return false;	/* drive the REAL gate to 0, not the dummy */
	return (keep_mif_en && mif_sleep) || sicd_mode;
}
static void hold_mif(void)
{
	if (mif_gate_zero) {
		writel_relaxed(0, mapped_base(0x02040000) + 0xf95c);	/* ACPM boot value 0 */
		dsb(sy);
		return;
	}
	if (mif_en_untouched())
		return;
	/* MIF_en (0x0204f95c) is the ACPM DID "MIF_en" gate; its working value is 1
	 * (the ACPM boot default), RE+live confirmed: ent_mif needs gate!=0 to run
	 * the MIF power-down. Leave it at 1 here, never clear it (stock never does). */
	writel_relaxed(1, mapped_base(0x02040000) + 0xf95c);
	dsb(sy);
}

/* mif_gate_probe: SAFE dry-run of the MIF_en gate logic -- replicate exactly what
 * the finisher/SMC does to 0x0204f95c (hold_mif + the pre-SMC store of enable to
 * the real-or-dummy gate), read the REAL gate value at that instant, log it, then
 * restore to 0 (as the post-SMC asm does). NO suspend, NO smc, NO wfi. Confirms
 * the gate value at the SMC point: 0 with mif_gate_zero, 1 (the bug) without. */
static int mif_gate_probe_set(const char *val, const struct kernel_param *kp)
{
	void __iomem *g = mapped_base(0x02040000) + 0xf95c;
	bool untouched = mif_en_untouched();
	unsigned long enable = mif_gate_zero ? 0 : 1;
	u32 before, at_smc;

	if (!g)
		return -ENODEV;
	before = readl_relaxed(g);
	if (mif_gate_zero)
		writel_relaxed(0, g);				/* hold_mif */
	else if (!untouched)
		writel_relaxed(keep_mif_en ? 1 : 0, g);		/* hold_mif */
	if (!untouched)
		writel_relaxed((u32)enable, g);			/* pre-SMC asm store (real gate) */
	wmb();
	at_smc = readl_relaxed(g);
	writel_relaxed(0, g);					/* post-SMC asm str wzr */
	wmb();
	pr_info("zrn-system-sleep: mif_gate_probe: REAL gate 0x0204f95c AT-SMC=%u (before=%u enable=%lu untouched=%d mif_gate_zero=%d mif_sleep=%d keep_mif_en=%d) -> restored 0\n",
		at_smc, before, enable, untouched, mif_gate_zero, mif_sleep, keep_mif_en);
	return 0;
}
static const struct kernel_param_ops mif_gate_probe_ops = { .set = mif_gate_probe_set };
module_param_cb(mif_gate_probe, &mif_gate_probe_ops, NULL, 0200);

static int begin_platform(void)
{
	mif_gate_before = readl_relaxed(mapped_base(0x02040000) + 0xf95c);
	if (mif_gate_before > 1)
		return -EINVAL;
	hold_mif();
	/* The ACPM MIF sleep-release IPC that drops the AP's "MIF req_Master"
	 * bit (0x0204f8ac bit4) now lives in prepare_late_platform(), NOT here.
	 * Sent from .prepare (this hook) it raced: CPUs/cpufreq/the DVFS manager
	 * are still live, and a MIF DVFS event between here and the finisher
	 * re-set the AP bit -> ACPM declined MIF-down (mif_requests_before=0x10,
	 * smc=-1/0x01100000). Stock sends it from devfreq .suspend_late; our
	 * .prepare_late runs just after that phase, after DVFS is quiesced. */
	return 0;
}

/* .prepare_late: runs after dpm_suspend_end (devices + the DVFS manager's
 * .suspend_late are done), IRQs still on and sleepable. This is where stock
 * drops the AP MIF vote, so nothing re-raises it before the IRQs-off finisher
 * reads mif_requests_before. Paired with the PM_POST_SUSPEND re-acquire. */
static int prepare_late_platform(void)
{
	if (mif_release) {
		int ret = s9p_acpm_dvfs_sleep(true);

		pr_info("zrn-system-sleep: MIF sleep-release (dvfs_sleep true, .prepare_late) = %d\n",
			ret);
	}
	return 0;
}

/* Clean all of DRAM to the point of coherency by VA. Ported verbatim in intent
 * from the 9820 deep driver's sleep_clean_ram(). Called with IRQs off and one
 * CPU online, before the firmware handoff. for_each_mem_range walks
 * memblock.memory; dcache_clean_poc cleans without invalidating (the data is
 * still live for resume). Placed before the one-cache-line SMC window and
 * before the APM watchdog is armed, so its latency (tens of ms over full DRAM)
 * is not inside either critical section. */
static void sleep_clean_ram(void)
{
	phys_addr_t start, end;
	u64 i, t0;

	if (!clean_ram)
		return;
	t0 = __arch_counter_get_cntvct();
	/* NOTE: dcache_clean_poc / for_each_mem_range are not exported to
	 * modules on this kernel, so the by-VA DRAM clean is stubbed out for
	 * the loadable-module build. Confirm cluster collapse via the apsoc
	 * counter first; if resume needs the clean, export the symbols in-tree
	 * (EXPORT_SYMBOL) or build this driver built-in and restore the loop:
	 *   for_each_mem_range(i, &start, &end)
	 *     dcache_clean_poc(__va(start), __va(end));
	 */
	(void)start; (void)end; (void)i;
	clean_ram_us = div_u64((__arch_counter_get_cntvct() - t0) * USEC_PER_SEC,
			       arch_timer_get_cntfrq());
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
	bool untouched = mif_en_untouched();
	void __iomem *gate = untouched ? (void __force __iomem *)&mif_en_dummy :
				mapped_base(0x02040000) + 0xf95c;
	void __iomem *grp4 = mapped_base(0x14070000) + 0x408;
	unsigned long enable = mif_gate_zero ? 0 : 1;	/* 1 = MIF_en enabled (ACPM default); RE+live confirmed MIF-down needs gate=1 */
	unsigned int b, low = 0, grp4_now;
	int hub_line = -1;
	int i, ret = prepare_platform();
	if (ret)
		return ret;
	if (prepare_only) {
		restore_platform();
		return -EAGAIN;
	}
	/* snapshot_abort: read the zrn-snapshot.h registers here, where Samsung's
	 * kernel is when it calls arm_cpuidle_suspend (after its syscore enter +
	 * save tables), then give the sleep up instead of entering it. */
	if (snapshot_abort && snap_awake.taken) {
		zrn_snap_take(&snap_sleep);
		restore_platform();
		return -EAGAIN;
	}
	/* The S2MPS18 IRQ (gpa0-1, DAT 0x14050024 bit 1) is active low and
	 * stays low until INT1-7 are read. Low now: a falling-edge EINT would
	 * never fire and the PMIC RTC alarm could not wake us. Refuse. */
	if (pmic_eint && !(readl_relaxed(mapped_base(0x14050000) + 0x24) & BIT(1))) {
		pmic_refused++;
		pr_info("zrn-system-sleep: refused: PMIC IRQ line (gpa0-1) low\n");
		restore_platform();
		return -EBUSY;
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
		if (!eint_pend_clear)
			continue;
		if (i < 4 && (eint_before[i] & (low >> (i * 8)) & 0xff))
			hub_pend_cleared++;
		writel_relaxed(eint_before[i] & mask, mapped_base(0x14050000) + 0xa00 + eint_offset[i]);
	}
	wmb();
	for (i = 0; i < 16; i++)
		pending_before[i] = readl_relaxed(mapped_base(0x10100000) + 0x1200 + i * 4) & readl_relaxed(mapped_base(0x10100000) + 0x1100 + i * 4);
	/* modem_suspend's pda_active:0 interrupt makes the CP raise its MIF
	 * request (bit 0) while it handles it; it drops it about 100 ms later.
	 * Stock's slower suspend path leaves that time; ours must wait, or
	 * ACPM finds a live CP request and keeps MIF up. */
	cp_settle_waited_us = 0;
	while (((readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0x01) &&
	       cp_settle_waited_us < cp_settle_ms * 1000) {
		udelay(500);
		cp_settle_waited_us += 500;
	}
	/* Directly drop the AP's MIF req_Master bit (bit 16+4) here in the IRQs-off
	 * finisher -- deterministic, nothing can re-raise it now (the DVFS IPC left
	 * it set). With bit1 (AUD) already dropped by the ABOX fix, ACPM then sees a
	 * zero AP MIF vote and latches MIF down. */
	if (mif_ap_clear) {
		void __iomem *mifsf = mapped_base(0x02040000) + 0xf8ac;

		writel_relaxed(readl_relaxed(mifsf) & ~0x00100000u, mifsf);
		wmb();
	}
	if (mif_force_release) {
		void __iomem *b = mapped_base(0x02040000);

		writel_relaxed(0, b + 0xb268);	/* granted mask (authoritative) = 0 */
		writel_relaxed(0, b + 0xb26c);	/* request bitmap = 0 */
		writel_relaxed(0, b + 0xb264);	/* req_count = 0 -> MIF-down eligible */
		writel_relaxed(1, b + 0xb260);	/* MIF_state = 1 (down-eligible) */
		wmb();
	}
	mif_requests_before = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
	/* DID_MIF_COUNT_SLEEP (0x0204f93c): ACPM's completed MIF-down count, same as
	 * the vendor acpm_get_mifdn_count(). Read before the SMC; if it does NOT
	 * increment across the sleep, MIF never latched down -- a master was still
	 * voting (mif_requests_before names it; bit4=0x10=ABOX/AUD). */
	mifdn_before = readl_relaxed(mapped_base(0x02040000) + 0xf97c);	/* mifcntSL (DID23); 0xf93c was IPC_vts1/dead */
	apsoc_before = readl_relaxed(mapped_base(0x02040000) + 0xf96c);	/* DID_AP_COUNT_SLEEP down (9810 APSOC-down proof) */
	if (gicc_off) {
		gicc_saved = readl_relaxed(mapped_base(0x10100000) + 0x2000);
		writel_relaxed(0, mapped_base(0x10100000) + 0x2000);
		dsb(sy);
		udelay(5);
	}
	/* gicd_off: turning the CPU interface off (gicc_off) does not stop the GIC
	 * distributor from asserting the SoC wake request (WAKEUP_STAT bit 31) for
	 * a pending SPI -- orphan level SPIs (hwirq 221/229/236, unclaimed in the
	 * DT) stay asserted and wake every sleep in ~50 ms. Disable the distributor
	 * (GICD_CTLR 0x10101000) across the SMC so nothing forwards; the real wake
	 * (SoC RTC alarm, a PMU-direct source) is unaffected. Restored on resume. */
	if (gicd_off) {
		gicd_saved = readl_relaxed(mapped_base(0x10100000) + 0x1000);
		writel_relaxed(0, mapped_base(0x10100000) + 0x1000);
		dsb(sy);
		udelay(5);
	}
	if (display_quiet) {
		void __iomem *dsim = mapped_base(0x16080000);
		void __iomem *dpp  = mapped_base(0x16020000) + 0x1000;	/* 0x16021000 */
		void __iomem *idma = mapped_base(0x16070000) + 0x1000;	/* 0x16071000 */
		void __iomem *gicd = mapped_base(0x10100000) + 0x1000;
		int b;
		/* mask every DSIM interrupt, clear its source (both w1c) */
		dq_dsim_intmsk = readl_relaxed(dsim + 0x54);
		writel_relaxed(0xffffffff, dsim + 0x54);
		writel_relaxed(0xffffffff, dsim + 0x50);
		/* DPP + IDMA: drop the IRQ enable and clear latched status (w1c) */
		dq_dpp_irq  = readl_relaxed(dpp + 0x00);
		dq_idma_irq = readl_relaxed(idma + 0x00);
		writel_relaxed(0, dpp + 0x00);
		writel_relaxed(readl_relaxed(dpp + 0x04),  dpp + 0x04);
		/* clear every IDMA channel (0x16071000 + 0x1000*ch, ch 0..5) */
		for (b = 0; b < 6; b++) {
			void __iomem *ch = mapped_base(0x16070000) + 0x1000 + b * 0x1000;
			writel_relaxed(0, ch + 0x00);
			writel_relaxed(readl_relaxed(ch + 0x04), ch + 0x04);	/* clears DPP hwirq 229 */
			writel_relaxed(readl_relaxed(ch + 0x08), ch + 0x08);
			writel_relaxed(readl_relaxed(ch + 0x0c), ch + 0x0c);
		}
		/* DMA common block (0x16070000, page 0): w1c its status regs -- this is
		 * what actually clears the DPP IRQ hwirq 229 (the per-channel pages
		 * above do not). */
		{
			void __iomem *dmac = mapped_base(0x16070000);
			int o;
			for (o = 0; o < 0x20; o += 4)
				writel_relaxed(readl_relaxed(dmac + o), dmac + o);
		}
		/* DECON0 frame IRQ (hwirq 214/215): mask + w1c pending */
		{
			void __iomem *decon = mapped_base(0x16030000);
			dq_decon_inten = readl_relaxed(decon + 0x40);
			writel_relaxed(0, decon + 0x40);
			writel_relaxed(0xffffffff, decon + 0x4c);
			writel_relaxed(0xffffffff, decon + 0x50);
		}
		/* UART (hwirq 429): mask UINTM + w1c UINTP */
		{
			void __iomem *uart = mapped_base(0x10440000);
			dq_uart_uintm = readl_relaxed(uart + 0x38);
			writel_relaxed(0xf, uart + 0x38);
			writel_relaxed(0xf, uart + 0x30);
		}
		wmb();
		/* clear the now-quiet sources from GIC pending (ICPENDR 0x280):
		 * 214/215/221 (reg6 bit22/23/29), 229/236 (reg7 bit5/12),
		 * 429 (reg13 bit13), 486 (reg15 bit6) */
		writel_relaxed((1u << 22) | (1u << 23) | (1u << 29), gicd + 0x280 + 6 * 4);
		writel_relaxed((1u << 5) | (1u << 12), gicd + 0x280 + 7 * 4);
		writel_relaxed(1u << 13, gicd + 0x280 + 13 * 4);
		writel_relaxed(1u << 6, gicd + 0x280 + 15 * 4);
		wmb();
	}
	/* count + name SPIs still pending at the SMC (ISPENDR 0x200, SPI regs 1..31) */
	gic_pend = 0;
	{
		void __iomem *gicd = mapped_base(0x10100000) + 0x1000;
		char buf[160];
		int n = 0, b;
		buf[0] = '\0';
		for (i = 1; i < 32; i++) {
			u32 v = readl_relaxed(gicd + 0x200 + i * 4);
			gic_pend += hweight32(v);
			while (v) {
				b = __ffs(v);
				v &= ~(1u << b);
				if (n < 150)
					n += scnprintf(buf + n, sizeof(buf) - n, " %u", i * 32 + b);
			}
		}
		pr_info("zrn-system-sleep: gic_pend=%u hwirqs:%s\n", gic_pend, buf);
	}
	grp4_before = readl_relaxed(grp4);
	chub_status_before = readl_relaxed(mapped_base(0x14060000) + 0x4084);
	grp4_cleared = 0;
	/* the followed lines are low by now (else refused above) */
	if (grp4_before && (clear_grp4 || grp4_stale_clear)) {
		/* grp4_bid_off: the vendor sleep-up order (flexpmu_cal_system_exynos9810.h)
		 * DISABLES the group before clearing -- GRP4_INTR_BID_ENABLE (0x14070400)
		 * bit0=0 then PMU_EVENT_INTERRUPT_ENABLE_GRP4 (0x14060000+0x7f10) bit0=0 --
		 * so the (APM-internal / alive-domain) source stops feeding GRP4, then the
		 * UPEND clear sticks. We only ever wrote CLEAR, which re-latched. */
		if (grp4_bid_off) {
			void __iomem *g = mapped_base(0x14070000);
			void __iomem *p = mapped_base(0x14060000);
			writel_relaxed(readl_relaxed(g + 0x400) & ~1u, g + 0x400);
			writel_relaxed(readl_relaxed(p + 0x7f10) & ~1u, p + 0x7f10);
			wmb();
		}
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
	g4_trace[3] = grp4_cleared;
	g4_relatch = -1;
	for (i = 0; i < 4000; i++) {
		if (readl_relaxed(grp4)) { g4_relatch = i; break; }
		udelay(1);
	}
	grp4_now = readl_relaxed(grp4);
	/* Keep this before enabling the MIF gate: retained console records must
	 * distinguish a missing AP request from corruption of earlier log text. */
	pr_info("zrn-system-sleep: handoff mif_sleep=%u requests=%02x gate=%u grp4=%08x->%08x chub=%08x eint_eff=%08x hub_line=%d grp4_at_smc=%08x cp_wait=%uus gic_pend=%u\n",
		mif_sleep, mif_requests_before,
		readl_relaxed(mapped_base(0x02040000) + 0xf95c), grp4_before, grp4_cleared,
		chub_status_before, eint_effective, hub_line, grp4_now, cp_settle_waited_us, gic_pend);
	if (pd_log) {
		u32 f[ARRAY_SIZE(fltcon_offset)];

		for (i = 0; i < PD_STATUS_COUNT; i++)
			pd_status[i] = readl_relaxed(mapped_base(0x14060000) + 0x4004 + 8 * i);
		fltcon_read(f);
		pr_info("zrn-system-sleep: handoff pd 4004.. %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			pd_status[0], pd_status[1], pd_status[2], pd_status[3], pd_status[4],
			pd_status[5], pd_status[6], pd_status[7], pd_status[8], pd_status[9],
			pd_status[10], pd_status[11], pd_status[12], pd_status[13], pd_status[14],
			pd_status[15], pd_status[16], pd_status[17], pd_status[18]);
		pr_info("zrn-system-sleep: handoff fltcon %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
			f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
	}
	/* again, after the console: what ACPM will find (zrn-sleepd's fw-abort) */
	grp4_at_smc = readl_relaxed(grp4);
	wake_src_read(&ws_stat_smc, &ws_stat2_smc, ws_grp_stat_smc,
		      ws_grp_pend_smc, ws_eint_smc);
	pr_info("zrn-system-sleep: AT-SMC ws=%08x/%08x grp_stat=%08x %08x %08x %08x %08x grp_pend=%08x %08x %08x %08x %08x eint=%08x %08x %08x %08x %08x\n",
		ws_stat_smc, ws_stat2_smc,
		ws_grp_stat_smc[0], ws_grp_stat_smc[1], ws_grp_stat_smc[2], ws_grp_stat_smc[3], ws_grp_stat_smc[4],
		ws_grp_pend_smc[0], ws_grp_pend_smc[1], ws_grp_pend_smc[2], ws_grp_pend_smc[3], ws_grp_pend_smc[4],
		ws_eint_smc[0], ws_eint_smc[1], ws_eint_smc[2], ws_eint_smc[3], ws_eint_smc[4]);
	/* Keep the enable/SMC/abort-disable instructions in one cache line.
	 * x19 is preserved by the secure-call ABI. Do not call or delay in
	 * the interval where ACPM is permitted to remove DRAM power. */
	/* Clean all DRAM to PoC last, before the watchdog and the SMC window: the
	 * other CPUs are long offline and nothing dirties RAM past this point, so
	 * the firmware finds every coherent cache written back if it collapses the
	 * clusters. Gated by clean_ram (default off). */
	sleep_clean_ram();
	wdt_apm_arm();
	a0 = psci_system ? 0xc400000eUL : 0xc4000001UL;
	a1 = psci_system ? resume : state;
	a2 = psci_system ? 0 : resume;
	a3 = a4 = a5 = a6 = a7 = 0;
	/* Final GRP4 pending clear, immediately before the SMC: a transient bit0
	 * pulse can appear after grp4_at_smc is read (even with no GIC/EINT source),
	 * and the firmware's upend check at sleep-up then aborts. 0x1407040c is the
	 * AP-safe write-1-then-0 clear (not the APM-owned BID/enable). */
	if (grp4_smc_clear) {
		writel_relaxed(1, mapped_base(0x14070000) + 0x40c);
		wmb();
		writel_relaxed(0, mapped_base(0x14070000) + 0x40c);
		wmb();
	}
	/* Stock-parity GRP0..GRP3 UPEND clear, immediately before the SMC. The stock
	 * exynos CPU-PM notifier clears every group's UPEND per-CPU at hand-off;
	 * ztOS only ever cleared GRP4, leaving a non-GIC GRP0/1 UPEND latched that
	 * aborts the MIF-down sleep. Read status (+0x08), write-1-to-clear (+0x0c). */
	if (grp_lo_clear) {
		void __iomem *g = mapped_base(0x14070000);
		int n;

		for (n = 0; n < 4; n++) {
			u32 up = readl_relaxed(g + n * 0x100 + 0x08);

			if (up) {
				writel_relaxed(up, g + n * 0x100 + 0x0c);
				wmb();
				writel_relaxed(0, g + n * 0x100 + 0x0c);
				wmb();
			}
		}
	}
	/* Silence the MCT broadcast comparator so a scheduled tick cannot wake us
	 * in the MIF-down window (G_INT_ENB off, G_INT_CSTAT acked). dbg_mct maps
	 * 0x10040000 at probe. */
	if (mct_quiet && dbg_mct) {
		mct_int_enb_saved = readl_relaxed(dbg_mct + 0x248);
		writel_relaxed(0, dbg_mct + 0x248);			/* G_INT_ENB = 0 */
		writel_relaxed(readl_relaxed(dbg_mct + 0x244), dbg_mct + 0x244); /* ack CSTAT */
		wmb();
	}
	/* Clear every pending GIC SPI right before the SMC (superset of display_quiet). */
	if (gic_clear_late) {
		void __iomem *gicd = mapped_base(0x10100000) + 0x1000;
		int w;
		for (w = 1; w < 32; w++)
			writel_relaxed(0xffffffff, gicd + 0x280 + w * 4);	/* ICPENDR */
		wmb();
	}
	/* Park the MCT tick comparator past the RTC alarm (value only; interrupt
	 * enable untouched, so ACPM does not decline). */
	if (mct_park && dbg_mct) {
		u64 cnt, park;

		mct_comp0_l_saved = readl_relaxed(dbg_mct + 0x200);
		mct_comp0_u_saved = readl_relaxed(dbg_mct + 0x204);
		cnt = readl_relaxed(dbg_mct + 0x100) |
		      ((u64)readl_relaxed(dbg_mct + 0x104) << 32);
		park = cnt + 0x400000000ULL;
		writel_relaxed((u32)park, dbg_mct + 0x200);		/* G_COMP0_L */
		writel_relaxed((u32)(park >> 32), dbg_mct + 0x204);	/* G_COMP0_U */
		writel_relaxed(0x3, dbg_mct + 0x24c);			/* ack G_WSTAT comp0 L/U */
		wmb();
	}
	/* Brute-drop ABOX's MIF vote: zero ABOX_SYSPOWER_CTRL (0x17c50010) so
	 * acpm_get_mif_request bit4 clears and ACPM can keep MIF down. Restored on
	 * resume. */
	if (abox_syspower_off && mapped_base(0x17c50000)) {
		abox_syspower_saved = readl_relaxed(mapped_base(0x17c50000) + 0x10);
		writel_relaxed(0, mapped_base(0x17c50000) + 0x10);
		wmb();
	}
	abox_vote_before = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
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
	mifdn_after = readl_relaxed(mapped_base(0x02040000) + 0xf97c);	/* mifcntSL (DID23); 0xf93c was IPC_vts1/dead */
	apsoc_after = readl_relaxed(mapped_base(0x02040000) + 0xf96c);	/* DID_AP_COUNT_SLEEP down (9810 APSOC-down proof) */
	abox_vote_after = (readl_relaxed(mapped_base(0x02040000) + 0xf8ac) >> 16) & 0xff;
	if (abox_syspower_off && mapped_base(0x17c50000))
		writel_relaxed(abox_syspower_saved, mapped_base(0x17c50000) + 0x10);	/* restore ABOX */
	/* Name the waker: read GICD ISPENDR (hwirq 0..511) the instant the SMC
	 * returns, BEFORE any GIC/display/MCT restore clears it. The later
	 * pending_after read (after restore_platform) always came back 0, so the
	 * source clears in between; this catches it. GICD_CTLR being off (gicd_off)
	 * does not clear the per-SPI pending latch. Also snapshot the PMU wake and
	 * GICC_HPPIR (0x2018, non-destructive) for cross-check. */
	if (wake_capture) {
		void __iomem *gicd = mapped_base(0x10100000) + 0x1000;
		void __iomem *gicc = mapped_base(0x10100000) + 0x2000;
		int wi;

		for (wi = 0; wi < 16; wi++)
			wake_ispend[wi] = readl_relaxed(gicd + 0x200 + wi * 4);
		wake_isactive0 = readl_relaxed(gicd + 0x300); /* ISACTIVER0 */
		wake_hppir = readl_relaxed(gicc + 0x18) & 0x3ff;
		wake_stat_early = readl_relaxed(mapped_base(0x14060000) + 0x600);
	}
	if (keep_mif_en && !untouched && !mif_gate_zero)
		writel_relaxed(1, gate);
	wdt_apm_disarm();
	if (gicc_off && gicc_saved)
		writel_relaxed(gicc_saved, mapped_base(0x10100000) + 0x2000);
	if (gicd_off && gicd_saved)
		writel_relaxed(gicd_saved, mapped_base(0x10100000) + 0x1000);
	if (mct_quiet && dbg_mct)
		writel_relaxed(mct_int_enb_saved, dbg_mct + 0x248);	/* restore G_INT_ENB */
	if (mct_park && dbg_mct) {
		writel_relaxed(mct_comp0_l_saved, dbg_mct + 0x200);	/* restore G_COMP0_L */
		writel_relaxed(mct_comp0_u_saved, dbg_mct + 0x204);	/* restore G_COMP0_U */
		writel_relaxed(0x3, dbg_mct + 0x24c);			/* ack G_WSTAT */
	}
	if (display_quiet) {
		writel_relaxed(dq_dsim_intmsk, mapped_base(0x16080000) + 0x54);
		writel_relaxed(dq_dpp_irq,  mapped_base(0x16020000) + 0x1000);
		writel_relaxed(dq_idma_irq, mapped_base(0x16070000) + 0x1000);
		writel_relaxed(dq_decon_inten, mapped_base(0x16030000) + 0x40);
		writel_relaxed(dq_uart_uintm,  mapped_base(0x10440000) + 0x38);
	}
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
	ret = suspend_fn(sicd_mode ? 0x00000001UL : (unsigned long)sleep_pstate, sleep_finisher);
	wdt_apm_disarm();
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
	wake_src_read(&ws_stat_wake, &ws_stat2_wake, ws_grp_stat_wake,
		      ws_grp_pend_wake, ws_eint_wake);
	pr_info("zrn-system-sleep: AT-WAKE ws=%08x/%08x grp_stat=%08x %08x %08x %08x %08x grp_pend=%08x %08x %08x %08x %08x eint=%08x %08x %08x %08x %08x\n",
		ws_stat_wake, ws_stat2_wake,
		ws_grp_stat_wake[0], ws_grp_stat_wake[1], ws_grp_stat_wake[2], ws_grp_stat_wake[3], ws_grp_stat_wake[4],
		ws_grp_pend_wake[0], ws_grp_pend_wake[1], ws_grp_pend_wake[2], ws_grp_pend_wake[3], ws_grp_pend_wake[4],
		ws_eint_wake[0], ws_eint_wake[1], ws_eint_wake[2], ws_eint_wake[3], ws_eint_wake[4]);
	early = ret != 0;
	if (!ret)
		entries++;
	/* Into the console log at once: a device resume that hangs afterward
	 * (UFS did) loses the module parameters, ramoops keeps this. */
	pr_info("zrn-system-sleep: smc=%d wake=%08x/%08x/%08x bid=%08x eint=%08x %08x %08x %08x %08x\n",
		ret, wake_stat, wake_stat2, wake_stat3, bid_pending,
		eint_pending[0], eint_pending[1], eint_pending[2], eint_pending[3], eint_pending[4]);
	pr_info("zrn-system-sleep: mifdn %u->%u (%s) mif_req=%02x vote %02x->%02x abox_syspower_saved=%x\n",
		mifdn_before, mifdn_after,
		mifdn_after != mifdn_before ? "MIF WENT DOWN" : "MIF BLOCKED (master still voting)",
		mif_requests_before, abox_vote_before, abox_vote_after, abox_syspower_saved);
	pr_info("zrn-system-sleep: apsoc-down %u->%u (%s) clean_ram=%d us=%u\n",
		apsoc_before, apsoc_after,
		apsoc_after != apsoc_before ? "SOC WENT DOWN (clusters collapsed)" : "SOC STAYED UP (STANDBY, not power-down)",
		clean_ram, clean_ram_us);
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
	if (wake_capture) {
		char buf[200];
		int n = 0, b;
		buf[0] = '\0';
		for (i = 0; i < 16; i++) {
			u32 v = wake_ispend[i];
			while (v) {
				b = __ffs(v);
				v &= ~(1u << b);
				if (n < 190)
					n += scnprintf(buf + n, sizeof(buf) - n, " %d", i * 32 + b);
			}
		}
		pr_info("zrn-system-sleep: WAKE-CAPTURE ispend hwirqs:%s isactive0=%08x hppir=%u wake_early=%08x\n",
			buf, wake_isactive0, wake_hppir, wake_stat_early);
	}
	/* Undo PMUCAL before CPU PM notifiers touch restored clocks. */
	restore_platform();
	cpu_cluster_pm_exit();
cpu_exit:
	cpu_pm_exit();
	return ret;
}
static const struct platform_suspend_ops platform_ops = {
	.valid = suspend_valid_only_mem, .prepare = begin_platform,
	.prepare_late = prepare_late_platform, .enter = enter_sleep_state,
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

/* stock_init_extra: the vendor pmucal_lpm_init rows (flexpmu_cal_system_
 * exynos9810.h:39-108) that init-memory.h leaves out and that touch only
 * always-on blocks, plus the BUS_COMPONENT_DRCG_EN rows, written the way the
 * exit table (already run after every AP-only wake) writes them: on a fresh
 * boot nothing has written them yet. Not here: PS_HOLD (matches live), the
 * G3D and AUD rows, DCF/DCPOST/DCRD power-off (zrn-pd / DOMS). */
static bool stock_init_extra;
module_param(stock_init_extra, bool, 0444);
static const struct zrn_init_setting init_extra[] = {
	/* Left out on purpose: RESET_OTP_CHUB_SYS_PWR_REG (sensor hub reset path)
	 * and the full-width CPUCL0/1 SHORTSTOP writes, excluded since 16 Oct
	 * because their reserved-bit readback masks are unknown. */
	{ 0x14060000, 0x0a08, 0x00000003, 0x00000002, "BURNIN_CTRL" },
};
static int apply_init_extra(void)
{
	for (int i = 0; i < ARRAY_SIZE(init_extra); i++) {
		const struct zrn_init_setting *s = &init_extra[i];
		void __iomem *reg = mapped_base(s->base) + s->offset;
		u32 old = readl_relaxed(reg);

		writel_relaxed((old & ~s->mask) | s->value, reg);
		pr_info("zrn-sleep-init: %s %08x -> %08x\n", s->name, old, readl_relaxed(reg));
	}
	dsb(sy);
	return run_table(2, ZRN_HANDLE);	/* exit_sleep: DRCG_EN rows */
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
	map_base(0x17c50000);	/* ABOX SFR, for ABOX_SYSPOWER_CTRL (MIF-vote release) */
	map_base(0x02040000);	/* DREX/MIF for the acpm_get_mif_request / mifdn reads */
	zrn_snapshot_setup();
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
	ret = map_base(0x14030000);
	if (ret)
		goto fail;
	ret = map_base(0x14050000);
	if (ret)
		goto fail;
	/* display: DSIM 0x16080000, DPP 0x16020000 (+0x1000 SFR), IDMA 0x16070000
	 * (+0x1000 ch0) -- for display_quiet to mask their IRQs before the SMC. */
	ret = map_base(0x16080000);
	if (ret)
		goto fail;
	ret = map_base(0x16020000);
	if (ret)
		goto fail;
	ret = map_base(0x16070000);
	if (ret)
		goto fail;
	ret = map_base(0x16030000);	/* DECON0 */
	if (ret)
		goto fail;
	ret = map_base(0x10440000);	/* UART (hwirq 429) */
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
		if (!ret && stock_init_extra)
			ret = apply_init_extra();
		if (!ret && cpu_shortstop) {
			/* The 9810 qch-analog: the full-width CPUCL0/1 SHORTSTOP rows
			 * from pmucal_lpm_init that init-memory.h omits. 0x1d000000 and
			 * 0x1d100000 are already mapped by the init_settings pass. */
			writel_relaxed(0xffffffff, mapped_base(0x1d000000) + 0x820);	/* CPUCL0_SHORTSTOP */
			writel_relaxed(0xffffffff, mapped_base(0x1d100000) + 0x820);	/* CPUCL1_SHORTSTOP */
			dsb(sy);
			pr_info("zrn-sleep-init: cpu_shortstop applied (CPUCL0/1 0x820 = 0xffffffff)\n");
		}
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
