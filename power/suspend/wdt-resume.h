/* SPDX-License-Identifier: GPL-2.0-only */
/* The cluster-1 watchdog (0x10060000) over the resume window.
 *
 * Mainline's s3c2410 driver stops the watchdog and masks its PMU reset
 * route (bit 23 of 0x14060408 AUTOMATIC_WDT_RESET_DISABLE and 0x1406040c
 * MASK_WDT_RESET_REQUEST) in device suspend, and SYS_SLEEP resets the
 * block anyway. From the SMC's return to the driver's own resume nothing
 * can reset a hung phone: a hang in restore_platform, cpu_resume or
 * bringing CPU4's cluster back. zrn_wdt_arm() runs the timer there with
 * reset on and the route open; zrn_wdt_release(), after the resume,
 * leaves it to the driver if the driver took it back, else stops it and
 * puts the route bits back as they were. Only bit 23 is touched.
 *
 * Shared with power/suspend/watchdog-test/test-routing.py (fake MMIO). */
#ifndef ZRN_WDT_RESUME_H
#define ZRN_WDT_RESUME_H

/* OSCCLK 26 MHz / (prescaler 255 + 1) / 128: mainline's max_timeout of 82 s */
#define ZRN_WDT_TICKS_PER_S	793
/* WTCON: prescaler 0xff, divider 128 (3 << 3), timer on (5), reset on (0) */
#define ZRN_WDT_CONTROL		0xff39

struct zrn_wdt_cover {
	void __iomem *wdt, *pmu;
	u32 count, route_disable, route_mask;
	bool armed;
	unsigned int arms, handed, stopped;
};

static inline void zrn_wdt_arm(struct zrn_wdt_cover *c, unsigned int seconds)
{
	u32 count;

	if (!seconds || !c->wdt || !c->pmu || c->armed)
		return;
	count = seconds >= 0xffff / ZRN_WDT_TICKS_PER_S ? 0xffff : seconds * ZRN_WDT_TICKS_PER_S;
	writel_relaxed(0, c->wdt);
	writel_relaxed(count, c->wdt + 4);	/* WTDAT */
	writel_relaxed(count, c->wdt + 8);	/* WTCNT */
	c->route_disable = readl_relaxed(c->pmu + 0x408) & BIT(23);
	c->route_mask = readl_relaxed(c->pmu + 0x40c) & BIT(23);
	writel_relaxed(readl_relaxed(c->pmu + 0x408) & ~BIT(23), c->pmu + 0x408);
	writel_relaxed(readl_relaxed(c->pmu + 0x40c) & ~BIT(23), c->pmu + 0x40c);
	writel_relaxed(ZRN_WDT_CONTROL, c->wdt);
	c->count = count;
	c->armed = true;
	c->arms++;
}

static inline void zrn_wdt_release(struct zrn_wdt_cover *c)
{
	if (!c->armed)
		return;
	c->armed = false;
	if (readl_relaxed(c->wdt) != ZRN_WDT_CONTROL || readl_relaxed(c->wdt + 4) != c->count) {
		c->handed++;	/* the driver's resume wrote its own settings */
		return;
	}
	writel_relaxed(0, c->wdt);
	writel_relaxed((readl_relaxed(c->pmu + 0x408) & ~BIT(23)) | c->route_disable, c->pmu + 0x408);
	writel_relaxed((readl_relaxed(c->pmu + 0x40c) & ~BIT(23)) | c->route_mask, c->pmu + 0x40c);
	c->stopped++;
}

#endif
