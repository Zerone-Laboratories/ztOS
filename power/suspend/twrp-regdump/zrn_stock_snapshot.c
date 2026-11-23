// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-stock-snapshot (TWRP / Samsung 4.9 kernel only): wrap Samsung's
 * platform suspend .enter (exynos_pm_enter) so that, after every syscore
 * suspend write (cal_pm_enter: enter + save tables, wake masks) and right
 * before arm_cpuidle_suspend's PSCI call, the registers in zrn-snapshot.h
 * are read. Read-only; it then calls Samsung's own exynos_pm_enter.
 *
 *   insmod zrn_stock_snapshot.ko; echo +15 > /sys/class/rtc/rtc0/wakealarm;
 *   echo mem > /sys/power/state; cat /proc/zrn-stock-snapshot
 *
 * Stays loaded (no exit): reboot TWRP afterwards.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/suspend.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "zrn-snapshot.h"

static struct zrn_snap snap_awake, snap_sleep;
static int (*orig_enter)(suspend_state_t);

static int snap_enter(suspend_state_t state)
{
	zrn_snap_take(&snap_sleep);
	return orig_enter(state);
}

static const struct platform_suspend_ops snap_ops = {
	.enter = snap_enter,
	.valid = suspend_valid_only_mem,
};

static int snap_show(struct seq_file *m, void *v)
{
	ZRN_SNAP_SHOW(m, &snap_awake, "stock-awake");
	ZRN_SNAP_SHOW(m, &snap_sleep, "stock-sleep-enter");
	return 0;
}

static int snap_open(struct inode *inode, struct file *file)
{
	return single_open_size(file, snap_show, NULL, 1 << 20);
}

static const struct file_operations snap_fops = {
	.open = snap_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init zrn_stock_snapshot_init(void)
{
	int ret;

	orig_enter = (void *)kallsyms_lookup_name("exynos_pm_enter");
	if (!orig_enter) {
		pr_err("zrn-stock-snapshot: exynos_pm_enter not found\n");
		return -ENOENT;
	}
	snap_awake.sram = vmalloc(ZRN_SNAP_SRAM_SIZE);
	snap_sleep.sram = vmalloc(ZRN_SNAP_SRAM_SIZE);
	if (!snap_awake.sram || !snap_sleep.sram)
		return -ENOMEM;
	ret = zrn_snap_map();
	if (ret) {
		pr_err("zrn-stock-snapshot: map failed %d (pages %d)\n", ret, zrn_snap_npages);
		return ret;
	}
	zrn_snap_take(&snap_awake);
	if (!proc_create("zrn-stock-snapshot", 0444, NULL, &snap_fops))
		return -ENOMEM;
	suspend_set_ops(&snap_ops);
	pr_info("zrn-stock-snapshot: %zu regs over %d pages; wrapping exynos_pm_enter %pS\n",
		(size_t)ZRN_SNAP_N, zrn_snap_npages, orig_enter);
	return 0;
}

module_init(zrn_stock_snapshot_init);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only register snapshot at Samsung's suspend enter");
