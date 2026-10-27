// SPDX-License-Identifier: GPL-2.0-only
/* Bounded 10s trials: rearm the systemd-owned 30s watchdog after its
 * suspend callback stops it. Normal watchdog resume retakes ownership. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/suspend.h>
#include <linux/delay.h>
static void __iomem *wdt, *pmu;
static unsigned int arms, route_errors, count_enter, count_exit;
static unsigned int count_probe, control_enter, control_exit;
static u32 saved_disable, saved_mask, saved_control;
static bool armed;
module_param(arms, uint, 0444);
module_param(route_errors, uint, 0444);
module_param(count_enter, uint, 0444);
module_param(count_exit, uint, 0444);
module_param(count_probe, uint, 0444);
module_param(control_enter, uint, 0444);
module_param(control_exit, uint, 0444);

/* The exynos7 watchdog driver masks both PMU reset paths in suspend.
 * Enabling WTCON alone leaves an expired timer unable to reset the SoC. */
static void restore(void)
{
 if (!armed)
  return;
 count_exit = readl(wdt + 8);
 control_exit = readl(wdt);
 writel(saved_control, wdt);
 writel((readl(pmu + 0x408) & ~BIT(23)) | saved_disable, pmu + 0x408);
 writel((readl(pmu + 0x40c) & ~BIT(23)) | saved_mask, pmu + 0x40c);
 readl(pmu + 0x40c);
 armed = false;
}

static int notify(struct notifier_block *nb, unsigned long event, void *data)
{
 if (event == CPU_PM_EXIT || event == CPU_PM_ENTER_FAILED) {
  restore();
  return NOTIFY_OK;
 }
 if (event == CPU_PM_ENTER && pm_suspend_target_state == PM_SUSPEND_MEM &&
     raw_smp_processor_id() == 0 && num_online_cpus() == 1) {
  u32 control = readl(wdt);
  if (!(control & BIT(5))) {
   saved_control = control;
   saved_disable = readl(pmu + 0x408) & BIT(23);
   saved_mask = readl(pmu + 0x40c) & BIT(23);
   armed = true;
   writel(readl(wdt + 4), wdt + 8);
   writel(readl(pmu + 0x408) & ~BIT(23), pmu + 0x408);
   writel(readl(pmu + 0x40c) & ~BIT(23), pmu + 0x40c);
   if ((readl(pmu + 0x408) | readl(pmu + 0x40c)) & BIT(23)) {
    route_errors++;
    restore();
    return NOTIFY_BAD;
   }
   writel((control | BIT(5) | BIT(0)) & ~BIT(2), wdt);
   count_enter = readl(wdt + 8);
   control_enter = readl(wdt);
   udelay(2000);
   count_probe = readl(wdt + 8);
   arms++;
  }
 }
 return NOTIFY_OK;
}
static struct notifier_block notifier = { .notifier_call = notify };
static int __init start(void)
{
 int ret;
 wdt = ioremap(0x10060000, 0x1000);
 if (!wdt) return -ENOMEM;
 pmu = ioremap(0x14060000, 0x1000);
 if (!pmu) {
  iounmap(wdt);
  return -ENOMEM;
 }
 ret = cpu_pm_register_notifier(&notifier);
 if (ret) {
  iounmap(pmu);
  iounmap(wdt);
 }
 return ret;
}
static void __exit stop(void)
{
 cpu_pm_unregister_notifier(&notifier);
 restore();
 iounmap(pmu);
 iounmap(wdt);
}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Rearm the existing watchdog for bounded full-sleep bringup");
