// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-acpm-dbg: read-only snapshot of ACPM's flexpmu debug block in APM SRAM
 * (0x0204f800..0x0204fa00) on ztOS, where /dev/mem SIGBUSes on that range.
 * Fields of interest: seq_status 0x0204f818, cur_seq 0x0204f828, SEQ_STATUS
 * access type 0x0204f838, current sequencer row 0x0204f83c, mif_always_on
 * 0x0204f95c. Used to tell whether the block survives a reset, so that after
 * a MIF-down hang it shows where the ACPM sequencer stopped.
 *
 * Prints "zrn-acpm-dbg: <phys> <w0> <w1> <w2> <w3>" and refuses to stay
 * loaded (-ECANCELED), so it never needs rmmod. Nothing is written.
 */
#include <linux/module.h>
#include <linux/io.h>

#define DBG_BASE 0x0204f800
#define DBG_SIZE 0x200

static int __init zrn_acpm_dbg_init(void)
{
	void __iomem *p = ioremap(DBG_BASE, DBG_SIZE);
	unsigned int off;

	if (!p) {
		pr_info("zrn-acpm-dbg: ioremap failed\n");
		return -ENOMEM;
	}
	pr_info("zrn-acpm-dbg: begin\n");
	for (off = 0; off < DBG_SIZE; off += 16)
		pr_info("zrn-acpm-dbg: %08x %08x %08x %08x %08x\n", DBG_BASE + off,
			readl(p + off), readl(p + off + 4), readl(p + off + 8), readl(p + off + 12));
	pr_info("zrn-acpm-dbg: end\n");
	iounmap(p);
	return -ECANCELED;
}

module_init(zrn_acpm_dbg_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only ACPM flexpmu debug block snapshot");
