// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-regpeek: read-only peek of up to 64 physical 32-bit registers given as
 * a module parameter, for SFRs that /dev/mem SIGBUSes on (claimed by drivers).
 *   insmod zrn_regpeek.ko addrs=0x14061088,0x140682fc
 * Prints "zrn-regpeek: <phys> <value>" and refuses to stay loaded.
 */
#include <linux/module.h>
#include <linux/io.h>

static unsigned long addrs[64];
static int naddrs;
module_param_array(addrs, ulong, &naddrs, 0);

static int __init zrn_regpeek_init(void)
{
	int i;

	for (i = 0; i < naddrs; i++) {
		void __iomem *p;

		if (addrs[i] & 3)
			continue;
		p = ioremap(addrs[i], 4);
		if (!p) {
			pr_info("zrn-regpeek: %08lx ioremap failed\n", addrs[i]);
			continue;
		}
		pr_info("zrn-regpeek: %08lx %08x\n", addrs[i], readl(p));
		iounmap(p);
	}
	return -ECANCELED;
}

module_init(zrn_regpeek_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only physical register peek");
