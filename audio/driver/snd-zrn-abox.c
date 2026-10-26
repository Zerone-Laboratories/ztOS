// SPDX-License-Identifier: GPL-2.0
/*
 * snd-zrn-abox: sound for the Galaxy S9 (Exynos 9810) on a mainline kernel.
 *
 * One ALSA card with one playback device, 48 kHz, 16 bit, stereo, out of the
 * phone's two speakers. With capture=1 (off by default, untested on the phone
 * yet), also the main microphones as hw:0,2: audio/MIC-REA.md.
 *
 * Every sound on this SoC passes through ABOX, an audio block built round a
 * Cortex-A7 that runs Samsung's "calliope" firmware. This driver starts that
 * firmware (calliope_sram.bin, calliope_dram.bin, calliope_iva.bin from the
 * phone's own vendor partition, in /lib/firmware), gives it one playback
 * stream (RDMA0) whose buffer is the ALSA buffer, routes that stream to the
 * UAIF1 port, and sets up the two MAX98512 amplifiers that listen there.
 *
 * It is a port by hand of the parts of Samsung's GPL driver that this needs
 * (sound/soc/samsung/abox/ and sound/soc/codecs/max98512.c in the
 * exynos9810 vendor kernel), not of its structure: no device tree node, no
 * clock or IOMMU framework. Addresses come from the stock device tree. The
 * audio power domain and PLL are as the bootloader leaves them (on, and at
 * 1179.648 MHz).
 *
 * The amplifiers' I2C controller is not in the mainline device tree either,
 * so the driver registers a bit-banged bus (i2c-gpio) on its two pins.
 *
 * NOT here: Maxim's speaker protection (DSM), which on the stock system runs
 * in the DSP against the current and voltage the amplifiers report. Until it
 * is, the analogue gain stays 3 dB under Samsung's and the boost converter's
 * output a little under theirs, and the speakers are protected by PipeWire's
 * speaker chain instead (rootfs/overlay/etc/pipewire/pipewire.conf.d/
 * 60-ztos-speaker-crossover.conf: high-pass and a lookahead limiter).
 */
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqdomain.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/suspend.h>
#include <linux/sizes.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include <linux/arm-smccc.h>

#define PMU_BASE		0x14060000
#define PMU_AUD_STATUS		0x4004
#define PMU_ABOX_CPU_CFG	0x415c
#define PMU_ABOX_CPU_STATUS	0x4160
#define PMU_ABOX_CPU_OPTION	0x4164
#define  ABOX_CPU_ENABLE	BIT(28)
#define PMU_GPIO_MODE_ABOX	0x1424
#define PMU_ABOX_CPU_STANDBY	0x3804
#define  ABOX_CPU_WFI		BIT(28)
#define PMU_PAD_RET_ABOX	0x4170

#define ABOX_SFR		0x17c50000
#define  ABOX_IP_ID		0x0000
#define  ABOX_VERSION		0x0004
#define  ABOX_SYSPOWER_CTRL	0x0010
#define  ABOX_SYSPOWER_STATUS	0x0014
#define  ABOX_CPU_PC		0x2c3c
#define  ABOX_ROUTE_CTRL0	0x0040
#define  ABOX_SPUS_CTRL0	0x0200
#define  ABOX_SPUS_CTRL1	0x0204
#define  ABOX_SPUS_SIFS_CNT0	0x0280	/* SIFS0's pace: bits 15:0 */
#define  ABOX_UAIF1_CTRL0	0x0510
#define  ABOX_UAIF1_CTRL1	0x0514
#define  ABOX_UAIF1_STATUS	0x051c
#define  ABOX_RDMA0		0x1000
#define CMU_AUD			0x17c00000
#define  CMU_DIV_AUDIF		0x1800
#define  CMU_DIV_BUS		0x1804
#define  CMU_DIV_UAIF1		0x1828
#define  CMU_MUX_UAIF1		0x1008
#define PINCTRL_AUD		0x17c60000
#define  GPB1_CON		0x0020
#define ABOX_SYSREG		0x17c10000
#define  SYSREG_MISC_CON	0x032c
#define ABOX_SRAM		0x17d00000
#define ABOX_SRAM_SIZE		0x31000
#define  IPC_TX			0x30000
#define  IPC_TX_ACK		0x302fc
#define  IPC_RX			0x30300
#define  IPC_RX_ACK		0x305fc
#define ABOX_GICD		0x17cf1000
#define ABOX_GICC		0x17cf2000

#define SYSMMU_AUD		0x17e00000
#define  MMU_CTRL		0x000
#define  MMU_CFG		0x004
#define  MMU_STATUS		0x008
#define  MMU_PT_BASE		0x00c
#define  MMU_FLUSH_ALL		0x010
#define  MMU_INT_STATUS		0x060
#define  MMU_INT_CLEAR		0x064
#define  MMU_FAULT_VA		0x070
#define  MMU_FAULT_INFO		0x078

#define IOVA_DRAM		0x80000000UL
#define IOVA_IVA		0x90000000UL
#define DRAM_SIZE		(SZ_16M + SZ_2M)
#define IVA_SIZE		SZ_512K
/* Shared with the modem on the stock system: the voice firmware and its
 * parameters. There is no modem driver here, so the DSP gets empty memory
 * where it looks for them. */
#define IOVA_VSS		0xa0000000UL
#define IOVA_VPARAM		0xa1000000UL
#define VSS_SIZE		SZ_16M
#define VPARAM_SIZE		SZ_16M
/* Where the modem's boot daemon (cbd) puts the voice firmware, VSS, which runs
 * on this DSP during calls: CP shared memory at 0xf0000000, after the CP's
 * own 0x7900000 (stock DT shmem,cp_size and shmem,vss_size). Samsung's
 * abox_vss clears a word at magic_offset in it before the DSP starts. */
#define CP_VSS_PA		0xf7900000UL
#define CP_VSS_SIZE		0x700000
#define CP_VSS_MAGIC		0x600000
#define LV2_TABLES		64

/* messages, as in Samsung's abox_ipc.h */
#define IPC_SYSTEM		1
#define IPC_PCMPLAYBACK		2
#define IPC_ABOX_CONFIG		0xa
#define  ABOX_SUSPEND		1
#define  ABOX_RESUME		2
#define  SET_MIXER_SAMPLE_RATE	1
#define  SET_MIXER_FORMAT	0x10
#define  PCM_PLTDAI_OPEN	11
#define  PCM_PLTDAI_CLOSE	12
#define  PCM_PLTDAI_HW_PARAMS	14
#define  PCM_PLTDAI_PREPARE	16
#define  PCM_PLTDAI_TRIGGER	17
#define  PCM_SET_BUFFER		20
#define IPC_MSG_BYTES		764

#define IOVA_RDMA0		0x91000000UL
#define IOVA_RDMA1		0x91100000UL	/* IOVA_RDMA_BUFFER(1) */
/* The call's own streams, as Samsung's HAL opens them for a CP call
 * (libaudioproxy proxy_start_voice_call): voice RX on RDMA4 (pcmC0D4p) and
 * voice TX on WDMA2 (pcmC0D10c), 48 kHz, 16 bit, stereo, four periods of 480
 * frames, started with no data of ours: in call mode the DSP's voice firmware
 * fills and drains them. */
#define IOVA_VOICE_RX		0x91400000UL	/* Samsung's IOVA_RDMA_BUFFER(4) */
#define IOVA_VOICE_TX		0x92200000UL	/* IOVA_WDMA_BUFFER(2) */
#define VOICE_RDMA		4
#define VOICE_WDMA		2
#define VOICE_PERIOD		(480 * 4)
#define VOICE_PERIODS		4
#define VOICE_BYTES		SZ_16K
/* Samsung's speaker management (Maxim DSM) in the DSP, as the stock HAL runs
 * it for media on the speaker (audio/DSM.md): its output on RDMA7 and the
 * amplifiers' current and voltage coming back on WDMA4, both with no data of
 * ours, 480 x 4 frames like the voice streams. */
#define IOVA_DSM_OUT		0x91700000UL	/* IOVA_RDMA_BUFFER(7): the firmware may expect Samsung's */
#define IOVA_DSM_VI		0x92400000UL	/* IOVA_WDMA_BUFFER(4) */
#define DSM_RDMA		7
#define DSM_WDMA		4
#define IPC_ERAP		5
#define  REALTIME_OPEN		1
#define  REALTIME_CLOSE		2
#define  REALTIME_EXTRA		0xea
#define  ERAP_VI_SENSE		1
#define DSM_PARAMS		185	/* per speaker; the right one's follow the left one's */
#define  DSM_ENABLE		1
#define  DSM_RDC_AT_ROOMTEMP	6
#define  DSM_COLDTEMP		8
#define  DSM_WRITE_FLAG		63
#define  DSM_WRITE_RDC_CAL_ONLY	0xca00ca00
#define ABOX_ROUTE_CTRL1	0x0044
#define ABOX_ROUTE_CTRL2	0x0048
#define ABOX_SPUM_CTRL0		0x0300
#define ABOX_SPUM_CTRL1		0x0304
#define IPC_PCMCAPTURE		3
#define RATE			48000
#define PERIOD_FRAMES		1024
#define PERIODS			8
#define TONE_BYTES		(PERIOD_FRAMES * PERIODS * 4)


#define  PCM_PLTDAI_HW_FREE	15
#define  PCM_PLTDAI_POINTER	18
#define RDMA_STATUS		0x30
#define ABOX_SPI		76	/* the block's interrupt to us, on the main GIC */
#define AMP_BUS			98
#define AMP_L			0x39
#define AMP_R			0x3a
#define EAR_GAIN		0x01	/* +3 dB: stock maxim,spk-gain-rcv */
#define EAR_DIGITAL		0x34	/* -3 dB: stock maxim,digital-gain-rcv */
#define AMP_VOL_MAX		0x40	/* 0 dB. Behind PipeWire's limiter and high-pass (see the note on DSM above). */
/* the DSP's position register wraps at 64K: no ring larger than that */
#define BUFFER_BYTES		SZ_64K
#define  ABOX_CHANGE_GEAR	4
#define  ABOX_SET_MODE		0x50
#define  ABOX_SET_TYPE		0x60
#define  ABOX_START_VSS		0xa0
#define  MODE_NORMAL		0
#define  MODE_IN_CALL		2
#define  SOUND_TYPE_VOICE	0
#define  SOUND_TYPE_SPEAKER	1
#define  ABOX_CHANGED_GEAR	9
#define CMU_DIV_PLL		0x1820
#define AUD_PLL_HZ		1179648000UL

static struct platform_device *pdev, *i2c_pdev;
static void __iomem *pmu, *sfr, *sysreg, *sram, *gicd, *gicc, *mmu, *cmu, *pins;
static void *dram, *iva, *vss, *vparam, *lv1, *lv2, *voice_rx, *voice_tx, *dsm_out, *dsm_vi;
static dma_addr_t dram_pa, iva_pa, vss_pa, vparam_pa, lv1_pa, lv2_pa, voice_rx_pa, voice_tx_pa, dsm_out_pa, dsm_vi_pa;
static int lv2_used = 1;	/* table 0 stays empty: what unmapped space points at */
static bool started, booted;
static bool suspending, dsp_fault, irq_quiesced;
static void dsp_recover(void);
static const struct firmware *fw_sram, *fw_dram, *fw_iva;
static struct snd_pcm *playback[2];
static DEFINE_MUTEX(stream_lock);
static struct delayed_work watchdog_work;
static unsigned int watchdog_recoveries;
module_param(watchdog_recoveries, uint, 0444);
static u32 fw_version;
static DEFINE_SPINLOCK(ipc_lock);
static int abox_irq;
static struct task_struct *rx_task;	/* only when the interrupt could not be had */
static struct snd_card *card;
/*
 * Two playback devices, two of the DSP's eight playback DMAs (RDMA0 and
 * RDMA1): its mixer sums every stream into SIFS0 and on to the speakers, so
 * hw:0,0 (PipeWire's) and hw:0,1 (Android's, without PipeWire in between)
 * play at once without a sound server mixing them.
 */
struct stream {
	int rdma;				/* which DMA: its messages, registers, interrupt */
	unsigned long iova;
	void *buf;
	dma_addr_t buf_pa;
	struct snd_pcm_substream *active;	/* the open stream, for period interrupts */
	size_t last_pos;			/* its position as last told (see abox_pcm_pointer) */
	int stalled;				/* reads in a row with the DMA not running */
	bool running, needs_reset;
	u32 watch_status;
	unsigned long watch_progress, irq_progress;
	/* its real rate, measured by the watchdog (see abox_watchdog) */
	snd_pcm_uframes_t rate_from;
	u64 rate_since;
};
static struct stream streams[2] = { { .rdma = 0, .iova = IOVA_RDMA0 }, { .rdma = 1, .iova = IOVA_RDMA1 } };

static bool any_active(void)
{
	return READ_ONCE(streams[0].active) || READ_ONCE(streams[1].active);
}
static struct i2c_adapter *amp_bus;
static int amp_volume = 0x20;	/* -8 dB */
static bool amp_on;
static struct delayed_work gear_work, gear_again;
static int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "log this many of the DSP's next messages");
static bool cp_vss = true;
module_param(cp_vss, bool, 0444);
MODULE_PARM_DESC(cp_vss, "map the modem's voice firmware (VSS) for the DSP (default on)");
static int rcv_amp = AMP_L;
module_param(rcv_amp, int, 0644);
MODULE_PARM_DESC(rcv_amp, "I2C address of the amplifier that drives the earpiece (0x39 or 0x3a)");
static int call_route;	/* 0 none, 1 earpiece, 2 speaker */
/* The speakers' factory calibration from EFS (/efs/maxim/rdc_cal, rdc_cal_r,
 * temp_cal: hex text), given at boot by zrn-audio; 0 = unknown, no DSM. */
static int dsm_rdc[2], dsm_temp;
static int n_rdc;
module_param_array(dsm_rdc, int, &n_rdc, 0644);
module_param(dsm_temp, int, 0644);
MODULE_PARM_DESC(dsm_rdc, "speaker calibration (EFS rdc_cal, rdc_cal_r)");
MODULE_PARM_DESC(dsm_temp, "calibration temperature (EFS temp_cal, tenths of a degree)");
static bool dsm_on;
static u32 dsm_rx[8];	/* the DSP's last DSM answer, first words */
static int n_dsm_rx = 8;
module_param_array(dsm_rx, uint, &n_dsm_rx, 0444);
static bool vss_started;
static int pointer_stalls, pointer_backsteps;
module_param(pointer_stalls, int, 0444);
module_param(pointer_backsteps, int, 0444);
static DEFINE_MUTEX(amp_lock);

/*
 * Capture from the main microphones (audio/MIC-REA.md). Off unless the module
 * is loaded with capture=1: then a third device, hw:0,2 "Microphone", appears,
 * and nothing of it touches the hardware until it is opened. The path is the
 * stock "route-main-mic" one: the two digital microphones on IN3L/IN3R of the
 * CS47L93 codec (SPI, bit-banged here) -> codec AIF1 TX -> ABOX UAIF0 (the
 * SoC drives the bit clock) -> NSRC0 -> SIFM0 -> WDMA1 -> the ALSA buffer.
 */
static bool capture;
module_param(capture, bool, 0444);
MODULE_PARM_DESC(capture, "offer the main microphones as hw:0,2 (default off; see audio/MIC-REA.md)");
static int cap_wdma = 1;	/* stock: route-nsrc0-to-wdma1, capture_link 9 */
module_param(cap_wdma, int, 0444);
MODULE_PARM_DESC(cap_wdma, "the DSP's capture DMA behind NSRC0/SIFM0 (0 or 1; stock 1)");
#define WDMA0_BUF_FULL		8	/* the DSP's interrupt for WDMA0; WDMA1's is 9 */
struct cap_stream {
	void *buf;
	dma_addr_t buf_pa;
	struct snd_pcm_substream *active;	/* prepared, for period interrupts */
	size_t last_pos;
	int stalled;
	bool opened, running, needs_reset, path_on, codec_up;
};
static struct cap_stream cap;
static struct snd_pcm *capture_pcm;
static unsigned int cap_irqs, cap_msgs;
module_param(cap_irqs, uint, 0444);
module_param(cap_msgs, uint, 0444);

static void pmu_update(u32 reg, u32 mask, u32 val)
{
	writel((readl(pmu + reg) & ~mask) | (val & mask), pmu + reg);
}

/* The core held in reset and unpowered, whatever it was doing. */
static void core_off(void)
{
	int i;

	pmu_update(PMU_ABOX_CPU_OPTION, ABOX_CPU_ENABLE, 0);
	pmu_update(PMU_ABOX_CPU_CFG, 0x1, 0x0);
	for (i = 0; i < 100 && (readl(pmu + PMU_ABOX_CPU_STATUS) & 0x1); i++)
		usleep_range(1000, 2000);
	}

/* One 4K page of the DSP's address space onto one of ours. */
static int mmu_map_page(unsigned long iova, phys_addr_t pa)
{
	u32 *l1 = lv1, *l2;
	u32 link = l1[iova >> 20];
	phys_addr_t table;

	if (link == (((u32)(lv2_pa >> 4)) | 1)) {
		if (lv2_used == LV2_TABLES)
			return -ENOMEM;
		table = lv2_pa + lv2_used * SZ_1K;
		lv2_used++;
		l1[iova >> 20] = (u32)(table >> 4) | 1;
	} else {
		table = (phys_addr_t)(link & ~0xfU) << 4;
	}
	l2 = lv2 + (table - lv2_pa);
	l2[(iova >> 12) & 0xff] = (u32)(pa >> 4) | (3 << 2) | 2;	/* read and write, small page */
	return 0;
}

static int mmu_map(unsigned long iova, phys_addr_t pa, size_t size)
{
	size_t off;
	int ret;

	for (off = 0; off < size; ) {
		/* a whole megabyte at once where everything lines up */
		if (!((iova + off) & (SZ_1M - 1)) && !((pa + off) & (SZ_1M - 1)) &&
				size - off >= SZ_1M) {
			((u32 *)lv1)[(iova + off) >> 20] = (u32)((pa + off) >> 4) | (3 << 4) | 2;
			off += SZ_1M;
			continue;
		}
		ret = mmu_map_page(iova + off, pa + off);
		if (ret)
			return ret;
		off += SZ_4K;
	}
	return 0;
}

/* Send the DSP one message: into its mailbox, flag it, and raise the software
 * interrupt on its controller whose number is the message's kind. Called with
 * interrupts off from the stream's trigger, so it only ever spins briefly. */
#define IPC_TRIES	10
static unsigned int ipc_retries;
module_param(ipc_retries, uint, 0444);

static int ipc_send(const u32 *words, int n)
{
	u32 msg[IPC_MSG_BYTES / 4] = { 0 };
	unsigned long flags;
	int i, try, ret = 0;

	if (!booted)
		return -ENODEV;
	if (n < 3 || n > ARRAY_SIZE(msg))
		return -EINVAL;
	memcpy(msg, words, n * 4);
	/* Samsung's __abox_process_ipc: 10 ms for the DSP to take it, sent
	 * again up to ten times before it counts as failed. A busy DSP can take
	 * longer than one try: given up after the first, the DSP was restarted
	 * under a playing stream, which then stayed stuck (2026-10-25). */
	for (try = 0; try < IPC_TRIES; try++) {
		spin_lock_irqsave(&ipc_lock, flags);
		memcpy_toio(sram + IPC_TX, msg, sizeof(msg));
		writel(1, sram + IPC_TX_ACK);
		writel(BIT(16) | (words[0] & 0xf), gicd + 0xf00);
		for (i = 0; i < 2000 && readl(sram + IPC_TX_ACK); i++)
			udelay(5);
		ret = readl(sram + IPC_TX_ACK) ? -EIO : 0;
		if (ret && try == IPC_TRIES - 1)
			writel(0, sram + IPC_TX_ACK);
		spin_unlock_irqrestore(&ipc_lock, flags);
		if (!ret)
			break;
		ipc_retries++;
	}
	if (ret) {
		WRITE_ONCE(dsp_fault, true);
		pr_err("snd-zrn-abox: the DSP did not take message %u/%u\n", words[0], words[2]);
	}
	return ret;
}

/* the same to another stream: playback or capture, by DMA number */
static int dma_msg(u32 ipc, u32 channel, u32 type, u32 a, u32 b, u32 c)
{
	/* the DMA's number twice, as task and as channel (Samsung's msg.task_id =
	 * channel_id): with task 0, the DSP took another stream's messages for
	 * RDMA0's, and closing hw:0,1 stopped hw:0,0 under PipeWire */
	u32 m[7] = { ipc, channel, type, channel, a, b, c };

	return ipc_send(m, 7);
}

static void voice_stream(u32 ipc, u32 channel, u32 iova, bool on)
{
	if (on) {
		dma_msg(ipc, channel, PCM_PLTDAI_OPEN, 0, 0, 0);
		dma_msg(ipc, channel, PCM_SET_BUFFER, iova, VOICE_PERIOD, VOICE_PERIODS);
		dma_msg(ipc, channel, PCM_PLTDAI_HW_PARAMS, RATE, 16, 2);
		dma_msg(ipc, channel, PCM_PLTDAI_PREPARE, 0, 0, 0);
		dma_msg(ipc, channel, PCM_PLTDAI_TRIGGER, 1, 0, 0);
	} else {
		dma_msg(ipc, channel, PCM_PLTDAI_TRIGGER, 0, 0, 0);
		dma_msg(ipc, channel, PCM_PLTDAI_HW_FREE, 0, 0, 0);
		dma_msg(ipc, channel, PCM_PLTDAI_CLOSE, 0, 0, 0);
	}
}

/*
 * The firmware keeps time by its own core's clock, and has to be told how
 * fast that is: it asks for a "gear" (a divider of the audio PLL) and waits to
 * hear what it got. Untold, it assumes the slowest gear, PLL / 12, while the
 * core here runs at whatever the bootloader left, PLL / 4, and so it played
 * everything three times too fast. The clock is left alone and the firmware
 * told the truth, at boot and every time it asks.
 */
/* streams rebuilt because they ran too fast (abox_watchdog) */
static unsigned int fast_rebuilds;
module_param(fast_rebuilds, uint, 0444);
/* 1: on a fast stream, power the audio block off and on and start the DSP
 * afresh (dsp_recover, aud_power_cycle). A DSP restart alone did not cure it
 * (2026-10-25): the rate converter stays locked until the block loses power. */
static bool rate_guard;
module_param(rate_guard, bool, 0644);

static unsigned int test_clock_hz;
module_param(test_clock_hz, uint, 0644);

/*
 * The DSP's real clock belongs to the APM firmware, not to CMU_AUD: its DVFS
 * domain dvfs_aud (ID 9) sets it, and playback speed followed the level
 * exactly (394 MHz gave 180,047 frames a second, 1180 MHz 542,411), whatever
 * the clock registers here said (2026-10-25). Samsung's driver answers every
 * gear the DSP asks for by setting that level (pm_qos_aud: abox.c
 * abox_change_cpu_gear) and then telling the DSP the frequency it applied;
 * a DSP told one thing while running at another plays at the wrong speed:
 * 2.8 times fast, the "broken audio" of that whole day. This does the same,
 * through the APM's mailbox, as the vendor acpm_ipc.c protocol and the
 * s9p-acpm tool: channel 0 takes DVFS requests [domain, kHz, 0 = set, 0].
 */
#define APM_MBOX		0x14100000
#define APM_SRAM		0x02039000
#define APM_SRAM_SIZE		0x30000
#define APM_SRAM_MAP		0x3000	/* the first three pages fault */
#define APM_INITDATA		0x7f00
#define APM_INTGR0		0x08
#define APM_INTGR1		0x1c
#define APM_INTCR1		0x20
#define APM_INTSR1		0x28
#define APM_DVFS_CHANNEL	0
#define APM_DVFS_AUD		9

static void __iomem *apm_mbox, *apm_sram;
static DEFINE_MUTEX(apm_lock);
static struct {
	bool ok;
	u32 id, q_len, elem;
	u32 tx_rear, tx_front, tx_base;		/* AP to APM: the APM's rx queue */
	u32 rx_rear, rx_front, rx_base;		/* APM to AP */
} apm_ch;
static u32 apm_seq = 1;

static u32 apm_rd(u32 off)
{
	return readl(apm_sram + off - APM_SRAM_MAP);
}

static void apm_wr(u32 off, u32 v)
{
	writel(v, apm_sram + off - APM_SRAM_MAP);
}

static bool apm_off_ok(u32 off)
{
	return off >= APM_SRAM_MAP && off < APM_SRAM_SIZE - 16;
}

static void apm_init(void)
{
	u32 table, ap_max, i;

	apm_mbox = ioremap(APM_MBOX, 0x1000);
	apm_sram = ioremap(APM_SRAM + APM_SRAM_MAP, APM_SRAM_SIZE - APM_SRAM_MAP);
	if (!apm_mbox || !apm_sram)
		return;
	table = apm_rd(APM_INITDATA + 2 * 4);	/* ipc_channels */
	ap_max = apm_rd(APM_INITDATA + 6 * 4);	/* ipc_ap_max */
	for (i = 0; i < ap_max && i < 16 && apm_off_ok(table + i * 72); i++) {
		u32 base = table + i * 72;

		if (apm_rd(base) != APM_DVFS_CHANNEL)
			continue;
		apm_ch.id = apm_rd(base);
		apm_ch.tx_rear = apm_rd(base + 4 * 4);
		apm_ch.tx_front = apm_rd(base + 5 * 4);
		apm_ch.tx_base = apm_rd(base + 6 * 4);
		apm_ch.rx_rear = apm_rd(base + 10 * 4);
		apm_ch.rx_front = apm_rd(base + 11 * 4);
		apm_ch.rx_base = apm_rd(base + 12 * 4);
		apm_ch.q_len = apm_rd(base + 13 * 4);
		apm_ch.elem = apm_rd(base + 14 * 4);
		apm_ch.ok = apm_ch.q_len > 1 && apm_ch.q_len < 64 && apm_ch.elem >= 16 &&
			apm_off_ok(apm_ch.tx_rear) && apm_off_ok(apm_ch.tx_front) && apm_off_ok(apm_ch.tx_base) &&
			apm_off_ok(apm_ch.rx_rear) && apm_off_ok(apm_ch.rx_front) && apm_off_ok(apm_ch.rx_base);
		break;
	}
	pr_info("snd-zrn-abox: APM DVFS channel %s (q_len %u, elem %u)\n",
		apm_ch.ok ? "found" : "not usable", apm_ch.q_len, apm_ch.elem);
}

/* the audio DVFS level, in kHz; 0 when it was applied */
static int apm_aud_set(u32 khz)
{
	u32 seq, front, nxt, i, k, rear;
	unsigned long until;
	int ret = -ETIMEDOUT;

	if (!apm_ch.ok)
		return -ENODEV;
	mutex_lock(&apm_lock);
	seq = apm_seq = apm_seq % 63 + 1;
	front = apm_rd(apm_ch.tx_front);
	nxt = (front + 1) % apm_ch.q_len;
	until = jiffies + HZ;
	while (nxt == apm_rd(apm_ch.tx_rear)) {
		if (time_after(jiffies, until))
			goto out;
		usleep_range(200, 400);
	}
	apm_wr(apm_ch.tx_base + apm_ch.elem * front, APM_DVFS_AUD | (seq << 16));
	apm_wr(apm_ch.tx_base + apm_ch.elem * front + 4, khz);
	apm_wr(apm_ch.tx_base + apm_ch.elem * front + 8, 0);
	apm_wr(apm_ch.tx_base + apm_ch.elem * front + 12, 0);
	apm_wr(apm_ch.tx_front, nxt);
	writel((1 << apm_ch.id) << 16, apm_mbox + APM_INTGR0);
	until = jiffies + HZ;
	while (!time_after(jiffies, until)) {
		if (readl(apm_mbox + APM_INTSR1) & (1 << apm_ch.id)) {
			front = apm_rd(apm_ch.rx_front);
			rear = apm_rd(apm_ch.rx_rear);
			for (i = rear; i != front; i = (i + 1) % apm_ch.q_len) {
				u32 w0 = apm_rd(apm_ch.rx_base + apm_ch.elem * i);

				if (((w0 >> 16) & 0x3f) != seq)
					continue;
				if (i != rear)	/* the entry at rear into this slot, then rear out */
					for (k = 0; k < apm_ch.elem / 4; k++)
						apm_wr(apm_ch.rx_base + apm_ch.elem * i + 4 * k,
							apm_rd(apm_ch.rx_base + apm_ch.elem * rear + 4 * k));
				rear = (rear + 1) % apm_ch.q_len;
				apm_wr(apm_ch.rx_rear, rear);
				if (rear == apm_rd(apm_ch.rx_front)) {
					writel(1 << apm_ch.id, apm_mbox + APM_INTCR1);
					if (rear != apm_rd(apm_ch.rx_front))
						writel(1 << apm_ch.id, apm_mbox + APM_INTGR1);
				}
				ret = (w0 & 0xffff) ? -EIO : 0;
				goto out;
			}
		}
		usleep_range(200, 400);
	}
out:
	mutex_unlock(&apm_lock);
	if (ret)
		pr_warn("snd-zrn-abox: APM audio level %u kHz: %d\n", khz, ret);
	return ret;
}

/* Samsung's levels for gears 1..4 (stock DT pm_qos_aud); 5 and up: none */
static const u32 aud_gear_khz[] = { 1180000, 800000, 590000, 394000 };
#define GEAR_BOOT	0xb00d		/* Samsung's own request while the DSP boots */
static DEFINE_SPINLOCK(gear_lock);
static struct { u32 id, gear; } gear_reqs[16];
static u32 gear_applied_khz;		/* the level the APM runs the DSP at */
static unsigned int gear_requests;
module_param(gear_requests, uint, 0444);

static void gear_request(u32 id, u32 gear)
{
	unsigned long flags;
	int i, free = -1;

	spin_lock_irqsave(&gear_lock, flags);
	for (i = 0; i < ARRAY_SIZE(gear_reqs); i++) {
		if (gear_reqs[i].id == id)
			break;
		if (!gear_reqs[i].id && free < 0)
			free = i;
	}
	if (i == ARRAY_SIZE(gear_reqs))
		i = free;
	if (i >= 0) {
		gear_reqs[i].id = id;
		gear_reqs[i].gear = gear;
	}
	spin_unlock_irqrestore(&gear_lock, flags);
	gear_requests++;
}

/* a fixed audio level in kHz instead of the gears (0: follow the gears), to
 * find which level the firmware's timing matches */
static unsigned int aud_khz;
/* the clock told again at every prepare (Samsung tells it only when asked) */
static bool tell_on_prepare;
module_param(tell_on_prepare, bool, 0644);
/* SPUS routing at DSP start; Samsung (TRY_TO_ASRC_OFF) leaves the rate
 * converter out when a stream's rate is the mixer's: 0x15333322 */
static unsigned int spus_ctrl0 = 0x15333333;
module_param(spus_ctrl0, uint, 0444);
/* Samsung's TRY_TO_ASRC_OFF (stock DT quirk "try to asrc off", abox.c
 * abox_try_to_asrc_off): a stream at the mixer's rate and width goes past the
 * rate converter (Samsung sets it before each PREPARE; here once at DSP start,
 * see dsp_boot), with SIFS0 left unpaced (count
 * 0: the speaker interface sets the pace, as Samsung for playback). Through the
 * converter our 48 kHz streams at random locked 2.8-3.75 times too fast until
 * the block lost power (2026-10-25). */
static bool asrc_off;
module_param(asrc_off, bool, 0444);
static bool uaif_off_on_close = true;
module_param(uaif_off_on_close, bool, 0644);
static int aud_khz_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	if (!ret)
		mod_delayed_work(system_wq, &gear_work, 0);
	return ret;
}
static const struct kernel_param_ops aud_khz_ops = {
	.set = aud_khz_set,
	.get = param_get_uint,
};
module_param_cb(aud_khz, &aud_khz_ops, &aud_khz, 0644);

/* the level the requests ask for: the lowest gear number wins (Samsung) */
static u32 gear_wanted_khz(void)
{
	unsigned long flags;
	u32 gear = UINT_MAX;
	int i;

	if (aud_khz)
		return aud_khz;

	spin_lock_irqsave(&gear_lock, flags);
	for (i = 0; i < ARRAY_SIZE(gear_reqs); i++)
		if (gear_reqs[i].id && gear_reqs[i].gear < gear)
			gear = gear_reqs[i].gear;
	spin_unlock_irqrestore(&gear_lock, flags);
	if (gear < 1)
		gear = 1;
	if (gear > ARRAY_SIZE(aud_gear_khz))
		return aud_gear_khz[ARRAY_SIZE(aud_gear_khz) - 1];	/* idle: the lowest level */
	return aud_gear_khz[gear - 1];
}

/* the level the requests want, applied, then told to the DSP */
static void gear_apply(void)
{
	u32 khz = gear_wanted_khz();

	if (khz != gear_applied_khz && !apm_aud_set(khz))
		gear_applied_khz = khz;
}

static void gear_tell(struct work_struct *work)
{
	u64 now = ktime_get_ns();
	u32 hz;
	u32 m[6] = { IPC_SYSTEM, 0, ABOX_CHANGED_GEAR, 0, 0, 0 };

	if (work)
		gear_apply();
	/* what the APM runs it at; without the APM, what CMU_AUD says */
	hz = gear_applied_khz ? gear_applied_khz * 1000 :
		AUD_PLL_HZ / ((readl(cmu + CMU_DIV_PLL) & 0xf) + 1);
	m[3] = test_clock_hz ? test_clock_hz : hz;

	m[5] = do_div(now, NSEC_PER_SEC);
	m[4] = (u32)now;
	ipc_send(m, 6);
}

/* Whatever the DSP has put in its mailbox for us: a period has been played,
 * or housekeeping of its own that needs no answer here. */
static void rx_one(void)
{
	u32 id, type, msg[11];
	unsigned long flags;
	static DEFINE_SPINLOCK(rx_lock);

	spin_lock_irqsave(&rx_lock, flags);
	if (!readl(sram + IPC_RX_ACK)) {
		spin_unlock_irqrestore(&rx_lock, flags);
		return;
	}
	/* Once acknowledged, the firmware may reuse every payload word. */
	memcpy_fromio(msg, sram + IPC_RX, sizeof(msg));
	writel(0, sram + IPC_RX_ACK);
	spin_unlock_irqrestore(&rx_lock, flags);
	id = msg[0];
	type = msg[2];
	if (debug > 0) {
		debug--;
		pr_info("snd-zrn-abox: from DSP: id %u task %d: %u %08x %08x %08x\n", id, msg[1],
			type, msg[3], msg[4], msg[5]);
	}
	if (id == IPC_SYSTEM && type == 3) {
		fw_version = msg[5];
		booted = true;
	} else if (id == IPC_ERAP) {
		int i;

		for (i = 0; i < 8; i++)
			dsm_rx[i] = msg[2 + i];
		pr_info("snd-zrn-abox: DSM: %08x %08x %08x %08x %08x %08x\n", dsm_rx[0], dsm_rx[1],
			dsm_rx[2], dsm_rx[3], dsm_rx[4], dsm_rx[5]);
	} else if (id == IPC_SYSTEM && type == ABOX_CHANGE_GEAR) {
		/* param1 the gear, param2 who asks (Samsung: abox_request_cpu_gear);
		 * answered once the APM has applied the level (gear_tell) */
		static unsigned long logged;

		if (!logged || time_after(jiffies, logged + 10 * HZ)) {
			logged = jiffies;
			pr_info("snd-zrn-abox: DSP asks for gear %u (%#x)\n", msg[3], msg[4]);
		}
		gear_request(msg[4], msg[3]);
		mod_delayed_work(system_wq, &gear_work, 0);
	} else if (id == IPC_PCMPLAYBACK && type == PCM_PLTDAI_POINTER) {
		u32 channel = msg[3];
		struct snd_pcm_substream *s;

		rcu_read_lock();
		s = channel < 2 ? READ_ONCE(streams[channel].active) : NULL;

		if (s) {
			WRITE_ONCE(streams[channel].irq_progress, jiffies);
			snd_pcm_period_elapsed(s);
		}
		rcu_read_unlock();
	} else if (id == IPC_PCMCAPTURE && type == PCM_PLTDAI_POINTER && capture &&
		   msg[3] == (u32)cap_wdma) {
		/* the microphone stream (abox_wdma_irq_handler); the call's and
		 * DSM's capture DMAs (WDMA2, WDMA4) are left alone as before */
		struct snd_pcm_substream *s;

		cap_msgs++;
		rcu_read_lock();
		s = READ_ONCE(cap.active);
		if (s)
			snd_pcm_period_elapsed(s);
		rcu_read_unlock();
	}
}

/* The DSP's interrupts arrive through its own controller: acknowledge them
 * there, as Samsung's driver does. Their numbers say what happened: 11 is
 * "stream 0 has played a period" and carries no message; the rest come with
 * something in the mailbox. */
#define RDMA0_BUF_EMPTY		0xb
static irqreturn_t abox_isr(int irq, void *unused)
{
	irqreturn_t ret = IRQ_NONE;
	bool period[2] = { false, false }, cap_period = false;
	int i;

	for (i = 0; i < 32; i++) {
		u32 stat = readl(gicc + 0x00c), nr = stat & 0x3ff;

		if (nr >= 1020)
			break;
		writel(stat, gicc + 0x010);
		if (nr < 16)
			writel(stat, gicc + 0x1000);
		if (nr == RDMA0_BUF_EMPTY || nr == RDMA0_BUF_EMPTY + 1)
			period[nr - RDMA0_BUF_EMPTY] = true;	/* 12 is RDMA1's */
		if (capture && nr == WDMA0_BUF_FULL + cap_wdma)
			cap_period = true;
		if (debug > 0) {
			debug--;
			pr_info("snd-zrn-abox: DSP interrupt %u (%08x)\n", nr, stat);
		}
		ret = IRQ_HANDLED;
	}
	rcu_read_lock();
	for (i = 0; i < 2; i++) {
		struct snd_pcm_substream *s = READ_ONCE(streams[i].active);

		if (period[i] && s) {
			WRITE_ONCE(streams[i].irq_progress, jiffies);
			snd_pcm_period_elapsed(s);
		}
	}
	if (cap_period) {
		struct snd_pcm_substream *s = READ_ONCE(cap.active);

		cap_irqs++;
		if (s)
			snd_pcm_period_elapsed(s);
	}
	rcu_read_unlock();
	rx_one();
	return ret;
}

static int rx_fn(void *unused)
{
	while (!kthread_should_stop()) {
		if (!READ_ONCE(suspending))
			rx_one();
		usleep_range(900, 1100);
	}
	return 0;
}

/*
 * The audio block's clocks as the bootloader left them, kept from probe and
 * compared after a SoC sleep, before the DSP starts again. After one sleep the DSP
 * played 2.8 times too fast (134,773 frames a second for 48,000: a 700 Hz tone
 * came out at 1980 Hz), and telling it its clock again changed nothing. Only
 * the registers Samsung's clock tables list are touched: an unlisted offset
 * here faults (PLL_CON0 + 4 did, read from user space, 2026-10-25).
 */
static const u16 cmu_saved_regs[] = {
	0x0100, 0x0120,						/* CPU_USER mux, PLL_AUD */
	0x1000, 0x1004, 0x1008, 0x100c, 0x1010, 0x1014,	/* muxes */
	0x1800, 0x1804, 0x1808, 0x180c, 0x1810, 0x1814,	/* dividers */
	0x1818, 0x181c, 0x1820, 0x1824, 0x1828, 0x182c, 0x1830,
};
static u32 cmu_saved[ARRAY_SIZE(cmu_saved_regs)];

/* ---- the audio block's power ----------------------------------------------
 *
 * Samsung powers the whole block off 500 ms after its last stream (runtime PM,
 * pd-aud): every session starts from cold hardware. Ours stayed powered from
 * boot, so a rate converter locked to the wrong ratio stayed that way until a
 * reboot. The sequence is Samsung's (cal-if flexpmu_cal_local_exynos9810.h,
 * blkpwr_aud): save the clocks, tell the secure monitor (the block's TZPC,
 * stock DT need_smc), power off, power on, tell it again, restore in order.
 * Without the secure monitor calls the block comes back closed to us.
 */
#define PMU_AUD_CONFIGURATION	0x4000
#define SMC_PD_ONOFF		0x82000410	/* SMC_CMD_PREAPRE_PD_ONOFF */
#define  PD_SAVE		0		/* EXYNOS_GET_IN_PD_DOWN */
#define  PD_RESTORE		1		/* EXYNOS_WAKEUP_PD_DOWN */
#define AUD_TZPC		0x17c10204
#define AUD_PLL_CON0		0x0120
static const struct { bool sys; u16 off; u32 mask; } aud_regs[] = {
	{ 0, 0x0000, ~0U }, { 0, 0x0144, ~0U }, { 0, 0x0168, ~0U }, { 0, 0x018c, ~0U },
	{ 0, 0x01b0, ~0U },
	{ 0, 0x1830, ~0U }, { 0, 0x182c, ~0U }, { 0, 0x1828, ~0U }, { 0, 0x1824, ~0U },
	{ 0, 0x1820, ~0U }, { 0, 0x181c, ~0U }, { 0, 0x1818, ~0U }, { 0, 0x1814, ~0U },
	{ 0, 0x1810, ~0U }, { 0, 0x180c, ~0U }, { 0, 0x1808, ~0U }, { 0, 0x1804, ~0U },
	{ 0, 0x1800, ~0U },
	{ 0, 0x1014, ~0U }, { 0, 0x1010, ~0U }, { 0, 0x100c, ~0U }, { 0, 0x1008, ~0U },
	{ 0, 0x1004, ~0U }, { 0, 0x1000, ~0U },
	{ 0, 0x0100, ~0U }, { 0, AUD_PLL_CON0, ~0U },	/* then its lock, if it runs */
	{ 0, 0x0850, ~0U },
	{ 0, 0x306c, ~0U }, { 0, 0x3068, ~0U }, { 0, 0x3064, ~0U }, { 0, 0x3060, ~0U },
	{ 0, 0x305c, ~0U }, { 0, 0x3058, ~0U }, { 0, 0x3054, ~0U }, { 0, 0x3050, ~0U },
	{ 0, 0x304c, ~0U }, { 0, 0x3048, ~0U }, { 0, 0x3044, ~0U }, { 0, 0x3040, ~0U },
	{ 0, 0x303c, ~0U }, { 0, 0x3038, ~0U }, { 0, 0x3034, ~0U }, { 0, 0x3030, ~0U },
	{ 0, 0x302c, ~0U }, { 0, 0x3028, ~0U }, { 0, 0x3024, ~0U },
	{ 0, 0x3008, ~0U }, { 0, 0x3004, ~0U }, { 0, 0x3000, ~0U },
	{ 0, 0x0800, 0x3U << 28 },
	{ 1, 0x0104, ~0U }, { 1, 0x0300, ~0U },
};
static u32 aud_saved[ARRAY_SIZE(aud_regs)];
/* held for every register read outside stream_lock (abox_pcm_pointer, the
 * effect controls): none may reach the block while it is off */
static DEFINE_SPINLOCK(aud_lock);
static bool aud_off;
static bool aud_broken;		/* it did not come back: never again until reboot */
static unsigned int aud_cycles;
module_param(aud_cycles, uint, 0444);
static bool aud_smc = true;
module_param(aud_smc, bool, 0644);

/* the last step of a power cycle reached, in a PMU register a watchdog reset
 * keeps (INFORM4): read after a hang to see where it stopped */
#define PMU_CRUMB		0x0810
static void crumb(u32 step)
{
	writel(0xab0c0000 | step, pmu + PMU_CRUMB);
	dsb(sy);
}

static void __iomem *aud_reg(int i)
{
	return (aud_regs[i].sys ? sysreg : cmu) + aud_regs[i].off;
}

static bool pmu_wait(u32 reg, u32 mask, u32 val)
{
	int i;

	for (i = 0; i < 1000; i++) {
		if ((readl(pmu + reg) & mask) == val)
			return true;
		udelay(10);
	}
	return false;
}

/* stream_lock held, the DSP stopped, its interrupt off */
static int aud_power_cycle(void)
{
	struct arm_smccc_res res;
	unsigned long flags;
	int i;

	if (aud_broken)
		return -EIO;
	for (i = 0; i < ARRAY_SIZE(aud_regs); i++)
		aud_saved[i] = readl(aud_reg(i)) & aud_regs[i].mask;
	spin_lock_irqsave(&aud_lock, flags);
	aud_off = true;
	spin_unlock_irqrestore(&aud_lock, flags);

	crumb(7);
	res.a0 = 0;
	if (aud_smc)
		arm_smccc_smc(SMC_PD_ONOFF, PD_SAVE, AUD_TZPC, 0, 0, 0, 0, 0, &res);
	crumb(8);
	if (res.a0) {
		/* nothing is off yet: carry on powered */
		pr_warn("snd-zrn-abox: secure monitor would not save the audio block (%lx)\n", res.a0);
		goto on;
	}
	pmu_update(PMU_AUD_CONFIGURATION, 0xf, 0x0);
	crumb(9);
	if (!pmu_wait(PMU_AUD_STATUS, 0xf, 0x0))
		pr_warn("snd-zrn-abox: audio block did not power off (%x)\n", readl(pmu + PMU_AUD_STATUS));
	crumb(10);
	pmu_update(PMU_AUD_CONFIGURATION, 0xf, 0xf);
	if (!pmu_wait(PMU_AUD_STATUS, 0xf, 0xf)) {
		aud_broken = true;	/* aud_off stays: nothing may touch it */
		pr_err("snd-zrn-abox: audio block did not power on (%x)\n", readl(pmu + PMU_AUD_STATUS));
		return -EIO;
	}
	crumb(11);
	res.a0 = 0;
	if (aud_smc)
		arm_smccc_smc(SMC_PD_ONOFF, PD_RESTORE, AUD_TZPC, 0, 0, 0, 0, 0, &res);
	crumb(12);
	if (res.a0)
		pr_warn("snd-zrn-abox: secure monitor would not restore the audio block (%lx)\n", res.a0);
	for (i = 0; i < ARRAY_SIZE(aud_regs); i++) {
		void __iomem *r = aud_reg(i);

		crumb(0x100 + i);
		if (aud_regs[i].mask == ~0U)
			writel(aud_saved[i], r);
		else
			writel((readl(r) & ~aud_regs[i].mask) | aud_saved[i], r);
		if (!aud_regs[i].sys && aud_regs[i].off == AUD_PLL_CON0 && (aud_saved[i] & BIT(31))) {
			int j;

			for (j = 0; j < 1000 && !(readl(r) & BIT(29)); j++)
				udelay(10);
			if (!(readl(r) & BIT(29)))
				pr_warn("snd-zrn-abox: PLL_AUD did not lock\n");
		}
	}
	crumb(13);
	aud_cycles++;
	gear_applied_khz = 0;	/* the APM level is set again (dsp_boot) */
on:
	spin_lock_irqsave(&aud_lock, flags);
	aud_off = false;
	spin_unlock_irqrestore(&aud_lock, flags);
	return 0;
}

static void cmu_save(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cmu_saved_regs); i++)
		cmu_saved[i] = readl(cmu + cmu_saved_regs[i]);
}

/* After a sleep: say which of them changed, and touch nothing. Writing them
 * back (PLL_AUD's control among them, while it ran) hung the phone on every
 * resume (2026-10-25, 19:16 and 19:55): only looking, until the cause is known. */
static void cmu_compare(void)
{
	int i, changed = 0;

	for (i = 0; i < ARRAY_SIZE(cmu_saved_regs); i++) {
		u32 reg = cmu_saved_regs[i], now = readl(cmu + reg);

		if (now == cmu_saved[i])
			continue;
		changed++;
		pr_info("snd-zrn-abox: clock %#06x was %08x at boot, %08x after sleep\n", reg, cmu_saved[i], now);
	}
	if (!changed)
		pr_info("snd-zrn-abox: audio clocks after sleep as at boot\n");
}

/*
 * SIFS0's pace, the speaker path's samples out of the mixer, as Samsung's
 * abox_sifsx_cnt_val() works it out: the bus clock (PLL_AUD / DIV_CLK_AUD_BUS)
 * times 2 * (32 / 16 bits), over 2 channels, over the rate, less 7. Left at 0
 * the path ran unpaced and the rate converter locked onto whatever it saw: at
 * random, at a stream's start, it took its samples 2.8 times too fast (134,773
 * frames a second for 48,000) and dropped two in three, until the audio block
 * next lost power; restarting the DSP did not help (2026-10-25). Set at every
 * DSP start and every stream start: the block forgets it when it powers down.
 */
/* Samsung's abox_flush_mixp: SIFS0's mixer emptied before its path powers up
 * and after it powers down (DAPM PRE_PMU | POST_PMD), so no play session
 * starts on what the last one left in it. Never done here before 2026-10-26. */
#define ABOX_SPUS_CTRL2		0x0208
static void mixp_flush(void)
{
	writel(readl(sfr + ABOX_SPUS_CTRL2) | BIT(0), sfr + ABOX_SPUS_CTRL2);
}
static unsigned int mixp_flushes;
module_param(mixp_flushes, uint, 0444);

static void sifs_pace(void)
{
	u32 aclk = AUD_PLL_HZ / ((readl(cmu + CMU_DIV_BUS) & 0xf) + 1);
	u32 cnt = (u32)((u64)aclk * 4 / 2 / RATE) - 5 - 2;

	writel((readl(sfr + ABOX_SPUS_SIFS_CNT0) & ~0xffffU) | (cnt & 0xffff), sfr + ABOX_SPUS_SIFS_CNT0);
}

static void cmu_div(u32 reg, u32 ratio)
{
	int i;

	writel((readl(cmu + reg) & ~0x1ffU) | ratio, cmu + reg);
	for (i = 0; i < 1000 && (readl(cmu + reg) & BIT(16)); i++)
		udelay(10);
}

/* ---- the amplifiers ------------------------------------------------------ */

static void amp_write(u8 addr, u16 reg, u8 val)
{
	u8 buf[3] = { reg >> 8, reg & 0xff, val };
	struct i2c_msg msg = { .addr = addr, .len = 3, .buf = buf };

	if (!amp_bus)
		amp_bus = i2c_get_adapter(AMP_BUS);
	if (amp_bus && i2c_transfer(amp_bus, &msg, 1) != 1)
		pr_warn_ratelimited("snd-zrn-abox: amplifier %02x did not take %04x\n", addr, reg);
}

static int amp_read(u8 addr, u16 reg)
{
	u8 address[2] = { reg >> 8, reg & 0xff }, value;
	struct i2c_msg msg[2] = {
		{ .addr = addr, .len = 2, .buf = address },
		{ .addr = addr, .flags = I2C_M_RD, .len = 1, .buf = &value },
	};

	if (!amp_bus || i2c_transfer(amp_bus, msg, 2) != 2)
		return -EIO;
	return value;
}

static void amp_both(u16 reg, u8 val)
{
	amp_write(AMP_L, reg, val);
	amp_write(AMP_R, reg, val);
}

/* Samsung's set-up, quietened (see the top of the file). */
/*
 * The amplifiers' current and voltage, for DSM: measured (0x3c), interleaved
 * at 24 kHz into one 16-bit slot each, left in slot 0, right in slot 1, as
 * Samsung's max98512 driver sets them (stock DT interleave_mode = <1>;
 * max98512_adc_config). Off: their outputs back to high impedance.
 */
static void amp_vi(bool on)
{
	amp_both(0x003c, on ? 0x03 : 0x00);
	amp_both(0x001f, on ? 0x20 : 0x00);
	amp_both(0x0024, on ? 0x85 : 0x88);
	amp_write(AMP_L, 0x001e, 0x00);
	amp_write(AMP_R, 0x001e, on ? 0x11 : 0x00);
	amp_write(AMP_L, 0x001a, on ? 0x01 : 0x00);
	amp_write(AMP_R, 0x001a, on ? 0x02 : 0x00);
	amp_write(AMP_L, 0x001c, on ? 0xfe : 0xff);
	amp_write(AMP_R, 0x001c, on ? 0xfd : 0xff);
}

static void amp_set(bool on)
{
	static const struct { u16 reg; u8 val; } init[] = {
		{ 0x003c, 0x00 },	/* no current/voltage measurement: nothing reads it */
		/* The measurement ADC (battery, boost output, temperature), set up
		 * as Samsung does. It has to run: both times sound was heard it
		 * had been switched on by hand, and without it there was none. */
		{ 0x0041, 0x07 }, { 0x0042, 0x00 }, { 0x0043, 0x0b },
		{ 0x0044, 0x02 }, { 0x0045, 0x02 }, { 0x0046, 0x02 },
		{ 0x0047, 0x0c }, { 0x0048, 0x0c }, { 0x0049, 0x0c },
		{ 0x001c, 0xff }, { 0x001d, 0xff },	/* their own data output: high impedance */
		{ 0x0020, 0x40 },	/* I2S, 16 bits a channel */
		{ 0x0021, 0x00 },	/* clocks come from the SoC */
		{ 0x0022, 0x02 },	/* 32 bit clocks a frame */
		{ 0x0023, 0x08 }, { 0x0024, 0x88 },	/* 48 kHz */
		{ 0x003a, 0x04 },	/* analogue gain: +12 dB (Samsung: +15; +3 is its earpiece setting) */
		{ 0x0036, 0x01 },	/* DC blocker */
		/*
		 * The boost converter that supplies the output stage. Left in its
		 * reset state (bypass) on this revision of the chip the stage has
		 * no supply at all: the chip's own ADC reads 0 for it, everything
		 * else reports fine, and nothing is heard. With bypass off it
		 * reads 0x44 at output code 0, 0x94 at 0x10 and 0xcf at 0x1c
		 * (Samsung's). Envelope tracking, which Samsung enables to lower it
		 * when the signal is small, is left off: here it took the supply
		 * straight down to the level at which nothing is heard.
		 */
		{ 0x0086, 0x05 }, { 0x0087, 0x02 }, { 0x0088, 0x01 },	/* bypass off */
		{ 0x003e, 0x18 },	/* boost output: most of the way to Samsung's 0x1c, headroom for the gain */
		{ 0x0040, 0x3e }, { 0x003f, 0x01 },
		{ 0x0084, 0x00 },	/* no envelope tracking */
		{ 0x004e, 0x10 },	/* squelch */
		/* Brown-out limiting off. Enabled with Samsung's thresholds but
		 * without the per-level current limits and gains that go with
		 * them, it sat engaged (status 5) and nothing came out. */
		{ 0x0050, 0x00 },
		{ 0x0011, 0x00 },
	};
	int i;

	mutex_lock(&amp_lock);
	if (on && !amp_on) {
		amp_both(0x0401, 0x01);		/* software reset */
		usleep_range(20000, 30000);
		for (i = 0; i < ARRAY_SIZE(init); i++)
			amp_both(init[i].reg, init[i].val);
		amp_write(AMP_L, 0x0018, 0x01);	/* left takes channel 0 */
		amp_write(AMP_L, 0x0025, 0x00);
		amp_write(AMP_L, 0x0026, 0x00);
		amp_write(AMP_R, 0x0018, 0x02);	/* right takes channel 1 */
		amp_write(AMP_R, 0x0025, 0x41);
		amp_write(AMP_R, 0x0026, 0x01);
		/* Samsung's gains (stock DT max98512: spk-gain 5, spk-gain-rcv 1,
		 * digital-gain-rcv 0x34): the earpiece (left, 0x39) at +3 dB
		 * analogue and -3 dB digital, fixed; the bottom speaker carries the
		 * volume. Both at the bottom speaker's gain overdrove the earpiece. */
		amp_write(AMP_L, 0x003a, EAR_GAIN);
		amp_write(AMP_L, 0x0035, EAR_DIGITAL);
		amp_write(AMP_R, 0x0035, amp_volume);
		if (dsm_on)
			amp_vi(true);
		amp_both(0x0038, 0x81);		/* amplifier on */
		amp_both(0x0400, 0x01);		/* out of shutdown; it waits for the clocks */
	} else if (!on && amp_on) {
		amp_both(0x0400, 0x00);
		amp_both(0x0038, 0x80);
	}
	amp_on = on;
	mutex_unlock(&amp_lock);
}

/*
 * A call: the voice firmware's sound goes out the same way as playback
 * (SIFS0 -> UAIF1 -> amplifiers). For the earpiece one amplifier is put in
 * Samsung's receiver mode (max98512.c, MAX98512_OSM_RCV_*): both PCM
 * channels mixed, +3 dB analogue gain, -3 dB digital, the other amplifier off.
 */
static void amp_call(int route)
{
	int other = rcv_amp == AMP_L ? AMP_R : AMP_L;

	amp_set(true);
	mutex_lock(&amp_lock);
	if (route == 1) {
		amp_write(other, 0x0400, 0x00);
		amp_write(other, 0x0038, 0x80);
		amp_write(rcv_amp, 0x0018, 0x03);
		amp_write(rcv_amp, 0x0025, 0x80);
		amp_write(rcv_amp, 0x0026, 0x01);
		amp_write(rcv_amp, 0x003a, 0x01);
		amp_write(rcv_amp, 0x0035, 0x34);
		amp_write(rcv_amp, 0x0038, 0x41);
		amp_write(rcv_amp, 0x0400, 0x01);
	}
	mutex_unlock(&amp_lock);
}

static int call_msg(u32 type, u32 param)
{
	u32 m[6] = { IPC_SYSTEM, 0, type, param, 0, 0 };

	return ipc_send(m, 6);
}

static int call_set(int route)
{
	int ret = 0;

	if (route == call_route)
		return 0;
	if (route) {
		if (!vss_started && cp_vss) {
			ret = call_msg(ABOX_START_VSS, 0);
			vss_started = !ret;
			msleep(50);
		}
		call_msg(ABOX_SET_TYPE, route == 2 ? SOUND_TYPE_SPEAKER : SOUND_TYPE_VOICE);
		ret = call_msg(ABOX_SET_MODE, MODE_IN_CALL);
		if (!call_route) {
			memset(voice_rx, 0, VOICE_BYTES);
			memset(voice_tx, 0, VOICE_BYTES);
			voice_stream(IPC_PCMPLAYBACK, VOICE_RDMA, IOVA_VOICE_RX, true);
			voice_stream(IPC_PCMCAPTURE, VOICE_WDMA, IOVA_VOICE_TX, true);
		}
		if (call_route)
			amp_set(false);	/* from one route to the other: start the amps over */
		amp_call(route);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(0), sfr + ABOX_UAIF1_CTRL0);
	} else {
		voice_stream(IPC_PCMCAPTURE, VOICE_WDMA, IOVA_VOICE_TX, false);
		voice_stream(IPC_PCMPLAYBACK, VOICE_RDMA, IOVA_VOICE_RX, false);
		ret = call_msg(ABOX_SET_MODE, MODE_NORMAL);
		amp_set(false);
		if (any_active())
			amp_set(true);	/* playback was running: back to both speakers */
	}
	pr_info("snd-zrn-abox: call audio %s (%d)\n", route == 1 ? "earpiece" : route == 2 ? "speaker" : "off", ret);
	call_route = route;
	return ret;
}

static void sfr_field(u32 reg, int shift, u32 mask, u32 val)
{
	writel((readl(sfr + reg) & ~(mask << shift)) | (val << shift), sfr + reg);
}

/* one DSM parameter block for a speaker (ch 0 left, 1 right): the
 * calibration only, as Samsung's maxdsm_set_rdc_temp_ch (platform C) */
static int dsm_cal(int ch)
{
	static u32 m[6 + DSM_PARAMS];

	memset(m, 0, sizeof(m));
	m[0] = IPC_ERAP;
	m[2] = REALTIME_EXTRA;
	m[3] = 1;			/* write */
	m[4] = ch * DSM_PARAMS;		/* where */
	m[5] = DSM_PARAMS;
	m[6 + DSM_WRITE_FLAG] = DSM_WRITE_RDC_CAL_ONLY;
	m[6 + DSM_RDC_AT_ROOMTEMP] = (u32)dsm_rdc[ch] << 8;
	m[6 + DSM_COLDTEMP] = (u32)(dsm_temp / 10) << 19;
	return ipc_send(m, ARRAY_SIZE(m));
}

static int dsm_read(int offset, int count)
{
	u32 m[6] = { IPC_ERAP, 0, REALTIME_EXTRA, 0, offset, count };

	return ipc_send(m, 6);
}

static int dsm_set(bool on)
{
	u32 erap[6] = { IPC_ERAP, 0, on ? REALTIME_OPEN : REALTIME_CLOSE, ERAP_VI_SENSE, 0, on };
	int ret;

	if (on == dsm_on)
		return 0;
	if (on && (!dsm_rdc[0] || !dsm_rdc[1] || !dsm_temp)) {
		pr_warn("snd-zrn-abox: no speaker calibration: DSM stays off\n");
		return -EINVAL;
	}
	if (on) {
		dsm_on = true;
		amp_set(false);
		amp_set(true);			/* again, with the current and voltage out */
		memset(dsm_out, 0, VOICE_BYTES);
		memset(dsm_vi, 0, VOICE_BYTES);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(1), sfr + ABOX_UAIF1_CTRL0);	/* UAIF1 in */
		sfr_field(ABOX_ROUTE_CTRL1, 12, 0xf, 9);	/* NSRC3 <- UAIF1 */
		sfr_field(ABOX_SPUM_CTRL0, 19, 0x1, 0);		/* SIFM3 -> WDMA */
		sfr_field(ABOX_ROUTE_CTRL2, 0, 0xf, 1);		/* RSRC0 <- SIFS0 */
		sfr_field(ABOX_SPUM_CTRL1, 0, 0x1, 1);		/* RECP PIFS0 */
		sfr_field(ABOX_SPUS_CTRL1, 16, 0x7, 7);		/* SIFS1 <- SPUS OUT7 */
		/* as the stock HAL: DSM on (the mixer path), then its streams */
		ret = ipc_send(erap, 6);
		msleep(20);
		voice_stream(IPC_PCMCAPTURE, DSM_WDMA, IOVA_DSM_VI, true);
		voice_stream(IPC_PCMPLAYBACK, DSM_RDMA, IOVA_DSM_OUT, true);
		msleep(20);
		dsm_cal(0);
		dsm_cal(1);
		sfr_field(ABOX_ROUTE_CTRL0, 4, 0xf, 2);		/* UAIF1 speaker <- SIFS1 */
	} else {
		sfr_field(ABOX_ROUTE_CTRL0, 4, 0xf, 1);		/* UAIF1 speaker <- SIFS0 */
		ret = ipc_send(erap, 6);
		voice_stream(IPC_PCMPLAYBACK, DSM_RDMA, IOVA_DSM_OUT, false);
		voice_stream(IPC_PCMCAPTURE, DSM_WDMA, IOVA_DSM_VI, false);
		sfr_field(ABOX_SPUM_CTRL1, 0, 0x1, 0);
		sfr_field(ABOX_ROUTE_CTRL2, 0, 0xf, 0);
		sfr_field(ABOX_ROUTE_CTRL1, 12, 0xf, 0);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) & ~BIT(1), sfr + ABOX_UAIF1_CTRL0);
		dsm_on = false;
		amp_vi(false);
	}
	pr_info("snd-zrn-abox: speaker DSM %s (%d)\n", on ? "on" : "off", ret);
	return ret;
}

static struct gpiod_lookup_table amp_gpios = {
	.dev_id = "i2c-gpio.98",
	.table = {
		/* data on gpp4-5, clock on gpp4-4: found by trying */
		GPIO_LOOKUP_IDX("gpp4", 5, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		GPIO_LOOKUP_IDX("gpp4", 4, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
		{ }
	},
};
static struct i2c_gpio_platform_data amp_i2c = { .udelay = 5 };	/* about 100 kHz */

/* ---- ALSA ----------------------------------------------------------------- */

static const struct snd_pcm_hardware abox_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = RATE, .rate_max = RATE,
	.channels_min = 2, .channels_max = 2,
	.buffer_bytes_max = BUFFER_BYTES,
	.period_bytes_min = SZ_1K, .period_bytes_max = BUFFER_BYTES / 2,
	.periods_min = 2, .periods_max = 64,
};

static struct stream *stream_of(struct snd_pcm_substream *s)
{
	return &streams[s->pcm->device];
}

static int st_msg(struct stream *st, u32 type, u32 a, u32 b, u32 c)
{
	return dma_msg(IPC_PCMPLAYBACK, st->rdma, type, a, b, c);
}

static int abox_pcm_open(struct snd_pcm_substream *s)
{
	int ret;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	s->runtime->hw = abox_hw;
	/* the DSP takes the period size in bytes and its ring in whole periods */
	snd_pcm_hw_constraint_integer(s->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(s->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	ret = st_msg(stream_of(s), PCM_PLTDAI_OPEN, 0, 0, 0);
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int abox_pcm_close(struct snd_pcm_substream *s)
{
	struct stream *st = stream_of(s);

	mutex_lock(&stream_lock);
	WRITE_ONCE(st->active, NULL);
	synchronize_rcu();
	if (!call_route && !any_active())
		amp_set(false);
	st_msg(st, PCM_PLTDAI_CLOSE, 0, 0, 0);
	/* the speaker side off until the next start, as Samsung's
	 * abox_uaif_shutdown: its FIFO starts empty with the next stream */
	if (uaif_off_on_close && !call_route && !dsm_on && !any_active())
		writel(readl(sfr + ABOX_UAIF1_CTRL0) & ~BIT(0), sfr + ABOX_UAIF1_CTRL0);
	if (!call_route && !dsm_on && !any_active())
		mixp_flush();
	mutex_unlock(&stream_lock);
	return 0;
}

static int abox_pcm_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
	struct stream *st = stream_of(s);
	struct snd_pcm_runtime *rt = s->runtime;
	int ret;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	/* always the same memory: the DSP sees it at the stream's IOVA */
	rt->dma_area = st->buf;
	rt->dma_addr = st->buf_pa;
	rt->dma_bytes = params_buffer_bytes(p);
	ret = st_msg(st, PCM_SET_BUFFER, st->iova, params_period_bytes(p), params_periods(p));
	if (!ret)
		ret = st_msg(st, PCM_PLTDAI_HW_PARAMS, params_rate(p), 16, params_channels(p));
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int abox_pcm_hw_free(struct snd_pcm_substream *s)
{
	struct stream *st = stream_of(s);

	mutex_lock(&stream_lock);
	WRITE_ONCE(st->active, NULL);
	synchronize_rcu();
	st_msg(st, PCM_PLTDAI_HW_FREE, 0, 0, 0);
	s->runtime->dma_area = NULL;
	s->runtime->dma_addr = 0;
	s->runtime->dma_bytes = 0;
	mutex_unlock(&stream_lock);
	return 0;
}

static int abox_pcm_prepare(struct snd_pcm_substream *s)
{
	struct stream *st = stream_of(s);
	int ret = 0;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	if (st->needs_reset) {
		/* A lost DMA or a new firmware instance has no usable PCM state. */
		ret = st_msg(st, PCM_PLTDAI_CLOSE, 0, 0, 0);
		if (!ret)
			ret = st_msg(st, PCM_PLTDAI_OPEN, 0, 0, 0);
		if (!ret)
			ret = st_msg(st, PCM_SET_BUFFER, st->iova,
				frames_to_bytes(s->runtime, s->runtime->period_size), s->runtime->periods);
		if (!ret)
			ret = st_msg(st, PCM_PLTDAI_HW_PARAMS, RATE, 16, 2);
		if (ret)
			goto out;
	}
	if (tell_on_prepare)
		gear_tell(NULL);
	memset(st->buf, 0, BUFFER_BYTES);
	st->last_pos = 0;
	st->stalled = 0;
	st->rate_since = 0;
	if (!call_route && !dsm_on && !READ_ONCE(streams[!st->rdma].active)) {
		mixp_flush();
		mixp_flushes++;
	}
	if (!call_route)
		amp_set(true);
	if (!asrc_off)
		sifs_pace();
	ret = st_msg(st, PCM_PLTDAI_PREPARE, 0, 0, 0);
	if (!ret) {
		st->needs_reset = false;
		WRITE_ONCE(st->active, s);
	}
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int abox_pcm_trigger(struct snd_pcm_substream *s, int cmd)
{
	struct stream *st = stream_of(s);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (suspending || !booted || st->needs_reset)
			return -EPIPE;
		st->watch_progress = jiffies;
		WRITE_ONCE(st->irq_progress, jiffies);
		st->watch_status = 0;
		WRITE_ONCE(st->running, true);
		writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(0), sfr + ABOX_UAIF1_CTRL0);
		{
			int ret = st_msg(st, PCM_PLTDAI_TRIGGER, 1, 0, 0);

			if (ret)
				WRITE_ONCE(st->running, false);
			return ret;
		}
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(st->running, false);
		return st_msg(st, PCM_PLTDAI_TRIGGER, 0, 0, 0);
	}
	return -EINVAL;
}

/* Where the DSP is reading. Its status register gives the start of the period
 * it is in and how far into that period it has got, both in units of 16
 * bytes. (Samsung's abox_rdma_pointer() takes the second as bytes, which
 * leaves the position crawling for a period and then jumping; PipeWire, which
 * schedules by the position rather than by period interrupts, then misses
 * every deadline. Measured here: the second field never exceeds 0x100 with
 * 4096-byte periods.) */
/*
 * The position is only ever allowed forward. A read that looks behind the
 * last one by less than half the ring (the DSP between periods, or not yet
 * "in progress" for a moment) would make ALSA see the stream running
 * backwards and call it an underrun: the last position is kept instead, and
 * counted (parameters pointer_stalls, pointer_backsteps).
 */
static snd_pcm_uframes_t abox_pcm_pointer(struct snd_pcm_substream *s)
{
	struct stream *st = stream_of(s);
	size_t buffer = snd_pcm_lib_buffer_bytes(s);
	unsigned long flags;
	size_t pos;
	u32 status;

	spin_lock_irqsave(&aud_lock, flags);
	if (aud_off) {
		spin_unlock_irqrestore(&aud_lock, flags);
		return bytes_to_frames(s->runtime, st->last_pos);
	}
	status = readl(sfr + ABOX_RDMA0 + 0x100 * st->rdma + RDMA_STATUS);
	spin_unlock_irqrestore(&aud_lock, flags);

	if (!buffer)
		return 0;
	if (!(status & BIT(31))) {
		pointer_stalls++;
		/* a moment between periods, or at the start; for good (the DSP
		 * stopped the DMA underneath), an underrun, so that ALSA starts
		 * the stream again rather than leave the speakers looping */
		if (++st->stalled > 50)
			return SNDRV_PCM_POS_XRUN;
		return bytes_to_frames(s->runtime, st->last_pos);
	}
	st->stalled = 0;
	/*
	 * As Samsung's abox_rdma_pointer(): RBUF_OFFSET (bits 28:16, in 16-byte
	 * units) is where the current period starts, though the DMA may have
	 * other bits of the offset set: it is brought down to a period boundary.
	 * RBUF_CNT (bits 12:0) is the bytes read in that period, NOT in 16-byte
	 * units. Shifting the count too put the pointer up to 16 times ahead within
	 * a period, whenever the firmware filled the count in: ALSA and the sound
	 * server took audio for played that was not, wrote over it, and two thirds
	 * of it never sounded; measured, the stream ran 2.8 times too fast (134,773
	 * frames a second), at random from one stream start to the next
	 * (2026-10-25).
	 */
	{
		size_t period = snd_pcm_lib_period_bytes(s);
		ssize_t offset = (ssize_t)((status >> 16) & 0x1fff) << 4;
		ssize_t count = status & 0x1fff;
		ssize_t below = buffer;

		while (period && (offset % period) && below >= 0) {
			below -= period;
			if ((below & offset) == offset)
				offset = below;
		}
		pos = (size_t)(offset + count) % buffer;
	}
	if (pos < st->last_pos && st->last_pos - pos < buffer / 2) {
		pointer_backsteps++;
		return bytes_to_frames(s->runtime, st->last_pos);
	}
	st->last_pos = pos;
	return bytes_to_frames(s->runtime, pos);
}

/* Independent of period IRQs and userspace pointer reads. 113 ms avoids
 * sampling the usual power-of-two rings at exactly the same phase. */
static void abox_watchdog(struct work_struct *work)
{
	static unsigned long amp_checked;
	bool amp_fault = false;
	int i;

	mutex_lock(&stream_lock);
	if (!suspending && READ_ONCE(dsp_fault) && !call_route && !dsm_on)
		dsp_recover();
	if (!suspending && booted) {
		if (!call_route && amp_on &&
		    (READ_ONCE(streams[0].running) || READ_ONCE(streams[1].running)) &&
		    time_after(jiffies, amp_checked + HZ)) {
			int left, right;

			mutex_lock(&amp_lock);
			left = amp_read(AMP_L, 0x0400);
			right = amp_read(AMP_R, 0x0400);
			mutex_unlock(&amp_lock);
			amp_fault = left < 0 || right < 0 || !(left & right & 1) ||
				(readl(sfr + ABOX_UAIF1_CTRL0) & 5) != 5;
			amp_checked = jiffies;
			if (amp_fault) {
				pr_warn("snd-zrn-abox: speaker path lost (amps %d/%d): reinitialising\n", left, right);
				amp_set(false);
				amp_set(true);
				writel(readl(sfr + ABOX_UAIF1_CTRL0) | 5, sfr + ABOX_UAIF1_CTRL0);
				watchdog_recoveries++;
			}
		}
		for (i = 0; i < ARRAY_SIZE(streams); i++) {
			struct stream *st = &streams[i];
			struct snd_pcm_substream *s = st->active;
			unsigned long flags;
			u32 status;

			if (!s)
				continue;
			snd_pcm_stream_lock_irqsave(s, flags);
			if (!st->running) {
				snd_pcm_stream_unlock_irqrestore(s, flags);
				continue;
			}
			status = readl(sfr + ABOX_RDMA0 + 0x100 * i + RDMA_STATUS);
			if ((status & BIT(31)) && status != st->watch_status) {
				st->watch_status = status;
				st->watch_progress = jiffies;
			}
			/* A DMA can keep looping while its period IRQs have stopped. */
			if (time_after(jiffies, st->watch_progress + msecs_to_jiffies(500)) ||
			    time_after(jiffies, READ_ONCE(st->irq_progress) + msecs_to_jiffies(500))) {
				st->needs_reset = true;
				watchdog_recoveries++;
				pr_warn("snd-zrn-abox: RDMA%d stalled (%08x): XRUN, rebuild on prepare\n", i, status);
				snd_pcm_stop(s, SNDRV_PCM_STATE_XRUN);
			} else {
				/*
				 * Its real rate, over 0.4 s at a time. One stream start in three or
				 * so, the firmware takes its samples 2.8 times too fast (134,773
				 * frames a second for 48,000) and drops two in three: broken,
				 * choppy sound until the stream ends (2026-10-25; the cause is not
				 * found yet), and stays so for every stream after. The DSP is
				 * then started afresh; the sound server starts the stream over.
				 */
				snd_pcm_uframes_t ptr = s->runtime->status->hw_ptr;
				u64 now = ktime_get_ns();

				if (!st->rate_since) {
					st->rate_since = now;
					st->rate_from = ptr;
				} else if (now - st->rate_since >= 400 * NSEC_PER_MSEC) {
					u64 frames = (u64)(ptr - st->rate_from) * NSEC_PER_SEC;
					u64 rate = div64_u64(frames, now - st->rate_since);

					st->rate_since = now;
					st->rate_from = ptr;
					if (rate > RATE * 3 / 2) {
						/* rebuilding the stream did not help (64 rebuilds in
						 * a row, all fast): the whole firmware is in it.
						 * Start the DSP afresh, as at boot (dsp_recover:
						 * at most once in 5 s). */
						static unsigned long logged;

						fast_rebuilds++;
						if (!logged || time_after(jiffies, logged + 60 * HZ)) {
							logged = jiffies;
							pr_warn("snd-zrn-abox: RDMA%d runs at %llu frames a second\n", i, rate);
						}
						if (rate_guard)
							WRITE_ONCE(dsp_fault, true);
					}
				}
			}
			snd_pcm_stream_unlock_irqrestore(s, flags);
		}
	}
	mutex_unlock(&stream_lock);
	schedule_delayed_work(&watchdog_work, msecs_to_jiffies(113));
}

static int abox_pcm_mmap(struct snd_pcm_substream *s, struct vm_area_struct *vma)
{
	struct stream *st = stream_of(s);

	return dma_mmap_coherent(&pdev->dev, vma, st->buf, st->buf_pa, BUFFER_BYTES);
}

static const struct snd_pcm_ops abox_pcm_ops = {
	.open = abox_pcm_open,
	.close = abox_pcm_close,
	.hw_params = abox_pcm_hw_params,
	.hw_free = abox_pcm_hw_free,
	.prepare = abox_pcm_prepare,
	.trigger = abox_pcm_trigger,
	.pointer = abox_pcm_pointer,
	.mmap = abox_pcm_mmap,
};

/* ---- capture: the main microphones (capture=1 only) ------------------------ */

/*
 * ABOX side. Registers from Samsung's abox.h / abox_if.c / cmucal-sfr.c
 * (exynos9810), routes from the stock mixer_paths.xml "route-ap-record":
 * ABOX NSRC0 = UAIF0, ABOX SIFM0 = WDMA, read by pcm device 9 (WDMA1).
 */
#define ABOX_QCHANNEL_DISABLE	0x0038	/* bit 2: UAIF0's bit clock never gated */
#define ABOX_UAIF0_CTRL0	0x0500
#define ABOX_UAIF0_CTRL1	0x0504
#define ABOX_WDMA0		0x2000
#define WDMA_STATUS		0x30
#define CMU_MUX_UAIF0		0x1004
#define CMU_DIV_UAIF0		0x1824
#define GPB0_CON		0x0000	/* gpb0-1..4: aud-i2s0-bus, function 2 */
#define IOVA_WDMA(x)		(0x92000000UL + SZ_1M * (x))	/* Samsung's IOVA_WDMA_BUFFER */
#define SET_INMUX0_SAMPLE_RATE	5
#define SET_INMUX0_FORMAT	0x14

/*
 * Codec side: Cirrus Logic CS47L93 (madera family, silicon ID 0x6371, as
 * CS47L92) on the stock spi@10470000, chip select 0. That SPI block is not in
 * the mainline device tree, so its pins are driven by hand: SPI mode 0,
 * 32-bit address (bit 31 set to read), 16 bits of padding, 16-bit value, big
 * endian (mainline cs47l92_16bit_spi_regmap). Pin roles: Exynos USI order,
 * CLK, MOSI, MISO, CS on gpp0-4..7 (stock spi2-bus, spi2-cs-func).
 */
#define CODEC_ID		0x6371
#define CODEC_SOFTWARE_RESET	0x0000
#define CODEC_HW_REVISION	0x0001
#define CODEC_SYSTEM_CLOCK_1	0x0101
#define CODEC_SAMPLE_RATE_1	0x0102
#define CODEC_FLL1		0x0170	/* fll->base: FLL1 Control 1 is +1 */
#define CODEC_MIC_CP_1		0x0200
#define CODEC_LDO2_CONTROL_1	0x0213
#define CODEC_MIC_BIAS_CTRL_1	0x0218
#define CODEC_MIC_BIAS_CTRL_5	0x021c
#define CODEC_INPUT_ENABLES	0x0300
#define CODEC_IN3L_CONTROL	0x0320
#define CODEC_ADC_VOL_3L	0x0321
#define CODEC_DMIC3L_CONTROL	0x0322
#define CODEC_IN3R_CONTROL	0x0324
#define CODEC_ADC_VOL_3R	0x0325
#define CODEC_AIF1_BCLK_CTRL	0x0500
#define CODEC_AIF1_RATE_CTRL	0x0503
#define CODEC_AIF1_FORMAT	0x0504
#define CODEC_AIF1_RX_BCLK_RATE	0x0506
#define CODEC_AIF1_FRAME_CTRL_1	0x0507
#define CODEC_AIF1_FRAME_CTRL_2	0x0508
#define CODEC_AIF1_TX_ENABLES	0x0519
#define CODEC_AIF1TX1MIX_SRC1	0x0700
#define CODEC_AIF1TX2MIX_SRC1	0x0708
#define CODEC_GPIO5_CTRL_1	0x1708	/* GPIO5..8 = AIF1 (pinctrl-cs47l92: pins 4..7) */
#define CODEC_IRQ1_STATUS_1	0x1800
#define CODEC_IRQ1_RAW_STATUS_1	0x1880
#define  CODEC_BOOT_DONE	0x0080
#define CODEC_IRQ1_RAW_STATUS_2	0x1881
#define  CODEC_FLL1_LOCK	0x0100
#define CODEC_SRC_IN3L		0x14	/* madera_mixer_values */
#define CODEC_SRC_IN3R		0x15
#define CODEC_IN_VU		0x0200
#define CODEC_IN_MUTE		0x0100
/* S2MPS18 LDO control registers (sensors/driver/zrn_bbd_board.c ldo_reg):
 * LDO33 vdd_pmic_codec_1p8 (always on in the stock DT), LDO34 vdd_codec_1p2
 * (DCVDD; not always on) */
#define PMIC_L33CTRL		0x6a
#define PMIC_L34CTRL		0x6b
#define PMIC_LDO_ON		0xc0

/* cs47l92_reva_16_patch, as mainline madera-core applies it after boot */
static const u16 codec_patch[][2] = {
	{ 0x3A2, 0x2C29 }, { 0x3A3, 0x0E00 }, { 0x281, 0x0000 }, { 0x282, 0x0000 },
	{ 0x4EA, 0x0100 }, { 0x22B, 0x0000 }, { 0x4A0, 0x0080 }, { 0x4A1, 0x0000 },
	{ 0x4A2, 0x0000 }, { 0x180B, 0x033F }, { 0x190B, 0x033F }, { 0x442, 0x0304 },
	{ 0x34C, 0x0003 }, { 0x124, 0x0C49 }, { 0x120, 0x0345 }, { 0x120, 0x0305 },
	{ 0x4FA, 0x5064 }, { 0x1300, 0x050E }, { 0x1302, 0x0101 }, { 0x1380, 0x02E0 },
	{ 0x1381, 0xF942 }, { 0x1382, 0x04CE }, { 0x1383, 0xFF06 }, { 0x1390, 0x0304 },
	{ 0x1391, 0xF8FF }, { 0x1392, 0x04F3 }, { 0x1393, 0xFF00 }, { 0x13A0, 0x02E0 },
	{ 0x13A1, 0xF942 }, { 0x13A2, 0x04CE }, { 0x13A3, 0xFF06 }, { 0x13B0, 0x0304 },
	{ 0x13B1, 0xF8FF }, { 0x13B2, 0x04F3 }, { 0x13B3, 0xFF00 }, { 0x412, 0x0005 },
	{ 0x41A, 0x0005 }, { 0x422, 0x0005 },
};

static struct gpiod_lookup_table codec_gpios = {
	.dev_id = "snd-zrn-abox",
	.table = {
		GPIO_LOOKUP_IDX("gpp0", 4, "codec-spi", 0, GPIO_ACTIVE_HIGH),	/* CLK */
		GPIO_LOOKUP_IDX("gpp0", 5, "codec-spi", 1, GPIO_ACTIVE_HIGH),	/* MOSI */
		GPIO_LOOKUP_IDX("gpp0", 6, "codec-spi", 2, GPIO_ACTIVE_HIGH),	/* MISO */
		GPIO_LOOKUP_IDX("gpp0", 7, "codec-spi", 3, GPIO_ACTIVE_HIGH),	/* CS, low active */
		GPIO_LOOKUP_IDX("gpg1", 0, "codec-reset", 0, GPIO_ACTIVE_HIGH),	/* low = reset */
		{ }
	},
};
static bool codec_gpios_added;
static struct gpio_desc *spi_clk, *spi_mosi, *spi_miso, *spi_cs, *codec_rst;
static DEFINE_MUTEX(codec_lock);
static unsigned int codec_errors;
module_param(codec_errors, uint, 0444);

int s9p_acpm_pmic_read(u8 reg, u8 *val);
int s9p_acpm_pmic_update(u8 reg, u8 val, u8 mask);

/* codec_lock held. Mode 0, MSB first, about 250 kHz. */
static void codec_xfer(const u8 *tx, u8 *rx, int n)
{
	int i, b;

	gpiod_set_raw_value(spi_cs, 0);
	udelay(1);
	for (i = 0; i < n; i++) {
		u8 in = 0;

		for (b = 7; b >= 0; b--) {
			gpiod_set_raw_value(spi_mosi, (tx[i] >> b) & 1);
			udelay(2);
			gpiod_set_raw_value(spi_clk, 1);
			udelay(1);
			in = (in << 1) | (gpiod_get_raw_value(spi_miso) & 1);
			udelay(1);
			gpiod_set_raw_value(spi_clk, 0);
		}
		rx[i] = in;
	}
	udelay(1);
	gpiod_set_raw_value(spi_cs, 1);
	udelay(2);
}

static void codec_write(u32 reg, u16 val)
{
	u8 tx[8] = { reg >> 24, reg >> 16, reg >> 8, reg, 0, 0, val >> 8, val }, rx[8];

	codec_xfer(tx, rx, 8);
}

static u16 codec_read(u32 reg)
{
	u8 tx[8] = { (reg >> 24) | 0x80, reg >> 16, reg >> 8, reg, 0, 0, 0, 0 }, rx[8];

	codec_xfer(tx, rx, 8);
	return (rx[6] << 8) | rx[7];
}

static void codec_update(u32 reg, u16 mask, u16 val)
{
	codec_write(reg, (codec_read(reg) & ~mask) | (val & mask));
}

/* LDO34 (the codec's 1.2 V DCVDD) on, through s9p-acpm if it is loaded; no
 * link dependency on it, so playback never needs that module. */
static void codec_supplies(void)
{
	int (*rd)(u8, u8 *) = symbol_get(s9p_acpm_pmic_read);
	int (*upd)(u8, u8, u8) = symbol_get(s9p_acpm_pmic_update);
	u8 l33 = 0, l34 = 0;

	if (!rd || !upd) {
		pr_warn("snd-zrn-abox: s9p-acpm not loaded: codec supplies (LDO33/34) as the bootloader left them\n");
		goto out;
	}
	rd(PMIC_L33CTRL, &l33);
	rd(PMIC_L34CTRL, &l34);
	pr_info("snd-zrn-abox: codec supplies: LDO33 %02x, LDO34 %02x\n", l33, l34);
	if ((l34 & PMIC_LDO_ON) != PMIC_LDO_ON) {
		upd(PMIC_L34CTRL, PMIC_LDO_ON, PMIC_LDO_ON);
		msleep(10);	/* stock regulator-enable-ramp-delay 5 ms */
		rd(PMIC_L34CTRL, &l34);
		pr_info("snd-zrn-abox: LDO34 switched on (%02x)\n", l34);
	}
out:
	if (rd)
		symbol_put(s9p_acpm_pmic_read);
	if (upd)
		symbol_put(s9p_acpm_pmic_update);
}

static int codec_get_gpios(void)
{
	struct device *dev = &pdev->dev;
	struct gpio_desc **d[4] = { &spi_clk, &spi_mosi, &spi_miso, &spi_cs };
	int i;

	if (codec_rst)
		return 0;
	if (!codec_gpios_added) {
		gpiod_add_lookup_table(&codec_gpios);
		codec_gpios_added = true;
	}
	for (i = 0; i < 4; i++) {
		*d[i] = gpiod_get_index(dev, "codec-spi", i,
			i == 2 ? GPIOD_IN : i == 3 ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW);
		if (IS_ERR(*d[i])) {
			int ret = PTR_ERR(*d[i]);

			*d[i] = NULL;
			pr_err("snd-zrn-abox: codec SPI pin %d: %d\n", i, ret);
			return ret;
		}
	}
	codec_rst = gpiod_get(dev, "codec-reset", GPIOD_OUT_LOW);	/* in reset */
	if (IS_ERR(codec_rst)) {
		int ret = PTR_ERR(codec_rst);

		codec_rst = NULL;
		pr_err("snd-zrn-abox: codec reset pin: %d\n", ret);
		return ret;
	}
	return 0;
}

static void codec_put_gpios(void)
{
	struct gpio_desc **d[5] = { &spi_clk, &spi_mosi, &spi_miso, &spi_cs, &codec_rst };
	int i;

	for (i = 0; i < 5; i++) {
		if (*d[i])
			gpiod_put(*d[i]);
		*d[i] = NULL;
	}
	if (codec_gpios_added)
		gpiod_remove_lookup_table(&codec_gpios);
	codec_gpios_added = false;
}

/* Power, reset, boot, identify, patch: once. As mainline madera_dev_init. */
static int codec_power_up(void)
{
	u16 id, rev, raw = 0;
	int i, ret;

	if (cap.codec_up)
		return 0;
	ret = codec_get_gpios();
	if (ret)
		return ret;
	codec_supplies();
	mutex_lock(&codec_lock);
	gpiod_set_raw_value(codec_rst, 0);
	usleep_range(2000, 3000);
	gpiod_set_raw_value(codec_rst, 1);
	usleep_range(2000, 3000);
	for (i = 0; i < 50; i++) {
		raw = codec_read(CODEC_IRQ1_RAW_STATUS_1);
		if (raw != 0xffff && (raw & CODEC_BOOT_DONE))
			break;
		usleep_range(1000, 2000);
	}
	id = codec_read(CODEC_SOFTWARE_RESET);
	rev = codec_read(CODEC_HW_REVISION);
	pr_info("snd-zrn-abox: codec id %04x rev %04x, boot status %04x\n", id, rev, raw);
	if (id != CODEC_ID) {
		/* nothing more written: back into reset */
		gpiod_set_raw_value(codec_rst, 0);
		mutex_unlock(&codec_lock);
		codec_errors++;
		pr_err("snd-zrn-abox: no CS47L93 on the codec bus (id %04x): no capture\n", id);
		return -ENODEV;
	}
	codec_write(CODEC_IRQ1_STATUS_1, CODEC_BOOT_DONE);	/* ack, as madera_wait_for_boot */
	for (i = 0; i < ARRAY_SIZE(codec_patch); i++)
		codec_write(codec_patch[i][0], codec_patch[i][1]);
	/* AIF1's four pins to their audio function (GPn_FN 0, default 1 = GPIO) */
	for (i = 0; i < 4; i++)
		codec_update(CODEC_GPIO5_CTRL_1 + 2 * i, 0x03ff, 0x0000);
	mutex_unlock(&codec_lock);
	cap.codec_up = true;
	return 0;
}

/*
 * The codec's clocks from the SoC's bit clock: FLL1 referenced to AIF1BCLK
 * (1.536 MHz = 48 kHz x 16 bit x 2), out 49.152 MHz, SYSCLK 98.304 MHz. That
 * keeps the codec locked to ABOX, so no rate converter is needed in the codec.
 * (Stock does the same with FLL2 for ASYNCCLK, star_madera.c
 * star_uaif0_madera_hw_params/prepare, and runs SYSCLK from a 26 MHz MCLK1
 * that is not known to be running here.) Values from mainline
 * madera_fllhj_apply() for fin 1536000, fout 49152000: refdiv 0, fref above
 * MID_THRESH: gains 0x21f0, lockdet_thr 8, fbdiv 1, integer: N 32, theta 0,
 * lambda 1, hp 0. Sequence as madera_fllhj_enable(). codec_lock held.
 */
static int codec_fll_on(void)
{
	int i;

	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0x0004);	/* HOLD */
	codec_write(CODEC_FLL1 + 0x2, 32);			/* N, CTRL_UPD clear */
	codec_update(CODEC_FLL1 + 0x3, 0xffff, 0);		/* THETA */
	codec_update(CODEC_FLL1 + 0x4, 0xffff, 1);		/* LAMBDA */
	codec_update(CODEC_FLL1 + 0x5, 0x03ff, 1);		/* FB_DIV */
	codec_update(CODEC_FLL1 + 0x6, 0x00c0, 0);		/* REFCLK_DIV */
	codec_update(CODEC_FLL1 + 0x8, 0xffff, 0x21f0);	/* gains */
	codec_update(CODEC_FLL1 + 0xa, 0xd000, 0x1000);	/* HP 0, PHASEDET_ENA */
	codec_update(CODEC_FLL1 + 0xb, 0x001e, 8 << 1);	/* LOCKDET_THR */
	codec_update(CODEC_FLL1 + 0xd, 0x0103, 0);		/* no EFS sync, no fast VCO */
	codec_update(CODEC_FLL1 + 0x1, 0xf000, 8 << 12);	/* REFCLK_SRC AIF1BCLK */
	codec_update(CODEC_FLL1 + 0x1, 0x0001, 0x0001);	/* ENA */
	codec_update(CODEC_FLL1 + 0xb, 0x0001, 0x0001);	/* LOCKDET */
	codec_update(CODEC_FLL1 + 0x2, 0x8000, 0x8000);	/* CTRL_UPD */
	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0);		/* HOLD off: lock */
	for (i = 0; i < 30; i++) {
		if (codec_read(CODEC_IRQ1_RAW_STATUS_2) & CODEC_FLL1_LOCK)
			return 0;
		usleep_range(1000, 2000);
	}
	return -ETIMEDOUT;
}

static void codec_fll_off(void)
{
	codec_update(CODEC_FLL1 + 0xb, 0x0001, 0);		/* LOCKDET */
	codec_update(CODEC_FLL1 + 0x1, 0x0004, 0x0004);	/* HOLD */
	codec_update(CODEC_FLL1 + 0x1, 0x0001, 0);		/* ENA */
	usleep_range(1000, 2000);
	codec_update(CODEC_FLL1 + 0x2, 0x8000, 0);		/* CTRL_UPD */
}

/* the route-main-mic path on: IN3L/IN3R (DMIC1, DMIC3) -> AIF1TX1/2 */
static int codec_start(void)
{
	int ret;

	mutex_lock(&codec_lock);
	ret = codec_fll_on();
	if (ret) {
		pr_err("snd-zrn-abox: codec FLL1 did not lock on the bit clock (%04x)\n",
			codec_read(CODEC_IRQ1_RAW_STATUS_2));
		codec_fll_off();
		codec_errors++;
		goto out;
	}
	codec_write(CODEC_SAMPLE_RATE_1, 0x0003);		/* 48 kHz */
	codec_write(CODEC_SYSTEM_CLOCK_1, 0x0444);		/* 98.304 MHz, on, from FLL1 */
	/* MICVDD 3.0 V from the charge pump (stock DT micvdd), not bypassed */
	codec_update(CODEC_LDO2_CONTROL_1, 0x07e0, 0x24 << 5);
	codec_update(CODEC_MIC_CP_1, 0x0003, 0x0001);
	usleep_range(3000, 4000);
	/* MICBIAS1 1.8 V, external capacitor (stock DT MICBIAS1); 1A for DMIC1
	 * (IN3L), 1C for DMIC3 (IN3R) (stock samsung,routing) */
	codec_update(CODEC_MIC_BIAS_CTRL_1, 0x81e1, 0x8000 | (3 << 5) | 0x0001);
	codec_update(CODEC_MIC_BIAS_CTRL_5, 0x0101, 0x0101);
	/* IN3: digital, clocked at 3.072 MHz (stock "IN3 OSR"), supplied from
	 * MICBIAS1 (stock cirrus,dmic-ref), high-pass on (stock "IN3L HPF") */
	codec_update(CODEC_IN3L_CONTROL, 0x9800, 0x8800);
	codec_update(CODEC_IN3R_CONTROL, 0x8000, 0x8000);
	codec_update(CODEC_DMIC3L_CONTROL, 0x0700, 5 << 8);
	/* AIF1: slave, I2S, 16-bit words in 16-bit slots, 32 bit clocks a frame,
	 * at SYSCLK rate 1; TX1 IN3L, TX2 IN3R */
	codec_update(CODEC_AIF1_BCLK_CTRL, 0x0020, 0);
	codec_update(CODEC_AIF1_RATE_CTRL, 0xf800, 0);
	codec_update(CODEC_AIF1_FORMAT, 0x0007, 2);
	codec_write(CODEC_AIF1_RX_BCLK_RATE, 32);
	codec_write(CODEC_AIF1_FRAME_CTRL_1, (16 << 8) | 16);
	codec_write(CODEC_AIF1_FRAME_CTRL_2, (16 << 8) | 16);
	codec_write(CODEC_AIF1TX1MIX_SRC1, CODEC_SRC_IN3L);
	codec_write(CODEC_AIF1TX2MIX_SRC1, CODEC_SRC_IN3R);
	codec_update(CODEC_AIF1_TX_ENABLES, 0x0003, 0x0003);
	codec_update(CODEC_INPUT_ENABLES, 0x0030, 0x0030);
	usleep_range(1000, 3000);
	/* unmuted at 0 dB, then the volume update (madera_in_ev) */
	codec_update(CODEC_ADC_VOL_3L, 0x01ff, 0x0080);
	codec_update(CODEC_ADC_VOL_3R, 0x01ff, 0x0080);
	codec_update(CODEC_ADC_VOL_3L, CODEC_IN_VU, CODEC_IN_VU);
	codec_update(CODEC_ADC_VOL_3R, CODEC_IN_VU, CODEC_IN_VU);
	pr_info("snd-zrn-abox: codec capture on (inputs %04x, AIF1 TX %04x, sysclk %04x)\n",
		codec_read(CODEC_INPUT_ENABLES), codec_read(CODEC_AIF1_TX_ENABLES),
		codec_read(CODEC_SYSTEM_CLOCK_1));
out:
	mutex_unlock(&codec_lock);
	return ret;
}

static void codec_stop(void)
{
	mutex_lock(&codec_lock);
	codec_update(CODEC_ADC_VOL_3L, CODEC_IN_MUTE | CODEC_IN_VU, CODEC_IN_MUTE | CODEC_IN_VU);
	codec_update(CODEC_ADC_VOL_3R, CODEC_IN_MUTE | CODEC_IN_VU, CODEC_IN_MUTE | CODEC_IN_VU);
	codec_update(CODEC_INPUT_ENABLES, 0x0030, 0);
	codec_update(CODEC_AIF1_TX_ENABLES, 0x0003, 0);
	codec_write(CODEC_AIF1TX1MIX_SRC1, 0);
	codec_write(CODEC_AIF1TX2MIX_SRC1, 0);
	codec_update(CODEC_MIC_BIAS_CTRL_5, 0x0101, 0);
	codec_update(CODEC_MIC_BIAS_CTRL_1, 0x0001, 0);
	codec_update(CODEC_MIC_CP_1, 0x0003, 0x0002);		/* off, bypass: reset default */
	codec_update(CODEC_SYSTEM_CLOCK_1, 0x0040, 0);
	codec_fll_off();
	mutex_unlock(&codec_lock);
}

/* stream_lock held. UAIF0 as master at 48 kHz/16/2, NSRC0 <- UAIF0, SIFM0 ->
 * WDMA, its bit clock running, then the codec. */
static int cap_path_on(void)
{
	int ret;

	if (cap.path_on)
		return 0;
	/* PLL / 16 (DIV_AUDIF, as set at boot) / 48 = 1.536 MHz, as UAIF1 */
	cmu_div(CMU_DIV_UAIF0, 47);
	writel(readl(cmu + CMU_MUX_UAIF0) & ~1U, cmu + CMU_MUX_UAIF0);
	writel((readl(pins + GPB0_CON) & ~0xffff0U) | 0x22220, pins + GPB0_CON);
	/* as UAIF1 (dsp_boot): I2S, 16 bits in 16-bit slots, two; abox_uaif_set_fmt
	 * CBS_CFS = MODE 1 (the SoC drives the clocks) */
	writel((9 << 24) | BIT(23) | (1 << 18) | (15 << 12), sfr + ABOX_UAIF0_CTRL1);
	writel(readl(sfr + ABOX_UAIF0_CTRL0) | BIT(2), sfr + ABOX_UAIF0_CTRL0);
	sfr_field(ABOX_ROUTE_CTRL1, 0, 0xf, 8);		/* NSRC0 <- UAIF0 */
	sfr_field(ABOX_SPUM_CTRL0, 7, 0x1, 0);		/* SIFM0 -> WDMA */
	sfr_field(ABOX_SPUM_CTRL0, 4, 0x1, 1);		/* NSRC0 rate converter on (stock SPUM ASRC1) */
	/* abox_uaif_set_tristate(0): the bit clock out before the codec locks to it */
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) | BIT(2), sfr + ABOX_QCHANNEL_DISABLE);
	usleep_range(1000, 2000);
	ret = codec_start();
	if (ret) {
		writel(readl(sfr + ABOX_QCHANNEL_DISABLE) & ~BIT(2), sfr + ABOX_QCHANNEL_DISABLE);
		sfr_field(ABOX_ROUTE_CTRL1, 0, 0xf, 0);
		return ret;
	}
	cap.path_on = true;
	return 0;
}

static void cap_path_off(void)
{
	if (!cap.path_on)
		return;
	if (cap.codec_up)
		codec_stop();
	writel(readl(sfr + ABOX_UAIF0_CTRL0) & ~BIT(1), sfr + ABOX_UAIF0_CTRL0);
	writel(readl(sfr + ABOX_QCHANNEL_DISABLE) & ~BIT(2), sfr + ABOX_QCHANNEL_DISABLE);
	sfr_field(ABOX_ROUTE_CTRL1, 0, 0xf, 0);		/* NSRC0 RESERVED, stock default */
	cap.path_on = false;
}

static int cap_msg(u32 type, u32 a, u32 b, u32 c)
{
	return dma_msg(IPC_PCMCAPTURE, cap_wdma, type, a, b, c);
}

/* what the DSP's input mux 0 runs at (abox_sample_rate_put_ipc,
 * abox_sif_format_put_ipc for SIFM0: SET_INMUX0_*) */
static int cap_config(void)
{
	u32 m[6] = { IPC_ABOX_CONFIG, 0, SET_INMUX0_SAMPLE_RATE, RATE, 0, 0 };
	int ret = ipc_send(m, 6);

	if (!ret) {
		m[2] = SET_INMUX0_FORMAT; m[3] = 16; m[4] = 2;
		ret = ipc_send(m, 6);
	}
	return ret;
}

static int cap_open(struct snd_pcm_substream *s)
{
	int ret;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	ret = codec_power_up();
	if (ret)
		goto out;
	s->runtime->hw = abox_hw;
	snd_pcm_hw_constraint_integer(s->runtime, SNDRV_PCM_HW_PARAM_PERIODS);
	snd_pcm_hw_constraint_step(s->runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	ret = cap_config();
	if (!ret)
		ret = cap_msg(PCM_PLTDAI_OPEN, 0, 0, 0);
	if (!ret)
		cap.opened = true;
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int cap_close(struct snd_pcm_substream *s)
{
	mutex_lock(&stream_lock);
	WRITE_ONCE(cap.active, NULL);
	synchronize_rcu();
	if (booted)
		cap_msg(PCM_PLTDAI_CLOSE, 0, 0, 0);
	cap_path_off();
	cap.opened = false;
	mutex_unlock(&stream_lock);
	return 0;
}

static int cap_hw_params(struct snd_pcm_substream *s, struct snd_pcm_hw_params *p)
{
	struct snd_pcm_runtime *rt = s->runtime;
	int ret;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	rt->dma_area = cap.buf;
	rt->dma_addr = cap.buf_pa;
	rt->dma_bytes = params_buffer_bytes(p);
	ret = cap_msg(PCM_SET_BUFFER, IOVA_WDMA(cap_wdma), params_period_bytes(p), params_periods(p));
	if (!ret)
		ret = cap_msg(PCM_PLTDAI_HW_PARAMS, params_rate(p), 16, params_channels(p));
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int cap_hw_free(struct snd_pcm_substream *s)
{
	mutex_lock(&stream_lock);
	WRITE_ONCE(cap.active, NULL);
	synchronize_rcu();
	if (booted)
		cap_msg(PCM_PLTDAI_HW_FREE, 0, 0, 0);
	s->runtime->dma_area = NULL;
	s->runtime->dma_addr = 0;
	s->runtime->dma_bytes = 0;
	mutex_unlock(&stream_lock);
	return 0;
}

static int cap_prepare(struct snd_pcm_substream *s)
{
	struct snd_pcm_runtime *rt = s->runtime;
	int ret;

	mutex_lock(&stream_lock);
	if (suspending || !booted) {
		ret = -EIO;
		goto out;
	}
	if (cap.needs_reset) {
		/* a new firmware instance: its routes and config gone too */
		cap_path_off();
		ret = cap_config();
		if (!ret)
			ret = cap_msg(PCM_PLTDAI_CLOSE, 0, 0, 0);
		if (!ret)
			ret = cap_msg(PCM_PLTDAI_OPEN, 0, 0, 0);
		if (!ret)
			ret = cap_msg(PCM_SET_BUFFER, IOVA_WDMA(cap_wdma),
				frames_to_bytes(rt, rt->period_size), rt->periods);
		if (!ret)
			ret = cap_msg(PCM_PLTDAI_HW_PARAMS, RATE, 16, 2);
		if (ret)
			goto out;
	}
	ret = cap_path_on();
	if (ret)
		goto out;
	memset(cap.buf, 0, BUFFER_BYTES);
	cap.last_pos = 0;
	cap.stalled = 0;
	ret = cap_msg(PCM_PLTDAI_PREPARE, 0, 0, 0);
	if (!ret) {
		cap.needs_reset = false;
		WRITE_ONCE(cap.active, s);
	}
out:
	mutex_unlock(&stream_lock);
	return ret;
}

static int cap_trigger(struct snd_pcm_substream *s, int cmd)
{
	int ret;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (suspending || !booted || cap.needs_reset || !cap.path_on)
			return -EPIPE;
		WRITE_ONCE(cap.running, true);
		/* abox_uaif_trigger: the port's input side on */
		writel(readl(sfr + ABOX_UAIF0_CTRL0) | BIT(1), sfr + ABOX_UAIF0_CTRL0);
		ret = cap_msg(PCM_PLTDAI_TRIGGER, 1, 0, 0);
		if (ret)
			WRITE_ONCE(cap.running, false);
		return ret;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		WRITE_ONCE(cap.running, false);
		ret = cap_msg(PCM_PLTDAI_TRIGGER, 0, 0, 0);
		writel(readl(sfr + ABOX_UAIF0_CTRL0) & ~BIT(1), sfr + ABOX_UAIF0_CTRL0);
		return ret;
	}
	return -EINVAL;
}

/* Where the DSP has written to: as abox_pcm_pointer, from WDMA's status
 * (abox_wdma_pointer: same fields as RDMA's), only ever forward. */
static snd_pcm_uframes_t cap_pointer(struct snd_pcm_substream *s)
{
	size_t buffer = snd_pcm_lib_buffer_bytes(s);
	size_t period = snd_pcm_lib_period_bytes(s);
	unsigned long flags;
	ssize_t offset, count, below;
	size_t pos;
	u32 status;

	spin_lock_irqsave(&aud_lock, flags);
	if (aud_off) {
		spin_unlock_irqrestore(&aud_lock, flags);
		return bytes_to_frames(s->runtime, cap.last_pos);
	}
	status = readl(sfr + ABOX_WDMA0 + 0x100 * cap_wdma + WDMA_STATUS);
	spin_unlock_irqrestore(&aud_lock, flags);
	if (!buffer)
		return 0;
	if (!(status & BIT(31))) {
		if (++cap.stalled > 50)
			return SNDRV_PCM_POS_XRUN;
		return bytes_to_frames(s->runtime, cap.last_pos);
	}
	cap.stalled = 0;
	offset = (ssize_t)((status >> 16) & 0x1fff) << 4;
	count = status & 0x1fff;
	below = buffer;
	while (period && (offset % period) && below >= 0) {
		below -= period;
		if ((below & offset) == offset)
			offset = below;
	}
	pos = (size_t)(offset + count) % buffer;
	if (pos < cap.last_pos && cap.last_pos - pos < buffer / 2)
		return bytes_to_frames(s->runtime, cap.last_pos);
	cap.last_pos = pos;
	return bytes_to_frames(s->runtime, pos);
}

static int cap_mmap(struct snd_pcm_substream *s, struct vm_area_struct *vma)
{
	return dma_mmap_coherent(&pdev->dev, vma, cap.buf, cap.buf_pa, BUFFER_BYTES);
}

static const struct snd_pcm_ops cap_ops = {
	.open = cap_open,
	.close = cap_close,
	.hw_params = cap_hw_params,
	.hw_free = cap_hw_free,
	.prepare = cap_prepare,
	.trigger = cap_trigger,
	.pointer = cap_pointer,
	.mmap = cap_mmap,
};

/* debugging: write a codec register number (hex), read back "reg value" */
static u32 codec_peek_reg;
static int codec_peek_set(const char *val, const struct kernel_param *kp)
{
	return kstrtou32(val, 16, &codec_peek_reg);
}
static int codec_peek_get(char *buf, const struct kernel_param *kp)
{
	u16 v;

	mutex_lock(&codec_lock);
	if (!cap.codec_up || !codec_rst) {
		mutex_unlock(&codec_lock);
		return sysfs_emit(buf, "codec not up\n");
	}
	v = codec_read(codec_peek_reg);
	mutex_unlock(&codec_lock);
	return sysfs_emit(buf, "%04x %04x\n", codec_peek_reg, v);
}
static const struct kernel_param_ops codec_peek_ops = { .set = codec_peek_set, .get = codec_peek_get };
module_param_cb(codec_peek, &codec_peek_ops, NULL, 0644);

static int vol_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = AMP_VOL_MAX;
	return 0;
}

static int vol_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.integer.value[0] = amp_volume;
	return 0;
}

static int vol_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	int val = clamp_t(int, v->value.integer.value[0], 0, AMP_VOL_MAX);

	if (val == amp_volume)
		return 0;
	mutex_lock(&amp_lock);
	amp_volume = val;
	if (amp_on)
		amp_write(AMP_R, 0x0035, val);	/* the earpiece keeps its own */
	mutex_unlock(&amp_lock);
	return 1;
}

/* the amplifier's digital volume: 0.25 dB a step, 0x40 being 0 dB */
static const DECLARE_TLV_DB_SCALE(vol_tlv, -1600, 25, 0);

static const struct snd_kcontrol_new vol_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker Playback Volume",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.info = vol_info, .get = vol_get, .put = vol_put,
	.tlv.p = vol_tlv,
};

static const char * const call_texts[] = { "Off", "Earpiece", "Speaker" };

static int call_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	return snd_ctl_enum_info(info, 1, ARRAY_SIZE(call_texts), call_texts);
}

static int call_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.enumerated.item[0] = call_route;
	return 0;
}

static int call_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	unsigned int route = v->value.enumerated.item[0];

	if (route >= ARRAY_SIZE(call_texts))
		return -EINVAL;
	if (route == call_route)
		return 0;
	call_set(route);
	return 1;
}

static int dsm_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	return 0;
}

static int dsm_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.integer.value[0] = dsm_on;
	return 0;
}

static int dsm_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	bool on = v->value.integer.value[0];

	if (on == dsm_on)
		return 0;
	return dsm_set(on) ? -EINVAL : 1;
}

static const struct snd_kcontrol_new dsm_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Speaker DSM Switch",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = dsm_info, .get = dsm_get, .put = dsm_put,
};

/* "DSM Read": a parameter index to ask the DSP for (18: the speakers' live
 * resistance); the answer lands in the parameter dsm_rx and the log */
static int dsmr_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 2 * DSM_PARAMS - 1;
	return 0;
}

static int dsmr_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	v->value.integer.value[0] = 0;
	return 0;
}

static int dsmr_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	return dsm_read(v->value.integer.value[0], 4) ? -EIO : 1;
}

static const struct snd_kcontrol_new dsmr_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "DSM Read",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = dsmr_info, .get = dsmr_get, .put = dsmr_put,
};

static const struct snd_kcontrol_new call_control = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Voice Call",
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
	.info = call_info, .get = call_get, .put = call_put,
};

/*
 * Samsung's sound on this phone is mostly the DSP's own: the extra blobs the
 * stock DT lists (abox ext_bin@0..6: speaker management, SoundBooster's
 * parameters, the voice processing sets) go into its DRAM at fixed offsets
 * before it starts, and the effects (SoundAlive, bass, EQ, ...) are blocks of
 * integers in its SRAM at 0x2e000 (stock abox_effect, abox_effect.h): the
 * values after a block's first 16 bytes, then 1 written to its first word.
 * Missing files are skipped: the DSP runs without them as before.
 */
static const struct { const char *name; u32 offset; } ext_bins[] = {
	{ "dsm.bin", 0x502000 },
	{ "AP_AUDIO_SLSI.bin", 0x7e0000 },
	{ "APBargeIn_AUDIO_SLSI.bin", 0x7dc000 },
	{ "SoundBoosterParam.bin", 0x4fc000 },
	{ "APDV_AUDIO_SLSI.bin", 0x4d0000 },
	{ "APBiBF_AUDIO_SLSI.bin", 0x7df000 },
	{ "dsm_tune.bin", 0x601000 },
};

static void ext_load(struct device *dev)
{
	const struct firmware *fw;
	int i;

	for (i = 0; i < ARRAY_SIZE(ext_bins); i++) {
		if (firmware_request_nowarn(&fw, ext_bins[i].name, dev))
			continue;
		if (ext_bins[i].offset + fw->size <= DRAM_SIZE) {
			memcpy(dram + ext_bins[i].offset, fw->data, fw->size);
			pr_info("snd-zrn-abox: %s at DRAM %#x\n", ext_bins[i].name, ext_bins[i].offset);
		}
		release_firmware(fw);
	}
}

#define EFFECTS		0x2e000		/* in the DSP's SRAM */
struct effect { const char *name; u32 base; int count, min, max; };
static const struct effect effects[] = {
	{ "SoundAlive", 0x000, 26, 0, 100 },	/* out device, preset, 9 EQ, 3D, bass, reverb, room, clarity, volume, ... */
	{ "DHA", 0x100, 14, 0, 100 },		/* Adapt Sound */
	{ "Bass Boost", 0x400, 2, 0, 100 },	/* on, strength */
	{ "Equalizer", 0x500, 23, 0, 14000 },	/* on, preset, bands, 10 levels, 10 frequencies */
	{ "SoundBooster Rotation", 0x900, 1, 0, 3 },
	{ "Upscaler", 0xa00, 1, 0, 2 },
};

static int fx_info(struct snd_kcontrol *k, struct snd_ctl_elem_info *info)
{
	const struct effect *e = &effects[k->private_value];

	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = e->count;
	info->value.integer.min = e->min;
	info->value.integer.max = e->max;
	return 0;
}

static int fx_get(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	const struct effect *e = &effects[k->private_value];
	unsigned long flags;
	int i;

	spin_lock_irqsave(&aud_lock, flags);
	for (i = 0; i < e->count && !aud_off; i++)
		v->value.integer.value[i] = (s32)readl(sram + EFFECTS + e->base + 0x10 + i * 4);
	spin_unlock_irqrestore(&aud_lock, flags);
	return 0;
}

static int fx_put(struct snd_kcontrol *k, struct snd_ctl_elem_value *v)
{
	const struct effect *e = &effects[k->private_value];
	unsigned long flags;
	int i;

	spin_lock_irqsave(&aud_lock, flags);
	if (aud_off) {
		spin_unlock_irqrestore(&aud_lock, flags);
		return -EBUSY;
	}
	for (i = 0; i < e->count; i++)
		writel((u32)clamp_t(long, v->value.integer.value[i], e->min, e->max),
			sram + EFFECTS + e->base + 0x10 + i * 4);
	writel(1, sram + EFFECTS + e->base);	/* changed: the DSP picks it up */
	spin_unlock_irqrestore(&aud_lock, flags);
	return 1;
}

/* ---- starting and stopping ------------------------------------------------ */

static int map_irq(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, ABOX_SPI, IRQ_TYPE_LEVEL_HIGH } };
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq > 0 ? irq : -EINVAL;
}

static void teardown(void)
{
	int i;

	cancel_delayed_work_sync(&watchdog_work);
	if (card)
		snd_card_free(card);
	if (capture) {
		/* the codec back into reset; its supplies are left as found */
		if (cap.path_on && started)
			cap_path_off();
		if (codec_rst)
			gpiod_set_raw_value(codec_rst, 0);
		cap.codec_up = false;
		codec_put_gpios();
	}
	cancel_delayed_work_sync(&gear_work);
	cancel_delayed_work_sync(&gear_again);
	if (abox_irq > 0)
		free_irq(abox_irq, &abox_irq);
	if (rx_task)
		kthread_stop(rx_task);
	if (amp_bus) {
		amp_set(false);
		i2c_put_adapter(amp_bus);
	}
	if (!IS_ERR_OR_NULL(i2c_pdev)) {
		platform_device_unregister(i2c_pdev);
		gpiod_remove_lookup_table(&amp_gpios);
	}
	if (started) {
		/* the core off first, then what it was looking at */
		core_off();
		writel(0x0, sfr + ABOX_SYSPOWER_CTRL);
		writel(0x0, mmu + MMU_CTRL);
		writel(0x0, mmu + MMU_CFG);
	}
	if (dram)
		dma_free_coherent(&pdev->dev, DRAM_SIZE, dram, dram_pa);
	if (iva)
		dma_free_coherent(&pdev->dev, IVA_SIZE, iva, iva_pa);
	if (vss)
		dma_free_coherent(&pdev->dev, VSS_SIZE, vss, vss_pa);
	if (vparam)
		dma_free_coherent(&pdev->dev, VPARAM_SIZE, vparam, vparam_pa);
	for (i = 0; i < 2; i++)
		if (streams[i].buf)
			dma_free_coherent(&pdev->dev, BUFFER_BYTES, streams[i].buf, streams[i].buf_pa);
	if (voice_rx)
		dma_free_coherent(&pdev->dev, VOICE_BYTES, voice_rx, voice_rx_pa);
	if (voice_tx)
		dma_free_coherent(&pdev->dev, VOICE_BYTES, voice_tx, voice_tx_pa);
	if (dsm_out)
		dma_free_coherent(&pdev->dev, VOICE_BYTES, dsm_out, dsm_out_pa);
	if (dsm_vi)
		dma_free_coherent(&pdev->dev, VOICE_BYTES, dsm_vi, dsm_vi_pa);
	if (cap.buf)
		dma_free_coherent(&pdev->dev, BUFFER_BYTES, cap.buf, cap.buf_pa);
	cap.buf = NULL;
	if (lv1)
		dma_free_coherent(&pdev->dev, SZ_16K, lv1, lv1_pa);
	if (lv2)
		dma_free_coherent(&pdev->dev, LV2_TABLES * SZ_1K, lv2, lv2_pa);
	platform_device_unregister(pdev);
	release_firmware(fw_sram);
	release_firmware(fw_dram);
	release_firmware(fw_iva);
}

static int dsp_boot(void)
{
	u32 m[6] = { 0 };
	char v[4];
	int i, ret;

	booted = false;
	WRITE_ONCE(dsp_fault, false);
	core_off();	/* a fresh start, whatever ran before */
	/* Samsung's boot gear: the top level while the DSP starts, so what it is
	 * told below is what it runs at (released once it runs: its own
	 * requests take over) */
	gear_request(GEAR_BOOT, 1);
	gear_apply();
	writel(0x7, mmu + MMU_CTRL);			/* block */
	writel(readl(mmu + MMU_INT_STATUS), mmu + MMU_INT_CLEAR);
	writel((15 << 7) | BIT(20) | BIT(24) | BIT(2), mmu + MMU_CFG);
	writel((u32)(lv1_pa >> 12), mmu + MMU_PT_BASE);
	writel(1, mmu + MMU_FLUSH_ALL);
	writel(0x5, mmu + MMU_CTRL);			/* enable */

	/* the DSP's own interrupt controller */
	writel(0xff, gicc + 0x004);
	writel(0x3, gicd + 0x000);
	for (i = 0; i < 40; i++)
		writel(0x10101010, gicd + 0x400 + i * 4);
	writel(0x3, gicc + 0x000);
	writel(0x1, sysreg + SYSREG_MISC_CON);

	memcpy_toio(sram, fw_sram->data, fw_sram->size);
	memset_io(sram + fw_sram->size, 0, ABOX_SRAM_SIZE - fw_sram->size);
	memset(dram, 0, DRAM_SIZE);
	memcpy(dram, fw_dram->data, fw_dram->size);
	ext_load(&pdev->dev);
	memset(iva, 0, IVA_SIZE);
	memcpy(iva, fw_iva->data, fw_iva->size);
	memset(vss, 0, VSS_SIZE);
	memset(vparam, 0, VPARAM_SIZE);
	memset(streams[0].buf, 0, BUFFER_BYTES);
	memset(streams[1].buf, 0, BUFFER_BYTES);


	if (irq_quiesced) {
		enable_irq(abox_irq);
		irq_quiesced = false;
	}
	writel(0x1, sfr + ABOX_SYSPOWER_CTRL);		/* it may use its DRAM */
	pmu_update(PMU_ABOX_CPU_CFG, 0x1, 0x1);
	pmu_update(PMU_ABOX_CPU_OPTION, ABOX_CPU_ENABLE, ABOX_CPU_ENABLE);
	pmu_update(PMU_PAD_RET_ABOX, 0x10000000, 0x10000000);
	pmu_update(PMU_GPIO_MODE_ABOX, 0x1, 0x1);
	started = true;

	for (i = 0; i < 400 && !booted; i++) {
		msleep(10);
		if (i > 200)
			rx_one();	/* the interrupt is not arriving: look anyway */
	}
	ret = -ETIMEDOUT;
	if (!booted) {
		pr_err("snd-zrn-abox: the DSP did not boot (pc %08x, iommu fault %08x at %08x)\n",
			readl(sfr + ABOX_CPU_PC), readl(mmu + MMU_INT_STATUS), readl(mmu + MMU_FAULT_VA));
		return ret;
	}
	memcpy(v, &fw_version, 4);
	pr_info("snd-zrn-abox: DSP firmware %c%c%c%c running, %s\n", v[3], v[2], v[1], v[0],
		abox_irq > 0 ? "interrupt driven" : "polled");

	/* resume, then the mixer everything is summed in: 48 kHz, 16 bit, stereo */
	m[0] = IPC_SYSTEM; m[2] = ABOX_RESUME;
	ret = ipc_send(m, 6);
	if (ret)
		return ret;
	msleep(20);
	gear_tell(NULL);
	gear_request(GEAR_BOOT, UINT_MAX);	/* the boot gear released (lowest wins: this never does) */
	pr_info("snd-zrn-abox: DSP clock %u kHz (APM audio level)\n", gear_applied_khz);
	m[0] = IPC_ABOX_CONFIG; m[2] = SET_MIXER_SAMPLE_RATE; m[3] = RATE;
	ret = ipc_send(m, 6);
	if (ret)
		return ret;
	m[2] = SET_MIXER_FORMAT; m[3] = 16; m[4] = 2;
	ret = ipc_send(m, 6);
	if (ret)
		return ret;
	writel((readl(sfr + ABOX_SPUS_CTRL1) & ~0x1fU) | 9, sfr + ABOX_SPUS_CTRL1);
	/* every stream: rate converter in, out to SIFS0 (6 to SIFS2, 7 to SIFS1) */
	/* never changed while the DSP runs: switching the converter at a
	 * stream's prepare stopped the firmware taking messages */
	writel(asrc_off ? spus_ctrl0 & ~(BIT(0) | BIT(4)) : spus_ctrl0, sfr + ABOX_SPUS_CTRL0);
	if (asrc_off)
		writel(readl(sfr + ABOX_SPUS_SIFS_CNT0) & ~0xffffU, sfr + ABOX_SPUS_SIFS_CNT0);
	else
		sifs_pace();
	/* UAIF1's speaker side listens to SIFS0 */
	writel((readl(sfr + ABOX_ROUTE_CTRL0) & ~0xf0U) | (1 << 4), sfr + ABOX_ROUTE_CTRL0);
	/* bit clock: PLL 1179.648 MHz / 16 = 73.728 MHz, / 48 = 1.536 MHz */
	cmu_div(CMU_DIV_AUDIF, 15);
	cmu_div(CMU_DIV_UAIF1, 47);
	writel(readl(cmu + CMU_MUX_UAIF1) & ~1U, cmu + CMU_MUX_UAIF1);
	/* gpb1-0..3 to the I2S function */
	writel((readl(pins + GPB1_CON) & ~0xffffU) | 0x2222, pins + GPB1_CON);
	/* I2S, 16 bits in 16-bit slots, two of them; the SoC drives the clocks */
	writel((9 << 24) | BIT(23) | (1 << 18) | (15 << 12), sfr + ABOX_UAIF1_CTRL1);
	writel(readl(sfr + ABOX_UAIF1_CTRL0) | BIT(2), sfr + ABOX_UAIF1_CTRL0);

	return 0;
}

/* zrn-sleepd keeps the card and sound server alive. A firmware restart cannot
 * resume an old DMA: ALSA reports SUSPENDED and userspace must prepare again. */
static u32 saved_effects[ARRAY_SIZE(effects)][26];
static bool pm_ready;
module_param(pm_ready, bool, 0444);

static int abox_pm_event(struct notifier_block *nb, unsigned long event, void *unused)
{
	int i, j, ret;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		/* Voice/DSM restoration needs its own protocol; never silently lose it. */
		if (call_route || dsm_on)
			return notifier_from_errno(-EBUSY);
		/* the codec and UAIF0 path have no suspend/resume here yet */
		if (READ_ONCE(cap.opened))
			return notifier_from_errno(-EBUSY);
		cancel_delayed_work_sync(&watchdog_work);
		snd_power_change_state(card, SNDRV_CTL_POWER_D3hot);
		for (i = 0; i < ARRAY_SIZE(playback); i++)
			snd_pcm_suspend_all(playback[i]);
		mutex_lock(&stream_lock);
		suspending = true;
		for (i = 0; i < ARRAY_SIZE(streams); i++)
			streams[i].needs_reset = true;
		for (i = 0; i < ARRAY_SIZE(effects); i++)
			for (j = 0; j < effects[i].count; j++)
				saved_effects[i][j] = readl(sram + EFFECTS + effects[i].base + 0x10 + j * 4);
		amp_set(false);
		if (abox_irq > 0) {
			disable_irq(abox_irq);
			irq_quiesced = true;
		}
		cancel_delayed_work_sync(&gear_work);
		cancel_delayed_work_sync(&gear_again);
		core_off();
		booted = false;
		writel(0, sfr + ABOX_SYSPOWER_CTRL);
		writel(0, mmu + MMU_CTRL);
		mutex_unlock(&stream_lock);
		break;
	case PM_POST_SUSPEND:
		if (!suspending)
			break;
		mutex_lock(&stream_lock);
		cmu_compare();
		ret = dsp_boot();
		if (!ret) {
			for (i = 0; i < ARRAY_SIZE(effects); i++) {
				for (j = 0; j < effects[i].count; j++)
					writel(saved_effects[i][j], sram + EFFECTS + effects[i].base + 0x10 + j * 4);
				writel(1, sram + EFFECTS + effects[i].base);
			}
		}
		if (ret) {
			booted = false;
			WRITE_ONCE(dsp_fault, true);
		}
		pm_ready = !ret;
		suspending = false;
		snd_power_change_state(card, SNDRV_CTL_POWER_D0);
		pr_info("snd-zrn-abox: resume firmware %d; streams require prepare\n", ret);
		mutex_unlock(&stream_lock);
		schedule_delayed_work(&watchdog_work, msecs_to_jiffies(113));
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block abox_pm = { .notifier_call = abox_pm_event };

static unsigned int dsp_restarts;
module_param(dsp_restarts, uint, 0444);
/* a DSP restart goes through a power cycle of the whole block */
static bool recover_power_cycle;
module_param(recover_power_cycle, bool, 0644);
/* write 1: restart the DSP through a power cycle now (for testing) */
static int restart_now_set(const char *val, const struct kernel_param *kp)
{
	WRITE_ONCE(dsp_fault, true);
	return 0;
}
static const struct kernel_param_ops restart_now_ops = { .set = restart_now_set };
module_param_cb(restart_now, &restart_now_ops, NULL, 0200);

/* stream_lock is held. A mailbox timeout is a DSP failure, not an ordinary
 * underrun. Stop all media streams before replacing the firmware instance. */
static void dsp_recover(void)
{
	static unsigned long next_retry;
	int i, j, ret;

	if (time_before(jiffies, next_retry))
		return;
	next_retry = jiffies + 5 * HZ;
	for (i = 0; i < ARRAY_SIZE(streams); i++) {
		struct stream *st = &streams[i];

		st->needs_reset = true;
		if (st->active)
			snd_pcm_stop_xrun(st->active);
	}
	if (capture) {
		cap.needs_reset = true;
		if (cap.active)
			snd_pcm_stop_xrun(cap.active);
	}
	amp_set(false);
	if (abox_irq > 0) {
		disable_irq(abox_irq);
		irq_quiesced = true;
	}
	cancel_delayed_work_sync(&gear_work);
	cancel_delayed_work_sync(&gear_again);
	for (i = 0; i < ARRAY_SIZE(effects); i++)
		for (j = 0; j < effects[i].count; j++)
			saved_effects[i][j] = readl(sram + EFFECTS + effects[i].base + 0x10 + j * 4);
	if (recover_power_cycle) {
		/* as Samsung's abox_disable: the DSP asked to suspend and left
		 * in WFI (not held in reset: with the core powered off
		 * underneath it, the block would not power down), its DRAM
		 * request dropped, the pads held, then the domain off */
		u32 m[6] = { IPC_SYSTEM, 0, ABOX_SUSPEND, 0, 0, 0 };
		bool dma = false, wfi;
		int k;

		/* the streams stopped and closed on the DSP first: power taken
		 * from under a running DMA froze the whole phone (2026-10-26) */
		crumb(1);
		for (k = 0; booted && k < ARRAY_SIZE(streams); k++) {
			st_msg(&streams[k], PCM_PLTDAI_TRIGGER, 0, 0, 0);
			st_msg(&streams[k], PCM_PLTDAI_CLOSE, 0, 0, 0);
		}
		if (booted && capture && cap.opened) {
			cap_msg(PCM_PLTDAI_TRIGGER, 0, 0, 0);
			cap_msg(PCM_PLTDAI_CLOSE, 0, 0, 0);
		}
		for (k = 0; k < 100; k++) {
			dma = (readl(sfr + ABOX_RDMA0 + RDMA_STATUS) |
			       readl(sfr + ABOX_RDMA0 + 0x100 + RDMA_STATUS)) & BIT(31);
			if (capture && cap.opened)
				dma |= !!(readl(sfr + ABOX_WDMA0 + 0x100 * cap_wdma + WDMA_STATUS) & BIT(31));
			if (!dma)
				break;
			usleep_range(500, 1000);
		}
		crumb(2);
		if (booted)
			ipc_send(m, 6);
		crumb(3);
		for (k = 0; k < 1000 && !(readl(pmu + PMU_ABOX_CPU_STANDBY) & ABOX_CPU_WFI); k++)
			udelay(10);
		wfi = readl(pmu + PMU_ABOX_CPU_STANDBY) & ABOX_CPU_WFI;
		booted = false;
		if (wfi && !dma) {
			crumb(4);
			writel(0, sfr + ABOX_SYSPOWER_CTRL);
			writel(0, mmu + MMU_CTRL);
			writel(0, gicd);
			pmu_update(PMU_GPIO_MODE_ABOX, 0x1, 0x1);
			crumb(5);
			ret = aud_power_cycle();
			pr_info("snd-zrn-abox: audio block power cycled (%d)\n", ret);
		} else {
			/* not safe to cut the power: a DSP restart only */
			pr_warn("snd-zrn-abox: no power cycle: DSP %s, DMA %s\n",
				wfi ? "idle" : "busy", dma ? "running" : "stopped");
		}
	}
	crumb(0x20);
	ret = aud_off ? -EIO : dsp_boot();
	crumb(0x21);
	if (!ret) {
		for (i = 0; i < ARRAY_SIZE(effects); i++) {
			for (j = 0; j < effects[i].count; j++)
				writel(saved_effects[i][j], sram + EFFECTS + effects[i].base + 0x10 + j * 4);
			writel(1, sram + EFFECTS + effects[i].base);
		}
		vss_started = false;
	} else {
		booted = false;
		WRITE_ONCE(dsp_fault, true);
	}
	pm_ready = !ret;
	dsp_restarts++;
	pr_warn("snd-zrn-abox: watchdog restarted DSP (%d), streams require prepare\n", ret);
}

static int __init abox_init(void)
{
	struct snd_pcm *snd_pcm;
	struct device *dev;
	u32 *l1;
	char v[4];
	int i, ret;

	INIT_DELAYED_WORK(&gear_work, gear_tell);
	INIT_DELAYED_WORK(&gear_again, gear_tell);
	INIT_DELAYED_WORK(&watchdog_work, abox_watchdog);
	apm_init();
	pdev = platform_device_register_simple("snd-zrn-abox", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	dev = &pdev->dev;
	dma_coerce_mask_and_coherent(dev, DMA_BIT_MASK(36));

	ret = request_firmware(&fw_sram, "calliope_sram.bin", dev);
	if (!ret)
		ret = request_firmware(&fw_dram, "calliope_dram.bin", dev);
	if (!ret)
		ret = request_firmware(&fw_iva, "calliope_iva.bin", dev);
	if (ret || fw_sram->size > ABOX_SRAM_SIZE || fw_dram->size > DRAM_SIZE ||
			fw_iva->size > IVA_SIZE) {
		pr_err("snd-zrn-abox: the calliope firmware is not in /lib/firmware (%d)\n", ret);
		ret = ret ? ret : -EINVAL;
		goto err;
	}

	pmu = ioremap(PMU_BASE, 0x10000);
	sfr = ioremap(ABOX_SFR, 0x10000);
	sysreg = ioremap(ABOX_SYSREG, 0x1000);
	sram = ioremap(ABOX_SRAM, ABOX_SRAM_SIZE);
	gicd = ioremap(ABOX_GICD, 0x1000);
	gicc = ioremap(ABOX_GICC, 0x2000);
	mmu = ioremap(SYSMMU_AUD, 0x9000);
	cmu = ioremap(CMU_AUD, 0x8000);
	pins = ioremap(PINCTRL_AUD, 0x1000);
	ret = -ENOMEM;
	if (!pmu || !sfr || !sysreg || !sram || !gicd || !gicc || !mmu || !cmu || !pins)
		goto err;

	ret = -ENODEV;
	if ((readl(pmu + PMU_AUD_STATUS) & 0xf) == 0xf && cmu)
		cmu_save();	/* the bootloader's clocks, for after a sleep */
	if ((readl(pmu + PMU_AUD_STATUS) & 0xf) != 0xf || readl(sfr + ABOX_IP_ID) != 0x41424f58) {
		pr_err("snd-zrn-abox: the audio block is not powered\n");
		goto err;
	}

	/* uncached, contiguous: what the DSP reads is what was written */
	ret = -ENOMEM;
	dram = dma_alloc_coherent(dev, DRAM_SIZE, &dram_pa, GFP_KERNEL);
	iva = dma_alloc_coherent(dev, IVA_SIZE, &iva_pa, GFP_KERNEL);
	vss = dma_alloc_coherent(dev, VSS_SIZE, &vss_pa, GFP_KERNEL);
	vparam = dma_alloc_coherent(dev, VPARAM_SIZE, &vparam_pa, GFP_KERNEL);
	for (i = 0; i < 2; i++)
		streams[i].buf = dma_alloc_coherent(dev, BUFFER_BYTES, &streams[i].buf_pa, GFP_KERNEL);
	voice_rx = dma_alloc_coherent(dev, VOICE_BYTES, &voice_rx_pa, GFP_KERNEL);
	voice_tx = dma_alloc_coherent(dev, VOICE_BYTES, &voice_tx_pa, GFP_KERNEL);
	dsm_out = dma_alloc_coherent(dev, VOICE_BYTES, &dsm_out_pa, GFP_KERNEL);
	dsm_vi = dma_alloc_coherent(dev, VOICE_BYTES, &dsm_vi_pa, GFP_KERNEL);
	lv1 = dma_alloc_coherent(dev, SZ_16K, &lv1_pa, GFP_KERNEL);
	lv2 = dma_alloc_coherent(dev, LV2_TABLES * SZ_1K, &lv2_pa, GFP_KERNEL);
	if (capture && (cap_wdma < 0 || cap_wdma > 1)) {
		pr_warn("snd-zrn-abox: cap_wdma %d not 0 or 1: no capture\n", cap_wdma);
		capture = false;
	}
	if (capture) {
		cap.buf = dma_alloc_coherent(dev, BUFFER_BYTES, &cap.buf_pa, GFP_KERNEL);
		if (!cap.buf)
			goto err;
	}
	if (!dram || !iva || !vss || !vparam || !streams[0].buf || !streams[1].buf || !voice_rx || !voice_tx || !dsm_out || !dsm_vi || !lv1 || !lv2)
		goto err;

	memset(lv2, 0, LV2_TABLES * SZ_1K);
	l1 = lv1;
	for (i = 0; i < 4096; i++)
		l1[i] = (u32)(lv2_pa >> 4) | 1;	/* nothing here: the empty table */
	ret = mmu_map(IOVA_DRAM, dram_pa, DRAM_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_IVA, iva_pa, IVA_SIZE);
	if (!ret && cp_vss) {
		/* the voice firmware where the modem put it, empty memory after */
		void __iomem *magic = ioremap(CP_VSS_PA + CP_VSS_MAGIC, 4);

		if (magic) {
			writel(0, magic);
			iounmap(magic);
		}
		ret = mmu_map(IOVA_VSS, CP_VSS_PA, CP_VSS_SIZE);
		if (!ret)
			ret = mmu_map(IOVA_VSS + CP_VSS_SIZE, vss_pa, VSS_SIZE - CP_VSS_SIZE);
	} else if (!ret)
		ret = mmu_map(IOVA_VSS, vss_pa, VSS_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_VPARAM, vparam_pa, VPARAM_SIZE);
	if (!ret)
		ret = mmu_map(IOVA_RDMA0, streams[0].buf_pa, BUFFER_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_RDMA1, streams[1].buf_pa, BUFFER_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_VOICE_RX, voice_rx_pa, VOICE_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_VOICE_TX, voice_tx_pa, VOICE_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_DSM_OUT, dsm_out_pa, VOICE_BYTES);
	if (!ret)
		ret = mmu_map(IOVA_DSM_VI, dsm_vi_pa, VOICE_BYTES);
	if (!ret && capture)
		ret = mmu_map(IOVA_WDMA(cap_wdma), cap.buf_pa, BUFFER_BYTES);
	if (!ret)
		ret = mmu_map(0x10000000, 0x10000000, SZ_4K);	/* the chip ID, as Samsung's driver does */
	if (ret)
		goto err;

	/* its interrupt to us; without it, the mailbox is looked at every millisecond */
	abox_irq = map_irq();
	if (abox_irq > 0 && request_irq(abox_irq, abox_isr, 0, "snd-zrn-abox", &abox_irq))
		abox_irq = -1;
	if (abox_irq <= 0) {
		pr_warn("snd-zrn-abox: no interrupt (%d): polling\n", abox_irq);
		rx_task = kthread_run(rx_fn, NULL, "snd-zrn-abox");
		if (IS_ERR(rx_task)) {
			ret = PTR_ERR(rx_task);
			rx_task = NULL;
			goto err;
		}
	}

	ret = dsp_boot();
	if (ret)
		goto err;
	memcpy(v, &fw_version, 4);

	/* the amplifiers' bus */
	gpiod_add_lookup_table(&amp_gpios);
	i2c_pdev = platform_device_register_data(NULL, "i2c-gpio", AMP_BUS, &amp_i2c, sizeof(amp_i2c));
	if (IS_ERR(i2c_pdev)) {
		gpiod_remove_lookup_table(&amp_gpios);
		ret = PTR_ERR(i2c_pdev);
		goto err;
	}
	for (i = 0; i < 20 && !amp_bus; i++) {
		amp_bus = i2c_get_adapter(AMP_BUS);
		if (!amp_bus)
			msleep(10);
	}
	if (!amp_bus)
		pr_warn("snd-zrn-abox: no amplifier bus (is i2c-gpio loaded?): will retry on playback\n");

	ret = snd_card_new(dev, SNDRV_DEFAULT_IDX1, "ZrnAbox", THIS_MODULE, 0, &card);
	if (ret)
		goto err;
	strscpy(card->driver, "snd-zrn-abox", sizeof(card->driver));
	strscpy(card->shortname, "Galaxy S9 speakers", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname), "Galaxy S9 speakers (ABOX %c%c%c%c, MAX98512)",
		v[3], v[2], v[1], v[0]);
	ret = snd_pcm_new(card, "Speakers", 0, 1, 0, &snd_pcm);
	if (!ret) {
		playback[0] = snd_pcm;
		strscpy(snd_pcm->name, "Speakers", sizeof(snd_pcm->name));
		snd_pcm_set_ops(snd_pcm, SNDRV_PCM_STREAM_PLAYBACK, &abox_pcm_ops);
		ret = snd_ctl_add(card, snd_ctl_new1(&vol_control, NULL));
	if (!ret) {
		struct snd_pcm *android;

		ret = snd_pcm_new(card, "Android", 1, 1, 0, &android);
		if (!ret) {
			playback[1] = android;
			strscpy(android->name, "Android", sizeof(android->name));
			snd_pcm_set_ops(android, SNDRV_PCM_STREAM_PLAYBACK, &abox_pcm_ops);
		}
	}
	if (!ret && capture) {
		ret = snd_pcm_new(card, "Microphone", 2, 0, 1, &capture_pcm);
		if (!ret) {
			strscpy(capture_pcm->name, "Microphone", sizeof(capture_pcm->name));
			snd_pcm_set_ops(capture_pcm, SNDRV_PCM_STREAM_CAPTURE, &cap_ops);
		}
	}
	if (!ret)
		ret = snd_ctl_add(card, snd_ctl_new1(&call_control, NULL));
	if (!ret)
		ret = snd_ctl_add(card, snd_ctl_new1(&dsm_control, NULL));
	if (!ret)
		ret = snd_ctl_add(card, snd_ctl_new1(&dsmr_control, NULL));
	for (i = 0; !ret && i < ARRAY_SIZE(effects); i++) {
		struct snd_kcontrol_new fx = {
			.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = effects[i].name,
			.access = SNDRV_CTL_ELEM_ACCESS_READWRITE,
			.info = fx_info, .get = fx_get, .put = fx_put, .private_value = i,
		};
		ret = snd_ctl_add(card, snd_ctl_new1(&fx, NULL));
	}
	}
	if (!ret)
		ret = snd_card_register(card);
	if (ret)
		goto err;
	ret = register_pm_notifier(&abox_pm);
	if (ret)
		goto err;
	pm_ready = true;
	schedule_delayed_work(&watchdog_work, msecs_to_jiffies(113));
	return 0;

err:
	teardown();
	return ret;
}

static void __exit abox_exit(void)
{
	unregister_pm_notifier(&abox_pm);
	teardown();
}

module_init(abox_init);
module_exit(abox_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Galaxy S9 (Exynos 9810) speakers: ABOX DSP and MAX98512 amplifiers");
MODULE_FIRMWARE("calliope_sram.bin");
MODULE_FIRMWARE("calliope_dram.bin");
MODULE_FIRMWARE("calliope_iva.bin");
MODULE_SOFTDEP("pre: i2c-gpio snd-pcm");
