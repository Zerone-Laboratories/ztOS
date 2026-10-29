// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-acpm-log: pin two pages of RAM in the low 256 MiB, where ACPM (the
 * Exynos 9810 power firmware) can reach DRAM through its 0x80000000 window,
 * so its flexpmu sleep-sequence log can be pointed there (acpmlogptr.py set).
 * DRAM keeps its contents across a forced reset, so a sleep that never comes
 * back still leaves the markers of how far ACPM got.
 *
 * The pages stay allocated until reboot: the module cannot be removed.
 */
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/string.h>

static unsigned long phys;
module_param(phys, ulong, 0444);
MODULE_PARM_DESC(phys, "physical address of the pinned log pages");

#define LO 0x85000000UL
#define HI 0x8e000000UL
#define TRIES 4096

static int __init zrn_acpm_log_init(void)
{
	LIST_HEAD(keep);
	struct page *got = NULL, *p, *t;
	int n = 0;

	while (n < TRIES) {
		phys_addr_t a;

		p = alloc_pages(GFP_KERNEL | __GFP_NOWARN, 1);

		if (!p)
			break;
		a = page_to_phys(p);
		if (a >= LO && a + 2 * PAGE_SIZE <= HI) {
			got = p;
			break;
		}
		list_add(&p->lru, &keep);
		n++;
	}
	list_for_each_entry_safe(p, t, &keep, lru) {
		list_del(&p->lru);
		__free_pages(p, 1);
	}
	if (!got) {
		pr_err("zrn-acpm-log: no page in [%lx, %lx)\n", LO, HI);
		return -ENOMEM;
	}
	memset(page_address(got), 0, 2 * PAGE_SIZE);
	phys = page_to_phys(got);
	pr_info("zrn-acpm-log: pinned %lx\n", phys);
	return 0;
}
module_init(zrn_acpm_log_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Pinned RAM for ACPM's sleep-sequence log");
