/* SPDX-License-Identifier: GPL-2.0 */
/*
 * zrn-cam, RAW through CSIS0 with no Samsung ISP. Included by zrn-cam.c.
 *
 *	dvfs-DOM-KHZ		ask the APM for a camera domain level, read the ISPPRE bus clock
 *	stream-count[-tp]	rear up, stream 5 s, count frames by polling, no DMA
 *	stream-frame[-tp]	the same, then one frame to memory, read from debugfs "frame"
 *
 * "-tp" is the sensor's test pattern, register 0x0601 = 2 (colour bars): nothing
 * from the room. Registers are from the vendor kernel: fimc-is-hw-csi-v5_1.[ch],
 * fimc-is-device-csi_v4.c, phy-exynos8895-mipi.c, fimc-is-cis-2l3.c and the stock DT.
 *
 * There are no interrupts: nothing is requested and the lines stay masked at the
 * GIC, so a storm cannot happen. All of it runs inside the write and is bounded.
 * The bus clock is only ever raised through the APM, which sets the voltage with
 * it; CMU registers are read here, never written.
 */
#include <linux/dma-mapping.h>
#include <linux/pm_wakeup.h>
#include "zrn-cam-2l3.h"

#define SENSOR_W	2016
#define SENSOR_H	1512
#define PDAF_H		378
#define FMT_RAW10	0x2b
#define FMT_NONE	0x3f
#define SETTLE_1352	66	/* fimc-is-hw-settle-10nm-lpp.h, 1360 */

#define CSIS_VERSION	0x000
#define CSIS_CMN	0x004
#define CSIS_MSK0	0x010
#define CSIS_SRC0	0x014
#define CSIS_MSK1	0x018
#define CSIS_SRC1	0x01c
#define CSIS_CFG(n)	(0x40 + (n) * 0x10)
#define CSIS_RES(n)	(0x44 + (n) * 0x10)
#define CSIS_FRM_CNT0	0x100
#define CSIS_PHY_STAT	0x700
#define CSIS_PHY_CMN	0x704

#define VC_CTRL		0x00
#define VC_FMT		0x04
#define VC_ADDR(n)	(0x10 + (n) * 4)
#define VC_ACT		0x30
#define VC_RESOL	0x40
#define CMN_DATA	0x400
#define CMN_MASK	0x404
#define CMN_SRC		0x408
#define CMN_ABORT	0x414
#define CMN_ERR		0x418
#define CMN_OTF		0x41c

#define FRAME_FRONT	SZ_1M
#define FRAME_AREA	(SENSOR_W * 2 * SENSOR_H)	/* observed: the DMA writes 16 bits a pixel */
#define FRAME_REAR	(2 * SZ_1M)
#define FRAME_SIZE	(FRAME_FRONT + FRAME_AREA + FRAME_REAR)
#define CANARY_FRONT	0xa5
#define CANARY_REAR	0x5a

static void __iomem *csis, *csis_vc, *csis_phy, *csis_dma, *sys_phy, *top_cmu;
static void *frame;
static dma_addr_t frame_dma;
static size_t frame_len;
static bool stop_now;
static struct wakeup_source *stream_ws;

/* --- the APM's DVFS channel, as snd-zrn-abox.c --- */
#define APM_MBOX	0x14100000
#define APM_SRAM	0x02039000
#define APM_SRAM_SIZE	0x30000
#define APM_SRAM_MAP	0x3000
#define APM_INITDATA	0x7f00
#define APM_INTGR0	0x08
#define APM_INTGR1	0x1c
#define APM_INTCR1	0x20
#define APM_INTSR1	0x28
#define APM_DVFS_CHANNEL 0
#define INTCAM_KHZ	690000	/* the top of stock's freq_info for dvfs_intcam (5) and dvfs_cam (7) */

static void __iomem *apm_mbox, *apm_sram;
static struct {
	bool ok;
	u32 id, q_len, elem, tx_rear, tx_front, tx_base, rx_rear, rx_front, rx_base;
} apm_ch;
static u32 apm_seq = 40;

static u32 apm_rd(u32 off) { return readl(apm_sram + off - APM_SRAM_MAP); }
static void apm_wr(u32 off, u32 v) { writel(v, apm_sram + off - APM_SRAM_MAP); }
static bool apm_off_ok(u32 off) { return off >= APM_SRAM_MAP && off < APM_SRAM_SIZE - 16; }

static void apm_init(void)
{
	u32 table, ap_max, i;

	if (apm_mbox)
		return;
	apm_mbox = ioremap(APM_MBOX, 0x1000);
	apm_sram = ioremap(APM_SRAM + APM_SRAM_MAP, APM_SRAM_SIZE - APM_SRAM_MAP);
	if (!apm_mbox || !apm_sram)
		return;
	table = apm_rd(APM_INITDATA + 2 * 4);
	ap_max = apm_rd(APM_INITDATA + 6 * 4);
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
}

static int apm_set(u32 domain, u32 khz)
{
	u32 seq, front, nxt, i, k, rear;
	unsigned long until;

	apm_init();
	if (!apm_ch.ok)
		return -ENODEV;
	seq = apm_seq = (apm_seq % 20) + 40;	/* apart from the audio driver's numbers */
	front = apm_rd(apm_ch.tx_front);
	nxt = (front + 1) % apm_ch.q_len;
	until = jiffies + HZ;
	while (nxt == apm_rd(apm_ch.tx_rear)) {
		if (time_after(jiffies, until))
			return -ETIMEDOUT;
		usleep_range(200, 400);
	}
	apm_wr(apm_ch.tx_base + apm_ch.elem * front, domain | (seq << 16));
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
				if (i != rear)
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
				return (w0 & 0xffff) ? -EIO : 0;
			}
		}
		usleep_range(200, 400);
	}
	return -ETIMEDOUT;
}

/* mux select and divider of the ISPPRE bus in CMU_TOP; read only */
static void bus_note(const char *when)
{
	u32 mux = readl(top_cmu + 0x1094), div = readl(top_cmu + 0x1890);

	note("%s: ISPPRE bus mux 0x%x div 0x%x", when, mux, div);
}

/* --- the sensor --- */
static int cis_write16(u16 reg, u16 val)
{
	u8 b[4] = { reg >> 8, reg, val >> 8, val };
	struct i2c_msg m = { .addr = sensor->addr, .len = 4, .buf = b };
	int ret = i2c_transfer(sensor->adapter, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int cis_write8(u16 reg, u8 val)
{
	u8 b[3] = { reg >> 8, reg, val };
	struct i2c_msg m = { .addr = sensor->addr, .len = 3, .buf = b };
	int ret = i2c_transfer(sensor->adapter, &m, 1);

	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static u8 burst_buf[2 + 2 * 2048];

/* sensor_cis_set_registers(), at 100 kHz instead of stock's 1 MHz */
static int cis_table(const u32 *t, size_t n)
{
	size_t i, count = 0;
	int ret = 0;

	usleep_range(3000, 3100);
	for (i = 0; i < n && !ret; i += 3) {
		u32 a = t[i], v = t[i + 1], w = t[i + 2];

		if (a == TB_BURST_ADDR) {
			burst_buf[0] = v >> 8;
			burst_buf[1] = v;
			count = 0;
		} else if (a == TB_BURST_DATA) {
			if (count >= 2048)
				return -E2BIG;
			burst_buf[2 + count * 2] = v >> 8;
			burst_buf[3 + count * 2] = v;
			count++;
			if (i + 3 >= n || t[i + 3] != TB_BURST_DATA) {
				struct i2c_msg m = { .addr = sensor->addr, .len = 2 + count * 2, .buf = burst_buf };

				ret = i2c_transfer(sensor->adapter, &m, 1);
				ret = ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
			}
		} else if (a == TB_DELAY) {
			usleep_range(v, v + 100);
		} else if (w == 1) {
			ret = cis_write8(a, v);
		} else {
			ret = cis_write16(a, v);
		}
	}
	if (ret)
		note("sensor table: row %zu of %zu failed: %d", i / 3, n / 3, ret);
	return ret;
}

/* --- CSIS0 and its D-PHY --- */
static void rmw(void __iomem *r, u32 mask, u32 val)
{
	writel((readl(r) & ~mask) | val, r);
}

static int csis_map(void)
{
	if (csis)
		return 0;
	csis = ioremap(0x16230000, 0x1000);
	csis_phy = ioremap(0x16160000, 0x10000);
	csis_vc = ioremap(0x162b0000, 0x1000);
	csis_dma = ioremap(0x162f0000, 0x100);
	sys_phy = ioremap(0x16210500, 4);
	return csis && csis_phy && csis_vc && csis_dma && sys_phy ? 0 : -ENOMEM;
}

static void csis_on(void)
{
	static const u32 sd[] = { 0x2800, 0x2c00, 0x3000, 0x3400 };
	unsigned int i;

	/* phy_power_on: release the PHY's reset; PMU isolation 0x70c already reads bypass */
	rmw(sys_phy, BIT(0), 0);
	rmw(sys_phy, BIT(0), BIT(0));

	rmw(csis + CSIS_CMN, BIT(1), BIT(1));
	for (i = 0; i < 10 && (readl(csis + CSIS_CMN) & BIT(1)); i++)
		udelay(10);

	/* csi_hw_s_phy_default_value, CSI_ID_A */
	writel(0x10, csis_phy + 0x404);
	writel(0x05, csis_phy + 0x40c);
	writel(0x68, csis_phy + 0x414);
	writel(0x6c, csis_phy + 0x418);
	writel(0x02, csis_phy + 0x2418);
	/* csi_hw_s_phy_config below 1.5 Gbps: no skew calibration, HS mode 1 */
	rmw(csis_phy + 0x2484, 0xff, 1);
	for (i = 0; i < 4; i++) {
		rmw(csis_phy + sd[i] + 0xac, BIT(2), BIT(2));
		rmw(csis_phy + sd[i] + 0xb0, 0xff, SETTLE_1352);
	}

	/* four lanes, VC_ONLY, as FIMC_IS_SENSOR_CFG mode 5 */
	rmw(csis + CSIS_CMN, 3 << 8, 3 << 8);
	rmw(csis + CSIS_PHY_CMN, 0xf << 1, 0xf << 1);
	rmw(csis + CSIS_CMN, 3 << 10, 2 << 10);
	writel(0 | (FMT_RAW10 << 2) | (1 << 12), csis + CSIS_CFG(0));
	writel(SENSOR_H << 16 | SENSOR_W, csis + CSIS_RES(0));
	writel(1 | (FMT_RAW10 << 2) | (2 << 12), csis + CSIS_CFG(1));
	writel(PDAF_H << 16 | SENSOR_W, csis + CSIS_RES(1));
	writel(2 | (FMT_NONE << 2) | (1 << 12), csis + CSIS_CFG(2));
	writel(0, csis + CSIS_RES(2));
	writel(3 | (FMT_NONE << 2) | (1 << 12), csis + CSIS_CFG(3));
	writel(0, csis + CSIS_RES(3));

	writel(0x0fffff1f, csis + CSIS_MSK0);
	writel(0x0000fff0, csis + CSIS_MSK1);
	writel(readl(csis + CSIS_SRC0), csis + CSIS_SRC0);
	writel(readl(csis + CSIS_SRC1), csis + CSIS_SRC1);

	/* csi_hw_s_dma_common */
	rmw(csis_dma + 0x00, 0xf, (1 << 2) | 2);
	rmw(csis_dma + 0x08, BIT(1), BIT(1));
	writel(0x140 << 16 | 0x140, csis_dma + 0x0c);
	rmw(csis_dma + 0x10, 0x1f, 0);

	/* csi_hw_enable */
	rmw(csis + CSIS_CMN, 0xf << 16, 0xf << 16);
	rmw(csis + CSIS_CMN, BIT(21), 0);
	rmw(csis + CSIS_PHY_CMN, BIT(0), BIT(0));
	rmw(csis + CSIS_CMN, BIT(0), BIT(0));
}

static void dma_stop(void)
{
	if (csis_vc)
		writel(0, csis_vc + VC_CTRL);
}

static void csis_off(void)
{
	dma_stop();
	/* csi_hw_disable, csi_hw_reset, masks off, PHY back in reset */
	rmw(csis + CSIS_PHY_CMN, 0x1f, 0);
	rmw(csis + CSIS_CMN, BIT(0), 0);
	rmw(csis + CSIS_CMN, BIT(1), BIT(1));
	udelay(100);
	writel(0, csis + CSIS_MSK0);
	writel(0, csis + CSIS_MSK1);
	writel(0, csis_vc + CMN_MASK);
	rmw(sys_phy, BIT(0), 0);
}

static void dma_prepare(void)
{
	unsigned int i;

	memset(frame, CANARY_FRONT, FRAME_FRONT);
	memset(frame + FRAME_FRONT, 0, FRAME_AREA);
	memset(frame + FRAME_FRONT + FRAME_AREA, CANARY_REAR, FRAME_REAR);
	dma_wmb();
	/* csi_s_config_dma mode: 2D, legacy storage, RAW10, not "pack12": 16 bits a pixel */
	writel(0, csis_vc + VC_FMT);
	writel(SENSOR_W, csis_vc + VC_RESOL);
	for (i = 0; i < 8; i++)
		writel(lower_32_bits(frame_dma) + FRAME_FRONT, csis_vc + VC_ADDR(i));
	writel(0, csis_vc + CMN_OTF);
	writel(0, csis_vc + CMN_DATA);
	writel(0x00f11f00, csis_vc + CMN_MASK);
	writel(readl(csis_vc + CMN_SRC), csis_vc + CMN_SRC);
}

static int frame_check(void)
{
	size_t i, bad_f = 0, bad_r = 0, last = 0;
	const u8 *p = frame;

	for (i = 0; i < FRAME_FRONT; i++)
		bad_f += p[i] != CANARY_FRONT;
	for (i = 0; i < FRAME_REAR; i++)
		bad_r += p[FRAME_FRONT + FRAME_AREA + i] != CANARY_REAR;
	for (i = 0; i < FRAME_AREA; i++)
		if (p[FRAME_FRONT + i])
			last = i + 1;
	frame_len = last;
	note("canaries: front %zu changed, rear %zu changed; last non-zero byte at %zu", bad_f, bad_r, last);
	return bad_f || bad_r ? -EFAULT : 0;
}

static void poll_stream(unsigned int ms, bool want_frame)
{
	u32 fs = 0, fe = 0, fs1 = 0, fe1 = 0, or0 = 0, or1 = 0, dma_or = 0, dfs = 0, dfe = 0, first_err = 0;
	unsigned int t, stat_hs = 0;
	bool armed = false, taken = false;
	ktime_t t0 = ktime_get();

	for (t = 0; t < ms && !stop_now; t++) {
		u32 s0 = readl(csis + CSIS_SRC0), s1 = readl(csis + CSIS_SRC1), d;

		if (s0)
			writel(s0, csis + CSIS_SRC0);
		if (s1)
			writel(s1, csis + CSIS_SRC1);
		fs += !!(s0 & BIT(24));
		fe += !!(s0 & BIT(20));
		fs1 += !!(s0 & BIT(25));
		fe1 += !!(s0 & BIT(21));
		or0 |= s0;
		or1 |= s1;
		if (!first_err && ((s0 & 0x000fff1f) || (s1 & 0xfff0)))
			first_err = t + 1;
		d = readl(csis_vc + CMN_SRC);
		if (d) {
			writel(d, csis_vc + CMN_SRC);
			dma_or |= d;
			dfs += !!(d & BIT(4));
			dfe += !!(d & BIT(8));
		}
		if (!(readl(csis + CSIS_PHY_STAT) & 0xf0))
			stat_hs++;
		if (want_frame && !armed && fs >= 5) {
			writel(BIT(0) | BIT(1), csis_vc + VC_CTRL);
			armed = true;
		}
		/* the first full frame out: stop before another starts over it */
		if (armed && !taken && dfe) {
			dma_stop();
			taken = true;
		}
		if (taken && t > 1 && !(d & BIT(4)) && dfs && fe > fs + 1000)
			break;
		usleep_range(900, 1100);
	}
	dma_stop();
	note("polled %lld ms, %u steps: FS %u FE %u, VC1 FS %u FE %u", ktime_ms_delta(ktime_get(), t0), t, fs, fe, fs1, fe1);
	note("src0 or 0x%08x src1 or 0x%08x first error at step %u; PHY seen active on %u; frame count 0x%x",
	     or0, or1, first_err, stat_hs, readl(csis + CSIS_FRM_CNT0));
	if (want_frame)
		note("DMA: src or 0x%08x, start %u end %u, err code 0x%x, act 0x%x; armed %d taken %d",
		     dma_or, dfs, dfe, readl(csis_vc + CMN_ERR), readl(csis_vc + VC_ACT), armed, taken);
}

static void cis_status(const char *when)
{
	u8 b[2] = { 0, 0 }, fc = 0, mode = 0;

	read16(sensor, 0x0005, &fc, 1);
	read16(sensor, 0x0100, &mode, 1);
	read16(sensor, 0x0340, b, 2);
	note("%s: sensor mode_select %u frame_count %u frame_length 0x%02x%02x", when, mode, fc, b[0], b[1]);
}

static int stream_run(bool want_frame, bool pattern)
{
	u32 ver;
	int ret;

	ret = csis_map();
	if (ret)
		return ret;
	ver = readl(csis + CSIS_VERSION);
	if (ver != 0x05010000) {
		note("CSIS version 0x%08x, not v5.1; stopped", ver);
		return -ENODEV;
	}
	if (want_frame && !frame) {
		frame = dma_alloc_coherent(ctl_dev, FRAME_SIZE, &frame_dma, GFP_KERNEL);
		if (!frame)
			return -ENOMEM;
		note("frame buffer at 0x%llx", (u64)frame_dma);
	}
	bus_note("before");
	read_id(&cameras[0]);
	ret = cis_table(tb_reset_tnp, ARRAY_SIZE(tb_reset_tnp));
	if (!ret)
		ret = cis_table(tb_global, ARRAY_SIZE(tb_global));
	if (!ret)
		ret = cis_table(tb_mode_2016x1512_30, ARRAY_SIZE(tb_mode_2016x1512_30));
	if (!ret && pattern)
		ret = cis_write8(0x0601, 2);
	if (ret) {
		note("sensor set-up failed: %d, nothing streamed", ret);
		return ret;
	}
	cis_status("set up");
	__pm_stay_awake(stream_ws);
	if (want_frame)
		dma_prepare();
	csis_on();
	note("phy status before stream 0x%x, CMN 0x%08x", readl(csis + CSIS_PHY_STAT), readl(csis + CSIS_CMN));

	/* sensor_2l3_cis_stream_on */
	cis_write8(0x0104, 1);
	ret = cis_write16(0x0100, 0x0103);
	cis_write8(0x0104, 0);
	if (ret)
		note("stream on: %d", ret);
	poll_stream(want_frame ? 1500 : 5000, want_frame);
	cis_status("after");

	dma_stop();
	cis_write8(0x0100, 0);	/* sensor_2l3_cis_stream_off */
	msleep(50);
	csis_off();
	__pm_relax(stream_ws);
	if (want_frame) {
		dma_rmb();
		ret = frame_check();
	}
	return ret;
}
