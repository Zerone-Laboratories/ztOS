// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-score: the SCORE device, as the stock DT describes it (score@16D00000:
 * 1 MB of registers, interrupt SPI 222, dma-coherent), registered by the
 * module itself: ztOS's DT has no node for it. Its power domains (DSPM,
 * DSPS) and clocks are left on by the bootloader; its two IOMMUs are left
 * off, so the DSP works in physical addresses below 4 GB.
 */
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "zrn_score_board.h"

#define SCORE_SFR	0x16d00000
#define SCORE_SFR_SIZE	0x100000
#define SCORE_SPI	222

static struct platform_device *score_pdev;

static int score_irq(void)
{
	struct device_node *gic = of_find_compatible_node(NULL, NULL, "arm,gic-400");
	struct irq_fwspec spec = { .param_count = 3, .param = { 0, SCORE_SPI, IRQ_TYPE_LEVEL_HIGH } };
	int irq;

	if (!gic)
		return -ENODEV;
	spec.fwnode = of_fwnode_handle(gic);
	irq = irq_create_fwspec_mapping(&spec);
	of_node_put(gic);
	return irq > 0 ? irq : -EINVAL;
}

int zrn_score_board_init(void)
{
	struct resource res[2] = {
		DEFINE_RES_MEM(SCORE_SFR, SCORE_SFR_SIZE),
		{ .flags = IORESOURCE_IRQ },
	};
	struct platform_device_info info = {
		.name = "exynos-score",
		.id = PLATFORM_DEVID_NONE,
		.res = res,
		.num_res = 2,
		.dma_mask = DMA_BIT_MASK(32),
	};
	int irq = score_irq();

	if (irq < 0)
		return irq;
	res[1].start = res[1].end = irq;
	score_pdev = platform_device_register_full(&info);
	if (IS_ERR(score_pdev)) {
		int ret = PTR_ERR(score_pdev);

		score_pdev = NULL;
		return ret;
	}
	return 0;
}

void zrn_score_board_exit(void)
{
	if (score_pdev)
		platform_device_unregister(score_pdev);
	score_pdev = NULL;
}
