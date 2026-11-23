// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-twrp-regdump: read-only dump of the Exynos 9810 DRAM controller (DREX)
 * channels, the MIF block at 0x1A240000, the PMU MIF-sequence registers and
 * the ACPM flexpmu debug words, for TWRP's Samsung 4.9 kernel (which has no
 * /dev/mem). Every access is a readl; nothing is written.
 *
 * Output goes to the kernel log as
 *   zrn-regdump: <phys> <w0> <w1> <w2> <w3>
 * The module refuses to stay loaded (returns -ECANCELED) so no rmmod is needed.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>

struct region {
	phys_addr_t base;
	size_t size;
};

static const struct region regions[] = {
	/* DREX channels 0-3: the same 0x400 that dmc-ztos-awake.json holds */
	{ 0x1b830000, 0x400 },
	{ 0x1b930000, 0x400 },
	{ 0x1ba30000, 0x400 },
	{ 0x1bb30000, 0x400 },
	/* MIF common block (CLKOUT 0x810/0x814, HCHGEN 0x850) */
	{ 0x1a240000, 0x1000 },
	/* PMU: wake masks 0x600-0x64f */
	{ 0x14060600, 0x50 },
	/* PMU MIF sequence targets */
	{ 0x14062960, 0x10 },	/* RESET_CMU_MIF_* */
	{ 0x14062980, 0x10 },	/* DDRPHY_CLKSTOP */
	{ 0x140629e0, 0x10 },	/* DDRPHY_DLL_CLK */
	{ 0x14062b00, 0x10 },	/* DISABLE_PLL_CMU_MIF */
	{ 0x14062c80, 0x10 },	/* TOP_BUS_MIF */
	{ 0x14062ce0, 0x10 },	/* PSCDC_MIF */
	{ 0x14063000, 0x10 },	/* PAD_RETENTION_LPDDR4 */
	{ 0x14063620, 0x10 },	/* EXT_REGULATOR_MIF */
	{ 0x140682e0, 0x10 },	/* TCXO */
	/* ACPM flexpmu debug words (mif_always_on at +0x15c) */
	{ 0x0204f800, 0x200 },
};

static int __init zrn_regdump_init(void)
{
	size_t i, off;

	pr_info("zrn-regdump: begin\n");
	for (i = 0; i < ARRAY_SIZE(regions); i++) {
		void __iomem *p = ioremap(regions[i].base, regions[i].size);

		if (!p) {
			pr_info("zrn-regdump: %pa ioremap failed\n", &regions[i].base);
			continue;
		}
		for (off = 0; off < regions[i].size; off += 16) {
			u32 w[4];
			int j;

			for (j = 0; j < 4; j++)
				w[j] = off + j * 4 < regions[i].size ?
					readl(p + off + j * 4) : 0;
			pr_info("zrn-regdump: %08llx %08x %08x %08x %08x\n",
				(unsigned long long)(regions[i].base + off),
				w[0], w[1], w[2], w[3]);
		}
		iounmap(p);
	}
	pr_info("zrn-regdump: end\n");
	return -ECANCELED;
}

module_init(zrn_regdump_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only DREX/PMU/flexpmu register dump for TWRP");
