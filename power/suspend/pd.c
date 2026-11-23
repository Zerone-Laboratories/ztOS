// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic Exynos9810 multimedia/audio power-domain driver (starlte).
 *
 * SYS_SLEEP can only reach a clean MIF-down if the SoC media/audio power
 * domains are off the way stock's genpd leaves them: the vendor PMUCAL
 * save/quiesce table (Q-channel + SysMMU stop) runs, the EL3 monitor saves the
 * domain's TrustZone protection, and only then the PMU CONFIGURATION field is
 * cleared. A raw PMU power-off that skips the save table is what hard-reset
 * DSPM and cluster-WDT-hung G2D/MFC/DSPS.
 *
 * This module mirrors the vendor pmucal_local_{enable,disable} exactly, driven
 * by the proven zrn_sequence_run() engine over the transcribed vendor tables
 * (pd-tables.h). It is deliberately standalone: it is NOT a genpd provider and
 * is NOT wired into the sleep path. It changes nothing on load. The operator
 * powers one domain off at a time by hand (screen blanked, no sleep) through
 * the enable_off bitmask, exactly as the G3D domain was proven before it was
 * trusted across suspend.
 *
 *   power-off(d): low_power_init(d) once; run <d>_save (SAVE: capture +
 *                 quiesce); EL3 TZ save SMC; run <d>_off (CONFIGURATION=0,
 *                 wait STATUS==0). On timeout: roll the domain back on,
 *                 restore, self-veto; panic only if the rollback itself fails.
 *   power-on(d):  run <d>_on (CONFIGURATION=0xf, wait STATUS==0xf); EL3 TZ
 *                 restore SMC; run <d>_save (RESTORE: write saved values back).
 */
#include <linux/arm-smccc.h>
#include <linux/syscore_ops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include "sequence.h"
#include "pd-tables.h"

#define ZRN_PD_PMU_BASE		0x14060000
#define ZRN_PD_PWR_CFG		0xf		/* CONFIGURATION/STATUS field */
#define ZRN_PD_STATUS(cfg)	((cfg) + 4)

#define SMC_CMD_PREPARE_PD_ONOFF	0x82000410
#define EXYNOS_GET_IN_PD_DOWN		0	/* save protection, in power-down */
#define EXYNOS_WAKEUP_PD_DOWN		1	/* restore protection, on wake */

/* pmucal_lpm_init rows (flexpmu_cal_system_exynos9810.h) that apply per domain:
 * the SRAM retention option, plus AUD's CPU option and half-cycle clock mux.
 * sboot leaves these at reset values; the vendor init table sets them, and the
 * domain must not be powered off without them (the G3D case, #155 vs #160). */
struct zrn_pd_init { u32 base, offset, mask, value; };
static const struct zrn_pd_init aud_init[] = {
	{ 0x14060000, 0x3808, 0x1u << 16, 0x1u << 16 },	/* AUD_CPU_OPTION */
	{ 0x17C00000, 0x0850, 0x1f, 0x01 },		/* AUD_HCHGEN_CLKMUX */
};
static const struct zrn_pd_init dpu_init[]    = { { 0x14060000, 0x4404, 0x3, 0x2 } };
static const struct zrn_pd_init dspm_init[]   = { { 0x14060000, 0x44a4, 0x3, 0x2 } };
static const struct zrn_pd_init dsps_init[]   = { { 0x14060000, 0x4544, 0x3, 0x2 } };
static const struct zrn_pd_init g2d_init[]    = { { 0x14060000, 0x45e4, 0x3, 0x2 } };
static const struct zrn_pd_init mfc_init[]    = { { 0x14060000, 0x49b0, 0x3, 0x2 } };
static const struct zrn_pd_init iva_init[]    = { { 0x14060000, 0x4910, 0x3, 0x2 } };
static const struct zrn_pd_init isppre_init[] = { { 0x14060000, 0x4730, 0x3, 0x2 } };
static const struct zrn_pd_init isphq_init[]  = { { 0x14060000, 0x47d0, 0x3, 0x2 } };
static const struct zrn_pd_init isplp_init[]  = { { 0x14060000, 0x4870, 0x3, 0x2 } };
static const struct zrn_pd_init dcf_init[]    = { { 0x14060000, 0x4224, 0x3, 0x2 } };
static const struct zrn_pd_init dcpost_init[] = { { 0x14060000, 0x42c4, 0x3, 0x2 } };
static const struct zrn_pd_init dcrd_init[]   = { { 0x14060000, 0x4364, 0x3, 0x2 } };
/* VTS carries four SRAM-retention options in pmucal_lpm_init (the PCM, code and
 * data voice-trigger SRAM banks, plus the block option), not the single
 * MEMORY_<DOM>_OPTION the other domains have. All are plain masked writes to the
 * always-on PMU, exactly as flexpmu_cal_system_exynos9810.h:6-8,22 sets them. */
static const struct zrn_pd_init vts_init[] = {
	{ 0x14060000, 0x5008, 0x3, 0x2 },	/* MEMORY_PCM_VTS_OPTION */
	{ 0x14060000, 0x5018, 0x3, 0x2 },	/* MEMORY_CODE_VTS_OPTION */
	{ 0x14060000, 0x5028, 0x3, 0x2 },	/* MEMORY_DATA_VTS_OPTION */
	{ 0x14060000, 0x4af4, 0x3, 0x2 },	/* MEMORY_VTS_OPTION */
};
/* CHUB keeps six SRAM-retention banks (flexpmu_cal_system_exynos9810.h:23-28).
 * The vendor sets them in system init; sboot leaves them at reset. Set them
 * before power-off so STATUS can settle to 0 (the G3D #155/#160 partial-STATUS
 * lesson). Plain masked writes to the always-on PMU. */
static const struct zrn_pd_init chub_init[] = {
	{ 0x14060000, 0x4b80, 0x3, 0x2 },	/* MEMORY_2_CHUB_OPTION */
	{ 0x14060000, 0x4b90, 0x3, 0x2 },	/* MEMORY_4_CHUB_OPTION */
	{ 0x14060000, 0x4bac, 0x3, 0x2 },	/* MEMORY_5_CHUB_OPTION */
	{ 0x14060000, 0x4bbc, 0x3, 0x2 },	/* MEMORY_1_CHUB_OPTION */
	{ 0x14060000, 0x4bcc, 0x3, 0x2 },	/* MEMORY_CHUB_OPTION */
	{ 0x14060000, 0x4c28, 0x3, 0x2 },	/* MEMORY_3_CHUB_OPTION */
};

struct zrn_pd_domain {
	const char *name;
	u32 config;		/* CONFIGURATION offset from ZRN_PD_PMU_BASE */
	u32 smc;		/* need_smc TrustZone restore address (DT pd node) */
	struct zrn_seq *on;	unsigned int n_on;
	struct zrn_seq *save;	unsigned int n_save;
	struct zrn_seq *off;	unsigned int n_off;
	const struct zrn_pd_init *init;	unsigned int n_init;
	bool lpm_done;
	bool off_by_us;
};

#define T(n)	(n), ARRAY_SIZE(n)
#define I(n)	(n), ARRAY_SIZE(n)
/* Domain-bit order. AUD is bit 0: the prime suspect (TWRP has no audio, our AUD
 * stays on at the handoff) and the first to validate at runtime. */
static struct zrn_pd_domain domains[] = {
	{ "AUD",    0x4000, 0x17c10204, T(aud_on),    T(aud_save),    T(aud_off),    I(aud_init) },
	{ "DPU",    0x4020, 0x16010204, T(dpu_on),    T(dpu_save),    T(dpu_off),    I(dpu_init) },
	{ "DSPM",   0x4028, 0x16c10204, T(dspm_on),   T(dspm_save),   T(dspm_off),   I(dspm_init) },
	{ "DSPS",   0x4030, 0x16f10204, T(dsps_on),   T(dsps_save),   T(dsps_off),   I(dsps_init) },
	{ "G2D",    0x4038, 0x17610204, T(g2d_on),    T(g2d_save),    T(g2d_off),    I(g2d_init) },
	{ "MFC",    0x4068, 0x17810204, T(mfc_on),    T(mfc_save),    T(mfc_off),    I(mfc_init) },
	{ "IVA",    0x4060, 0x17010204, T(iva_on),    T(iva_save),    T(iva_off),    I(iva_init) },
	{ "ISPPRE", 0x4048, 0x16210204, T(isppre_on), T(isppre_save), T(isppre_off), I(isppre_init) },
	{ "ISPHQ",  0x4050, 0x16610204, T(isphq_on),  T(isphq_save),  T(isphq_off),  I(isphq_init) },
	{ "ISPLP",  0x4058, 0x16410204, T(isplp_on),  T(isplp_save),  T(isplp_off),  I(isplp_init) },
	{ "DCF",    0x4008, 0x16a10204, T(dcf_on),    T(dcf_save),    T(dcf_off),    I(dcf_init) },
	{ "DCPOST", 0x4010, 0x16b10204, T(dcpost_on), T(dcpost_save), T(dcpost_off), I(dcpost_init) },
	{ "DCRD",   0x4018, 0x16810204, T(dcrd_on),   T(dcrd_save),   T(dcrd_off),   I(dcrd_init) },
	/* VTS: bit 13 (0x2000). CONFIG 0x4078 / STATUS 0x407c (pd-vts@14064070,
	 * reg base+8/+0xc); need_smc 0x13810204 (starlte DT). ON in ztOS but OFF
	 * in stock, so it holds the OSC/Q-channel request that sends the ACPM
	 * MIF-down DOWN sequence down the oscillator-gating branch. Its save/off
	 * tables quiesce the VTS<->CHUB async bridge at 0x13A00000, which is
	 * reachable only because CHUB is powered (it is, in ztOS).
	 *
	 * smc was 0x13810204 (starlte need_smc), but VTS's secure-RESTORE SMC
	 * (zrn_pd_tz EXYNOS_WAKEUP_PD_DOWN) raised an async SError 0xbe000011 in
	 * zrn_pd_power_on at resume, in BOTH MIF-down and plain AP sleep. Set to 0
	 * to skip the TZPC secure save+restore (as CHUB does) and isolate whether
	 * that SMC is the source; if the SError persists it is generated earlier
	 * (off/sleep) and only delivered at the next SMC.
	 *
	 * n_save pinned to 0 (vts_save skipped): the resume fault traced to the
	 * vts_save RESTORE in zrn_pd_power_on reading VTS CMU 0x13800000+0x804
	 * (external abort / async SError), which is not bus-ready right after a
	 * sleep even though VTS PMU STATUS reads on -- the sleep tables also skip
	 * the VTS CMU save when VTS is off, so nothing re-clocks it. VTS is kept
	 * off permanently (powered on only momentarily so pinctrl can restore its
	 * banks), so there is no CMU state to restore -- same as CHUB. */
	{ "VTS",    0x4078, 0,          T(vts_on),    vts_save, 0,    T(vts_off),    I(vts_init) },
	/* CHUB: bit 14 (0x4000). CONFIG 0x4080 / STATUS 0x4084. The DOMINANT MIF-down
	 * gate: the ACPM DOWN sequence reads CHUB_STATUS in five places, VTS_STATUS in
	 * two, AUD_STATUS in none (confirmed in the live ACPM image). CHUB left on
	 * sends the sequence down the oscillator-gating branch no matter what VTS
	 * does, which is why every VTS/AUD-only trial still dark-hung. CHUB has no
	 * generic pd DT node and no need_smc: the vendor contexthub pmucal powers it
	 * with CONFIGURATION=0 + STATUS-wait and no TZPC SMC, so smc=0 here and
	 * zrn_pd_tz() is skipped for it. Order: power CHUB off AFTER VTS (VTS quiesce
	 * reaches the CHUB-side async bridge at 0x13A00000, alive only while CHUB is
	 * on) and back on BEFORE VTS, so CHUB is last here and syscore resume walks
	 * the array in reverse. The on-table is the vendor chub_on truncated to its
	 * first two ops (CONFIGURATION=0xf, wait STATUS=0xf); the remaining
	 * PMUCAL_WRITE_WAIT rows reset and un-clamp the Cortex-M4 for firmware load,
	 * a handshake that never completes with no CHUB firmware (CPU_RESET STATUS
	 * never flips, -ETIMEDOUT) and is not wanted for a bare power cycle, so n_on
	 * is pinned to 2. chub_save RESTORE then writes the CHUB CMU/QCH clocks back,
	 * which the earlier failed full-chub_on resume skipped and left the VTS<->CHUB
	 * bridge half-quiesced.
	 *
	 * n_save is pinned to 0 (chub_save is skipped). chub_save only READS the
	 * shared CHUB CMU/QCH at 0x13A00000 to save clock values for a later
	 * restore; it performs no quiescing (CHUB's Q-channel is parked by the
	 * hardware when CONFIGURATION=0, with no master once VTS is already off).
	 * Those shared-bridge reads are exactly what stalls the AP once VTS has
	 * perturbed 0x13A00000, and ztOS keeps CHUB off permanently (no sensor
	 * consumer), so there is nothing to restore. CHUB-off is therefore just
	 * low_power_init + chub_off, touching nothing on the shared bridge. */
	{ "CHUB",   0x4080, 0,          chub_on, 2,   chub_save, 0,   T(chub_off),   I(chub_init) },
};
#define ZRN_PD_N	ARRAY_SIZE(domains)

static DEFINE_MUTEX(zrn_pd_lock);
static bool zrn_pd_ready;

/* EL3 TrustZone protection restore through the secure monitor. Default on: a
 * non-secure access to a domain restored without it is an SError on this SoC. */
static bool zrn_pd_smc = true;
module_param_named(smc, zrn_pd_smc, bool, 0444);
MODULE_PARM_DESC(smc, "restore domain TrustZone protection through EL3 (default on)");

/* The control knob. One bit a domain (AUD bit 0). Default 0: every domain is
 * vetoed, so load powers nothing off. Set a bit to power that domain off now;
 * clear it to power the domain back on and restore it. */
static unsigned int zrn_pd_enable_off;
static unsigned int zrn_pd_faulted;	/* domains that rolled back; stay vetoed */
module_param_named(faulted, zrn_pd_faulted, uint, 0444);
MODULE_PARM_DESC(faulted, "bitmask of domains a failed power-off rolled back and self-vetoed");
/* pd_off_retries: re-run a domain's off sequence this many times (200us apart)
 * before giving up and rolling back, to ride out an intermittent power-off
 * hold (VTS STATUS stuck 0xf behind the VTS<->CHUB bridge). */
static unsigned int pd_off_retries = 8;
module_param(pd_off_retries, uint, 0644);
static unsigned int off_retries[ZRN_PD_N];
module_param_array(off_retries, uint, NULL, 0444);

static unsigned int power_ons[ZRN_PD_N], power_offs[ZRN_PD_N], last_status[ZRN_PD_N];
static unsigned int n_ons = ZRN_PD_N, n_offs = ZRN_PD_N, n_status = ZRN_PD_N;
module_param_array(power_ons, uint, &n_ons, 0444);
module_param_array(power_offs, uint, &n_offs, 0444);
module_param_array(last_status, uint, &n_status, 0444);

/* ---- CHUB hub-up resume reclock (sensor hub must come back across MIF-down) ----
 * The static CHUB entry truncates chub_on to n_on=2 (power only) on the old "no
 * CHUB consumer" assumption, which SKIPS the RESET_CMU clock-tree bring-up in ops
 * 3-11 and leaves the CHUB CMU in reset -> any CMU access on resume is an
 * unmaskable synchronous external abort. To bring the hub back we must run the
 * full chub_on, but the two M4 CPU_RESET WRITE_WAIT ops poll an M4 status that
 * never flips with no CHUB firmware (-ETIMEDOUT) and abort the sequence before
 * RESET_CMU de-asserts. chub_nowait_mask converts those ops to plain WRITE (no
 * poll) at load; default bits 2 and 8 are exactly the two CPU_RESET ops. All
 * polled registers are PMU-side (0x14060000), so no CMU access happens here. */
static unsigned int chub_non = 2;	/* ops of chub_on to run (2=power only; 12=full) */
static unsigned int chub_nsave;		/* chub_save RESTORE ops (0=skip; >0 re-clock) */
static unsigned int chub_nowait_mask = (1u << 2) | (1u << 8);
/* Force the two M4 CPU_RESET ops (idx 2, 8) to WRITE value=1 -> keep the CHUB
 * Cortex-M4 held in reset through the whole bring-up. ztOS drives the BCM4773
 * hub by bit-banging SPI from the AP over the CHUB-domain gph0/USI; it does NOT
 * run the M4 firmware. Releasing the M4 (op8 value=0) lets its retained-SRAM
 * firmware restart and fight the AP for the same SPI bus -> resume hard-lockup.
 * Holding it in reset still lets op10 de-assert RESET_CMU (CMU clock is M4-
 * independent), so the CMU/GPIO/USI come up clocked and AP-owned. */
static unsigned int chub_write1_mask = (1u << 2) | (1u << 8);
module_param(chub_non, uint, 0444);
module_param(chub_nsave, uint, 0444);
module_param(chub_nowait_mask, uint, 0444);
module_param(chub_write1_mask, uint, 0444);
MODULE_PARM_DESC(chub_non, "chub_on ops to run on power-on (2=power only, 12=full reclock)");
MODULE_PARM_DESC(chub_nsave, "chub_save RESTORE ops on resume (0=skip, >0 re-clock USI/QCH)");
MODULE_PARM_DESC(chub_nowait_mask, "chub_on op indices to force WRITE (no poll); default the 2 M4 CPU_RESET ops");
MODULE_PARM_DESC(chub_write1_mask, "chub_on op indices to force value=1 (hold M4 in reset); default idx 2,8");
/* chub_m4_wfi: park the CHUB Cortex-M4 in WFI before the CHUB PMU power-off.
 * ztOS runs no CHUB firmware -- the S9 hub is an external Broadcom part; the
 * CHUB domain is up only so zrn_bbd can bit-bang the GPS SPI over the CHUB-block
 * USI -- so the M4 is held in reset and never asserts STANDBYWFI. Its Q-channel
 * then stays busy and the CHUB PMU power-off (CONFIGURATION=0, wait STATUS==0)
 * times out at 0xf. Merely asserting CPU_RESET does NOT help: a reset core still
 * never asserts STANDBYWFI. The fix is to load a tiny WFI stub into CHUB SRAM
 * and RELEASE the M4 onto it so it parks in WFI -> STANDBYWFI asserts -> the
 * chub_off STATUS-wait settles. Default on. */
static bool chub_m4_wfi = true;
module_param(chub_m4_wfi, bool, 0644);
MODULE_PARM_DESC(chub_m4_wfi, "park the CHUB M4 in WFI (load stub + release CPU_RESET) before CHUB power-off so STATUS can reach 0 (default on)");
/* Set by the last park: 1 if the M4 reached STANDBYWFI, 0 otherwise. Advisory
 * only -- the park never fails the power-off; chub_off's STATUS->0 is authoritative. */
static bool chub_wfi;
module_param(chub_wfi, bool, 0444);
MODULE_PARM_DESC(chub_wfi, "1 if the CHUB M4 reached STANDBYWFI at the last power-off park (read-only)");

/* ---- CHUB Cortex-M4 cold-boot WFI park (ported from Exynos9610 contexthub) ----
 *
 * CHUB M4 CODE SRAM, AP-physical. The core's reset-vector space (CM4 internal
 * 0x00000000) aliases to this bank: a Cortex-M4 resets with VTOR=0, so on reset
 * it fetches the initial MSP from [SRAM+0] and the reset PC from [SRAM+4].
 * Confirmed by DUMPGPR: a stray M4 PC of 0x1bb8e read back as AP 0x13C00000 +
 * 0x1bb8e, i.e. CM4 internal 0 == AP 0x13C00000. The old value here (0x13E40000)
 * was wrong -- the stub was planted in a bank the M4 never fetches from, so the
 * core (which was never actually reset) kept running leftover S-Boot code.
 * ioremap covers 0x13C00000..+0x10000, far below the 0x13C1B000 bank that
 * SIGBUSes, so both the vector write and the MSP top (0x10000) land in mapped,
 * M4-visible SRAM. [INFER: internal-0 alias to 0x13C00000 is from DUMPGPR
 * evidence; verify live by reading chub_wfi_pc back as a low address.] */
#define ZRN_CHUB_SRAM_BASE	0x13C00000
/* CHUB CMU, AP-physical. Not required by the cold reset (the AP->SRAM AXI path
 * and its QCHs stay clocked independently of the CHUB CMU); kept mapped only for
 * reference. */
#define ZRN_CHUB_CMU_BASE	0x13A00000
#define ZRN_CHUB_CMU_ASYNCAHBM_QCH	0x3010
/* CHUB M4 DUMPGPR, AP-physical (confirmed 0x13BF0000; chub.h:276 names CTRL=0x0,
 * PCR=0x4). CTRL=1 enables the snapshot; PCR is the captured M4 PC. Read it back
 * after the park to prove the core is in OUR stub (a low address ~0x08), not the
 * stale 0x1bb8e. */
#define ZRN_CHUB_DUMPGPR_BASE	0x13BF0000
#define CHUB_DUMPGPR_CTRL	0x00
#define CHUB_DUMPGPR_PCR	0x04

/* PMU CHUB CPU-reset block (offsets from ZRN_PD_PMU_BASE). This is the ported
 * 9610 "pmu_chub_reset" window: on 9810 it sits at 0x4b98 with the vendor layout
 * CONFIGURATION(+0)/STATUS(+4)/OPTION(+8) (chub.h:256-260; 9810
 * CHUB_RESET_RELEASE_VALUE=0x10000000). CONFIGURATION bit0 is the Cortex-M4
 * reset: assert(=1) holds the core reset with VTOR cleared, release(=0) makes it
 * cold-fetch the vector from SRAM 0x0. STATUS bit0 mirrors run state, STATUS
 * bit28=STANDBYWFI. OPTION bit28=ENABLE_CPU (the 9610 RELEASE_VALUE) arms the
 * released core to run. */
#define CHUB_CPU_CONFIG		0x4b98	/* bit0: M4 reset (1=held, 0=release/cold-boot) */
#define CHUB_CPU_STATUS		0x4b9c	/* bit0=running, bit28=STANDBYWFI */
#define CHUB_CPU_OPTION		0x4ba0	/* bit28=ENABLE_CPU (9610 CHUB_RESET_RELEASE_VALUE) */
#define CHUB_CPU_ENABLE		BIT(28)	/* 0x10000000 */
/* CHUB block reset/clamp controls (all bit0), from the vendor chub_on table
 * (pd-tables.h:864-873) and sensors/driver/zrn_bbd_board.c:286-290. */
#define CHUB_RESET_LOGIC	0x4b38
#define CHUB_LOGIC_CLAMP	0x4b54
#define CHUB_RESET_CMU		0x4c10
#define CHUB_SWEEPER_CLEAN	0x4c30
#define CHUB_STANDBYWFI		BIT(28)
/* PMU CPU_STANDBY sequencer option for CHUB (0x14060000 + 0x38xx). The PMU only
 * tears the CPU power-island's Q-channel down on STANDBYWFI when USE_STANDBYWFI
 * (CPU_OPTION bit16) is set -- VTS does VTS_CPU_OPTION(0x3818)|=0x10000 before
 * its own power-off. The 0x38xx table runs ABOX/AUD(0x3804/0x3808),
 * VTS(0x3814/0x3818), then CHUB, stride 0x10 -> CHUB CPU_OPTION ~= 0x3828.
 * [INFER: 0x3828 offset is derived, not confirmed; it is overridable, and the
 * whole 0x3804..0x3843 block is snapshot into chub_standby[] so the live slot
 * can be identified.] */
#define CHUB_CPU_USE_STANDBYWFI	BIT(16)	/* 0x00010000 */
static unsigned int chub_cpu_option_off = 0x3828;
module_param(chub_cpu_option_off, uint, 0644);
MODULE_PARM_DESC(chub_cpu_option_off, "PMU offset of CHUB CPU_OPTION for USE_STANDBYWFI (inferred 0x3828; verify live)");
static bool chub_use_standbywfi = true;
module_param(chub_use_standbywfi, bool, 0644);
MODULE_PARM_DESC(chub_use_standbywfi, "set CHUB CPU_OPTION USE_STANDBYWFI (bit16) before releasing the M4 (default on)");
/* DUMPGPR M4 PC captured at the last park (read-only debug). A low value (~0x08)
 * proves the core cold-booted our stub; 0x1bb8e means it never reset. */
static unsigned int chub_wfi_pc;
module_param(chub_wfi_pc, uint, 0444);
MODULE_PARM_DESC(chub_wfi_pc, "CHUB M4 DUMPGPR program counter read back after the last WFI park");
/* The 0x3804..0x3843 CPU_STANDBY block, snapshot at the last park, so the CHUB
 * slot (whose STANDBY status reflects the M4 entering WFI) can be identified. */
static unsigned int chub_standby[16];
static unsigned int chub_standby_n = 16;
module_param_array(chub_standby, uint, &chub_standby_n, 0444);
/* Assert LOGIC_CLAMP before the CPU reset. The vendor order asserts CPU reset
 * first; that once crashed the phone on a still-running M4 mid-bus-transaction.
 * Clamp-first isolates the M4's bus outputs before resetting it. Default off
 * (vendor-exact); the first thing to try if a live assert hangs. */
static bool chub_clamp_first;
module_param(chub_clamp_first, bool, 0644);
MODULE_PARM_DESC(chub_clamp_first, "assert LOGIC_CLAMP before CPU reset (safer for a running M4; default off = vendor-exact)");

/* The CM4 WFI stub, as u32 LE words at SRAM 0x0 (AP ZRN_CHUB_SRAM_BASE):
 *   [0x00] 0x00010000  initial MSP (top of the 64K SRAM window; stub uses no stack)
 *   [0x04] 0x00000009  reset vector -> CM4 addr 0x08 | thumb bit
 *   [0x08] 0xb672      cpsid i   (mask IRQ/fault so nothing can wake the core out)
 *   [0x0A] 0xbf30      wfi
 *   [0x0C] 0xe7fd      b .       (branches to 0x0A: re-enter wfi on any wake) */
static const u32 chub_wfi_stub[] = {
	0x00010000, 0x00000009, 0xbf30b672, 0x0000e7fd,
};

/* ---- phys->virt region cache + sequence IO (mirrors platform.c) ---- */
struct region { u32 physical; void __iomem *mapped; };
static struct region regions[64];
static unsigned int region_count;

static void __iomem *mapped_base(u32 base)
{
	unsigned int i;

	for (i = 0; i < region_count; i++)
		if (regions[i].physical == base)
			return regions[i].mapped;
	return NULL;	/* every table base is mapped at init */
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
	regions[region_count++] = (struct region){ base, mapped };
	return 0;
}

static void unmap_all(void)
{
	while (region_count)
		iounmap(regions[--region_count].mapped);
}

static u32 register_read(void *ctx, u32 base, u32 offset)
{
	return readl_relaxed(mapped_base(base) + offset);
}

static void register_write(void *ctx, u32 base, u32 offset, u32 value)
{
	writel_relaxed(value, mapped_base(base) + offset);
}

static void seq_delay(void *ctx, unsigned int usec) { udelay(usec); }
static const struct zrn_seq_io io = {
	.read = register_read, .write = register_write, .delay_us = seq_delay,
};

static u32 pd_status_read(const struct zrn_pd_domain *d)
{
	return readl_relaxed(mapped_base(ZRN_PD_PMU_BASE) + ZRN_PD_STATUS(d->config))
		& ZRN_PD_PWR_CFG;
}

static bool pd_is_on(const struct zrn_pd_domain *d)
{
	return pd_status_read(d) == ZRN_PD_PWR_CFG;
}

static int zrn_pd_tz(u32 addr, unsigned long op)
{
	struct arm_smccc_res res = {};

	/* addr 0 means the domain has no TZPC restore (no need_smc in the vendor DT,
	 * e.g. CHUB): the vendor powers it without a secure save/restore step. */
	if (!zrn_pd_smc || !addr)
		return 0;
	arm_smccc_smc(SMC_CMD_PREPARE_PD_ONOFF, op, addr, 0, 0, 0, 0, 0, &res);
	return res.a0 ? -EIO : 0;
}

static void zrn_pd_low_power_init(struct zrn_pd_domain *d)
{
	void __iomem *m;
	unsigned int i;
	u32 v;

	if (d->lpm_done)
		return;
	for (i = 0; i < d->n_init; i++) {
		const struct zrn_pd_init *s = &d->init[i];

		m = mapped_base(s->base) + s->offset;
		v = (readl_relaxed(m) & ~s->mask) | s->value;
		writel_relaxed(v, m);
	}
	dsb(sy);
	d->lpm_done = true;
	pr_info("zrn-pd: %s LPM init applied (%u regs)\n", d->name, d->n_init);
}

/* vendor pmucal_local_enable: handle(on) -> TZ restore -> restore(save) */
static int zrn_pd_power_on(struct zrn_pd_domain *d)
{
	unsigned int failed = 0;
	int ret;

	ret = zrn_sequence_run(d->on, d->n_on, ZRN_HANDLE, &io, &failed);
	if (ret) {
		pr_err("zrn-pd: %s power-on seq failed at %u (%s): %d, STATUS=%01x\n",
		       d->name, failed,
		       failed < d->n_on ? d->on[failed].name : "?", ret,
		       pd_status_read(d));
		return ret;
	}
	ret = zrn_pd_tz(d->smc, EXYNOS_WAKEUP_PD_DOWN);
	if (ret) {
		pr_err("zrn-pd: %s TZ restore SMC failed: %d\n", d->name, ret);
		return ret;
	}
	ret = zrn_sequence_run(d->save, d->n_save, ZRN_RESTORE, &io, &failed);
	if (ret)
		pr_err("zrn-pd: %s restore seq failed at %u: %d\n",
		       d->name, failed, ret);
	last_status[d - domains] = pd_status_read(d);
	return ret;
}

/* vendor pmucal_local_disable: save(save) -> TZ save -> handle(off).
 * A timeout leaves the domain half-off (the SError/reset risk): roll it back on
 * and restore, self-veto, and only panic if the rollback itself fails. */
/* Park the CHUB Cortex-M4 in WFI before powering CHUB off (see chub_m4_wfi).
 * Runs in syscore_suspend: atomic, IRQs off. Every region it touches (PMU, CHUB
 * SRAM, DUMPGPR) is pre-mapped at init via the mapped_base cache; only
 * writel/readl_relaxed + udelay here, no ioremap.
 *
 * WHY A COLD RESET AND NOT THE OPTION TOGGLE: the earlier attempts flipped
 * CHUB_CPU_OPTION bit28 (ENABLE_CPU) -- the 9610 POWER_ON "release" -- on an M4
 * that S-Boot had already released and left running. ENABLE_CPU does not reset
 * core state, so the core kept its S-Boot VTOR/PC and never looked at our vector
 * (DUMPGPR PC stayed at 0x1bb8e). A Cortex-M4 only re-fetches [VTOR+0]=MSP and
 * [VTOR+4]=PC on a core RESET, and reset also forces VTOR back to 0 == AP
 * 0x13C00000 where our stub lives. So we drive the vendor pmucal cold-reset
 * (the chub_on ops 2-11 reset assert->release), exactly as the vendor contexthub
 * bring-up does. The stub is written while the domain is still powered and
 * clocked (before the reset) -- the vendor loads firmware before running the
 * reset sequence. On the CPU_CONFIG release the core cold-fetches the stub and
 * parks in WFI; with USE_STANDBYWFI set, the PMU then drops the CPU Q-channel so
 * the chub_off domain power-off STATUS can fall to 0.
 *
 * SAFETY: asserting CPU_CONFIG(0x4b98) bit0 in isolation once crashed the phone
 * (a running M4 mid-bus-transaction). Here it is asserted inside the full vendor
 * assert group (RESET_LOGIC/RESET_CMU/LOGIC_CLAMP), which the vendor pmucal is
 * proven to run on this SoC. If a live assert still hangs, set chub_clamp_first. */
static void chub_m4_park_wfi(void)
{
	void __iomem *pmu = mapped_base(ZRN_PD_PMU_BASE);
	void __iomem *sram = mapped_base(ZRN_CHUB_SRAM_BASE);
	void __iomem *gpr = mapped_base(ZRN_CHUB_DUMPGPR_BASE);
	void __iomem *cmu = mapped_base(ZRN_CHUB_CMU_BASE);
	unsigned int i;
	u32 v;

	chub_wfi = false;
	chub_wfi_pc = 0;
	if (!pmu || !sram)
		return;

	/* Snapshot the CPU_STANDBY block so the live CHUB slot can be identified. */
	for (i = 0; i < ARRAY_SIZE(chub_standby); i++)
		chub_standby[i] = readl_relaxed(pmu + 0x3804 + i * 4);

	/* 0. Ensure the CHUB domain is powered (vendor chub_on ops 0-1:
	 *    CHUB_CONFIGURATION 0x4080=0xf, wait CHUB_STATUS 0x4084=0xf). It already
	 *    is on this path, but the cold reset needs it up and clocked. */
	v = readl_relaxed(pmu + 0x4080);
	writel_relaxed((v & ~0xfu) | 0xf, pmu + 0x4080);
	for (i = 0; i < 1000; i++) {
		if ((readl_relaxed(pmu + 0x4084) & 0xf) == 0xf)
			break;
		udelay(10);
	}

	/* 1. CLOCK THE CMU WITH THE M4 HELD. After the hub goes to standby and
	 *    low_power_init runs, the CHUB CMU is held in reset (RESET_CMU asserted),
	 *    so any SRAM/DUMPGPR access aborts (synchronous external abort) -- this is
	 *    exactly what the first stub-load faulted on (zrn_pd_power_off+0x3fc, ESR
	 *    0x96000010). Mirror the proven zrn_bbd_chub_unpark reset-release: HOLD the
	 *    M4 (CPU_CONFIG bit0 = 1), pulse the LOGIC/CMU reset + clamp, then de-assert
	 *    them so the CMU is clocked while the M4 stays held, and point the
	 *    peripheral muxes at the always-on OSCCLK and open the CHUB QCH gates. Only
	 *    after this is the SRAM bus alive enough to plant the stub. */
	writel_relaxed(readl_relaxed(pmu + CHUB_CPU_CONFIG) | 1u, pmu + CHUB_CPU_CONFIG);
	writel_relaxed(1u, pmu + CHUB_RESET_LOGIC);
	writel_relaxed(1u, pmu + CHUB_RESET_CMU);
	writel_relaxed(1u, pmu + CHUB_LOGIC_CLAMP);
	writel_relaxed(0u, pmu + CHUB_SWEEPER_CLEAN);
	dsb(sy);
	udelay(10);
	writel_relaxed(0u, pmu + CHUB_LOGIC_CLAMP);
	writel_relaxed(0u, pmu + CHUB_RESET_LOGIC);
	writel_relaxed(0u, pmu + CHUB_RESET_CMU);	/* CMU clocked; M4 still held */
	writel_relaxed(1u, pmu + CHUB_SWEEPER_CLEAN);
	dsb(sy);
	udelay(50);
	if (cmu) {
		writel_relaxed(0u, cmu + 0x100c);	/* MUX_USI00 -> OSCCLK */
		writel_relaxed(0u, cmu + 0x1808);	/* DIV_USI00 -> /1 */
		writel_relaxed(0u, cmu + 0x1008);	/* MUX_I2C  -> OSCCLK */
		dsb(sy);
		udelay(10);
		writel_relaxed(0x003f0002u, cmu + 0x3028);	/* QCH GPIO_CHUB */
		writel_relaxed(0x003f0002u, cmu + 0x3054);	/* QCH SYSREG_CHUB */
		writel_relaxed(0x003f0002u, cmu + 0x305c);	/* QCH USI_CHUB00 */
		dsb(sy);
		udelay(50);
	}

	/* 2. Enable DUMPGPR so the M4 PC can be read back afterwards (bus now clocked). */
	if (gpr)
		writel_relaxed(1u, gpr + CHUB_DUMPGPR_CTRL);

	/* 3. Plant the WFI stub at SRAM 0x0 (CM4-internal 0x00000000) while the M4 is
	 *    held and the bus is clocked. The core cold-fetches [0]=MSP, [4]=PC when
	 *    its reset is released in step 5. */
	for (i = 0; i < ARRAY_SIZE(chub_wfi_stub); i++)
		writel_relaxed(chub_wfi_stub[i], sram + i * 4);
	dsb(sy);

	/* 4. Arm ENABLE_CPU (the 9610 CHUB_RESET_RELEASE_VALUE) so the released core
	 *    runs, and USE_STANDBYWFI so the PMU drops the CPU Q-channel on WFI (the
	 *    VTS_CPU_OPTION|=0x10000 analogue). */
	writel_relaxed(readl_relaxed(pmu + CHUB_CPU_OPTION) | CHUB_CPU_ENABLE,
		       pmu + CHUB_CPU_OPTION);
	if (chub_use_standbywfi && chub_cpu_option_off < 0x10000)
		writel_relaxed(readl_relaxed(pmu + chub_cpu_option_off) | CHUB_CPU_USE_STANDBYWFI,
			       pmu + chub_cpu_option_off);
	dsb(sy);

	/* 5. Release ONLY the M4 core reset (CPU_CONFIG bit0 -> 0). The CMU/LOGIC are
	 *    already out of reset and clocked from step 1, so -- unlike the old code,
	 *    which released CPU_CONFIG before RESET_CMU -- the core cold-fetches the
	 *    stub from SRAM 0 (VTOR=0) against a live clock and parks in WFI. */
	writel_relaxed(readl_relaxed(pmu + CHUB_CPU_CONFIG) & ~1u, pmu + CHUB_CPU_CONFIG);
	dsb(sy);
	udelay(10);

	/* 6. Poll STANDBYWFI (CPU STATUS bit28) up to ~10 ms. Advisory only: the
	 *    chub_off STATUS->0 is the authoritative power-off check. */
	for (i = 0; i < 1000; i++) {
		if (readl_relaxed(pmu + CHUB_CPU_STATUS) & CHUB_STANDBYWFI) {
			chub_wfi = true;
			break;
		}
		udelay(10);
	}

	/* 7. Capture the M4 PC via DUMPGPR for the live "are we in our stub?" check. */
	if (gpr)
		chub_wfi_pc = readl_relaxed(gpr + CHUB_DUMPGPR_PCR);

	pr_info("zrn-pd: CHUB M4 WFI park: STANDBYWFI=%d (status=%#x) pc=%#x\n",
		chub_wfi, readl_relaxed(pmu + CHUB_CPU_STATUS), chub_wfi_pc);
}

static int zrn_pd_power_off(struct zrn_pd_domain *d)
{
	unsigned int idx = d - domains, failed = 0;
	int ret, rb;

	zrn_pd_low_power_init(d);
	if (chub_m4_wfi && !strcmp(d->name, "CHUB"))
		chub_m4_park_wfi();	/* cold-boot the M4 into a WFI stub so CHUB STATUS can fall to 0 */

	ret = zrn_sequence_run(d->save, d->n_save, ZRN_SAVE, &io, &failed);
	if (ret) {
		pr_err("zrn-pd: %s save seq failed at %u: %d\n", d->name, failed, ret);
		return ret;
	}
	ret = zrn_pd_tz(d->smc, EXYNOS_GET_IN_PD_DOWN);
	if (ret) {
		pr_err("zrn-pd: %s TZ save SMC failed: %d\n", d->name, ret);
		/* protection untouched, domain still on: restore captured state */
		zrn_sequence_run(d->save, d->n_save, ZRN_RESTORE, &io, &failed);
		return ret;
	}
	ret = zrn_sequence_run(d->off, d->n_off, ZRN_HANDLE, &io, &failed);
	/* An intermittent hold (e.g. the VTS<->CHUB bridge not yet quiesced) leaves
	 * STATUS=0xf and -ETIMEDOUT on the first try; re-running CONFIGURATION=0 +
	 * STATUS-wait after a short settle catches it instead of a permanent veto. */
	if (ret) {
		unsigned int attempt;
		for (attempt = 0; attempt < pd_off_retries && ret; attempt++) {
			udelay(200);
			off_retries[idx]++;
			ret = zrn_sequence_run(d->off, d->n_off, ZRN_HANDLE, &io, &failed);
		}
	}
	if (!ret) {
		d->off_by_us = true;
		power_offs[idx]++;
		last_status[idx] = pd_status_read(d);
		pr_info("zrn-pd: %s powered OFF (STATUS=%01x)\n", d->name, last_status[idx]);
		return 0;
	}

	pr_err("zrn-pd: %s power-off seq failed at %u (%s): %d, STATUS=%01x; rolling back\n",
	       d->name, failed, failed < d->n_off ? d->off[failed].name : "?",
	       ret, pd_status_read(d));
	rb = zrn_sequence_run(d->on, d->n_on, ZRN_HANDLE, &io, &failed);
	if (!rb)
		rb = zrn_pd_tz(d->smc, EXYNOS_WAKEUP_PD_DOWN);
	if (!rb)
		rb = zrn_sequence_run(d->save, d->n_save, ZRN_RESTORE, &io, &failed);
	zrn_pd_faulted |= BIT(idx);
	zrn_pd_enable_off &= ~BIT(idx);
	d->off_by_us = false;
	last_status[idx] = pd_status_read(d);
	if (rb)
		panic("zrn-pd: %s power-off rollback failed (%d), STATUS=%01x",
		      d->name, rb, last_status[idx]);
	pr_err("zrn-pd: %s rolled back on and self-vetoed (STATUS=%01x)\n",
	       d->name, last_status[idx]);
	return ret;
}

/* Bring every domain to the state enable_off asks for, one at a time. */
static void zrn_pd_reconcile(void)
{
	unsigned int i;

	for (i = 0; i < ZRN_PD_N; i++) {
		struct zrn_pd_domain *d = &domains[i];
		bool want_off = zrn_pd_enable_off & BIT(i);

		if (want_off && !(zrn_pd_faulted & BIT(i)) &&
		    !d->off_by_us && pd_is_on(d))
			zrn_pd_power_off(d);
		else if (!want_off && d->off_by_us)
			zrn_pd_power_on(d);
		last_status[i] = pd_status_read(d);
	}
}

static int enable_off_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	mutex_lock(&zrn_pd_lock);
	ret = param_set_uint(val, kp);
	if (!ret && zrn_pd_ready)
		zrn_pd_reconcile();
	mutex_unlock(&zrn_pd_lock);
	return ret;
}
static const struct kernel_param_ops enable_off_ops = {
	.set = enable_off_set, .get = param_get_uint,
};
module_param_cb(enable_off, &enable_off_ops, &zrn_pd_enable_off, 0644);
MODULE_PARM_DESC(enable_off,
	"bitmask, one bit a domain (AUD=1 DPU=2 DSPM=4 DSPS=8 G2D=0x10 MFC=0x20 "
	"IVA=0x40 ISPPRE=0x80 ISPHQ=0x100 ISPLP=0x200 DCF=0x400 DCPOST=0x800 "
	"DCRD=0x1000 VTS=0x2000 CHUB=0x4000): set to power a domain off, clear to power it on (default 0)");

/* late_off: domains to power off only for the sleep itself. Mainline drivers
 * that live inside a domain still save and restore their registers during
 * device suspend (17c60000.pinctrl sits in AUD: with AUD already off its late
 * suspend never returns). Syscore suspend runs after every device callback and
 * syscore resume before any, so the domain is off only while nobody touches
 * it, which is the order the vendor kernel has. Same bits as enable_off. */
static unsigned int zrn_pd_late_off;
module_param_named(late_off, zrn_pd_late_off, uint, 0644);
MODULE_PARM_DESC(late_off,
	"bitmask as enable_off: power these domains off in syscore suspend and back on in syscore resume (default 0)");
static unsigned int zrn_pd_late_done, late_offs, late_ons, late_errors;
module_param(late_offs, uint, 0444);
module_param(late_ons, uint, 0444);
module_param(late_errors, uint, 0444);
/* DISR_EL1 syndrome of the deferred SError consumed on the last resume, 0 if
 * none. Non-zero means a pending async SError was swallowed (see resume). */
static unsigned long resume_disr;
module_param(resume_disr, ulong, 0444);

static int zrn_pd_syscore_suspend(void *data)
{
	unsigned int i;

	zrn_pd_late_done = 0;
	if (!zrn_pd_ready)
		return 0;
	for (i = 0; i < ZRN_PD_N; i++) {
		struct zrn_pd_domain *d = &domains[i];

		if (!(zrn_pd_late_off & BIT(i)) || (zrn_pd_faulted & BIT(i)) ||
		    d->off_by_us || !pd_is_on(d))
			continue;
		if (zrn_pd_power_off(d)) {
			late_errors++;
			continue;
		}
		zrn_pd_late_done |= BIT(i);
		late_offs++;
	}
	return 0;	/* a domain left on must not refuse the sleep */
}

static void zrn_pd_resume_power_on_all(void)
{
	unsigned int i;

	/* Reverse of suspend order: a domain listed later powers on first. CHUB
	 * (last) must come back before VTS, whose resume re-opens the VTS<->CHUB
	 * bridge that is only reachable with CHUB already on. */
	for (i = ZRN_PD_N; i-- > 0; ) {
		struct zrn_pd_domain *d = &domains[i];

		if (!(zrn_pd_late_done & BIT(i)))
			continue;
		if (zrn_pd_power_on(d))
			late_errors++;
		else
			late_ons++;
		d->off_by_us = false;
	}
}

/* Powering VTS/CHUB back on across a MIF-down sleep raises a pending async
 * SError (0xbe000011): a benign power-cycle bus error that arm64 treats as a
 * fatal RAS SError and panics on, although the sleep and wake themselves
 * succeed (handoff mif_sleep=1, cpu_pm_resume reached). Mask PSTATE.A across the
 * power-on so a consumable RAS error is deferred into DISR_EL1 rather than
 * taken, record its syndrome (resume_disr) and clear it, then unmask. Vendor PM
 * code masks SError across power transitions for the same reason. If a later
 * real SError is ever deferred here it shows up in resume_disr for inspection. */
static void zrn_pd_syscore_resume(void *data)
{
	unsigned long disr;

	asm volatile("msr daifset, #4\n\tisb" ::: "memory");	/* mask SError (A) */
	zrn_pd_resume_power_on_all();
	asm volatile("dsb sy\n\tisb" ::: "memory");		/* drain; make any async error pending */
	asm volatile("mrs %0, S3_0_C12_C1_1" : "=r" (disr));	/* read DISR_EL1 */
	resume_disr = disr;
	asm volatile("msr S3_0_C12_C1_1, xzr" ::: "memory");	/* clear deferred SError */
	asm volatile("msr daifclr, #4\n\tisb" ::: "memory");	/* unmask SError */
	zrn_pd_late_done = 0;
}

static const struct syscore_ops zrn_pd_syscore_ops = {
	.suspend = zrn_pd_syscore_suspend,
	.resume = zrn_pd_syscore_resume,
};
static struct syscore zrn_pd_syscore = { .ops = &zrn_pd_syscore_ops };

static int zrn_pd_map_all(void)
{
	unsigned int i, j;
	int ret;

	ret = map_base(ZRN_PD_PMU_BASE);
	if (ret)
		return ret;
	/* CHUB SRAM is not a table base; pre-map it here (sleepable) so the atomic
	 * syscore-suspend park can plant the WFI stub via the mapped_base cache.
	 * The CHUB CMU (0x13A00000) is already mapped via vts_off, but map it too
	 * (map_base is idempotent) so the park's QCH write never depends on that. */
	ret = map_base(ZRN_CHUB_SRAM_BASE);
	if (ret)
		return ret;
	ret = map_base(ZRN_CHUB_CMU_BASE);
	if (ret)
		return ret;
	/* DUMPGPR: enabled during the park, read back for the M4-PC debug param. */
	ret = map_base(ZRN_CHUB_DUMPGPR_BASE);
	if (ret)
		return ret;
	for (i = 0; i < ZRN_PD_N; i++) {
		struct zrn_pd_domain *d = &domains[i];
		struct zrn_seq *tables[3] = { d->on, d->save, d->off };
		unsigned int counts[3] = { d->n_on, d->n_save, d->n_off };
		unsigned int t;

		for (t = 0; t < 3; t++)
			for (j = 0; j < counts[t]; j++) {
				struct zrn_seq *s = &tables[t][j];

				if (s->offset >= 0x10000 || s->cond_offset >= 0x10000)
					return -EINVAL;
				ret = map_base(s->base);
				if (!ret)
					ret = map_base(s->cond_base);
				if (ret)
					return ret;
			}
		for (j = 0; j < d->n_init; j++) {
			ret = map_base(d->init[j].base);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int __init zrn_pd_init(void)
{
	unsigned int i;
	int ret;

	if (!of_machine_is_compatible("samsung,exynos9810"))
		return -ENODEV;

	/* Apply the CHUB hub-up reclock overrides before mapping, so map_all walks
	 * whatever extra chub_on/chub_save ops we enable. Clamp to the real tables. */
	for (i = 0; i < ZRN_PD_N; i++) {
		struct zrn_pd_domain *d = &domains[i];
		unsigned int b;

		if (strcmp(d->name, "CHUB"))
			continue;
		if (chub_non > ARRAY_SIZE(chub_on))
			chub_non = ARRAY_SIZE(chub_on);
		if (chub_nsave > ARRAY_SIZE(chub_save))
			chub_nsave = ARRAY_SIZE(chub_save);
		d->n_on = chub_non;
		d->n_save = chub_nsave;
		for (b = 0; b < ARRAY_SIZE(chub_on); b++) {
			if ((chub_nowait_mask & (1u << b)) &&
			    chub_on[b].op == PMUCAL_WRITE_WAIT)
				chub_on[b].op = PMUCAL_WRITE;
			if (chub_write1_mask & (1u << b))
				chub_on[b].value = chub_on[b].mask;	/* field -> all 1s */
		}
		pr_info("zrn-pd: CHUB reclock: n_on=%u n_save=%u nowait_mask=%#x write1_mask=%#x\n",
			d->n_on, d->n_save, chub_nowait_mask, chub_write1_mask);
		break;
	}

	ret = zrn_pd_map_all();
	if (ret) {
		unmap_all();
		return ret;
	}
	for (i = 0; i < ZRN_PD_N; i++)
		last_status[i] = pd_status_read(&domains[i]);
	mutex_lock(&zrn_pd_lock);
	zrn_pd_ready = true;
	/* Honour any enable_off passed on the insmod line (default 0: no-op). */
	zrn_pd_reconcile();
	mutex_unlock(&zrn_pd_lock);
	register_syscore(&zrn_pd_syscore);
	pr_info("zrn-pd: %u domains, %u regions mapped; enable_off=%#x (all vetoed by default)\n",
		(unsigned int)ZRN_PD_N, region_count, zrn_pd_enable_off);
	for (i = 0; i < ZRN_PD_N; i++)
		pr_info("zrn-pd:   bit %2u %-6s CONFIG=%#06x STATUS=%01x smc=%#x\n",
			i, domains[i].name, domains[i].config,
			last_status[i], domains[i].smc);
	return 0;
}

static void __exit zrn_pd_exit(void)
{
	unsigned int i;

	unregister_syscore(&zrn_pd_syscore);
	/* Never leave a domain we powered off behind: restore each one. */
	mutex_lock(&zrn_pd_lock);
	for (i = 0; i < ZRN_PD_N; i++)
		if (domains[i].off_by_us)
			zrn_pd_power_on(&domains[i]);
	zrn_pd_ready = false;
	mutex_unlock(&zrn_pd_lock);
	unmap_all();
}
module_init(zrn_pd_init);
module_exit(zrn_pd_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Exynos9810 generic multimedia/audio power-domain driver (starlte)");
