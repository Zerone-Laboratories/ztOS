#!/usr/bin/env python3
"""Run the real notifier against fake MMIO; no watchdog hardware access."""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).with_name('zrn_deep_watchdog_test.c').read_text()
source = '\n'.join(line for line in source.splitlines()
                   if not line.startswith('#include'))
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t u32;
#define __iomem
#define __init
#define __exit
#define BIT(n) (1U << (n))
#define module_param(...)
#define module_init(...)
#define module_exit(...)
#define MODULE_LICENSE(...)
#define MODULE_DESCRIPTION(...)
#define ENOMEM 12
#define CPU_PM_ENTER 1
#define CPU_PM_EXIT 2
#define CPU_PM_ENTER_FAILED 3
#define PM_SUSPEND_MEM 3
#define NOTIFY_OK 1
#define NOTIFY_BAD 2
struct notifier_block {
 int (*notifier_call)(struct notifier_block *, unsigned long, void *);
};
static u32 fake_wdt[1024], fake_pmu[1024];
static bool sticky_mask;
static int pm_suspend_target_state = PM_SUSPEND_MEM;
static int raw_smp_processor_id(void) { return 0; }
static int num_online_cpus(void) { return 1; }
static u32 readl(void *p) { return *(u32 *)p; }
static void writel(u32 value, void *p) {
 if (sticky_mask && p == &fake_pmu[0x40c / 4]) value |= BIT(23);
 *(u32 *)p = value;
}
static void *ioremap(unsigned long addr, unsigned long size) {
 return addr == 0x10060000 ? fake_wdt : fake_pmu;
}
static void iounmap(void *p) {}
static void udelay(unsigned int us) {}
static int cpu_pm_register_notifier(struct notifier_block *n) { return 0; }
static void cpu_pm_unregister_notifier(struct notifier_block *n) {}
'''
checks = r'''
int main(void) {
 assert(start() == 0);
 fake_wdt[0] = 0xff18;
 fake_wdt[1] = 30000;
 fake_pmu[0x408 / 4] = BIT(23) | BIT(7);
 fake_pmu[0x40c / 4] = BIT(23) | BIT(9);
 assert(notify(NULL, CPU_PM_ENTER, NULL) == NOTIFY_OK);
 assert(armed && arms == 1);
 assert(fake_wdt[2] == 30000);
 assert(count_enter == 30000);
 assert((fake_wdt[0] & (BIT(5) | BIT(0))) == (BIT(5) | BIT(0)));
 assert(!(fake_wdt[0] & BIT(2)));
 assert(fake_pmu[0x408 / 4] == BIT(7));
 assert(fake_pmu[0x40c / 4] == BIT(9));
 fake_pmu[0x408 / 4] |= BIT(8);
 fake_wdt[2] = 24000;
 assert(notify(NULL, CPU_PM_EXIT, NULL) == NOTIFY_OK);
 assert(!armed && fake_wdt[0] == 0xff18);
 assert(count_exit == 24000);
 assert(fake_pmu[0x408 / 4] == (BIT(23) | BIT(7) | BIT(8)));
 assert(fake_pmu[0x40c / 4] == (BIT(23) | BIT(9)));
 assert(notify(NULL, CPU_PM_ENTER, NULL) == NOTIFY_OK);
 assert(notify(NULL, CPU_PM_ENTER_FAILED, NULL) == NOTIFY_OK);
 assert(!armed && fake_wdt[0] == 0xff18);
 sticky_mask = true;
 assert(notify(NULL, CPU_PM_ENTER, NULL) == NOTIFY_BAD);
 assert(!armed && route_errors == 1 && arms == 2);
 assert(fake_wdt[0] == 0xff18);
 sticky_mask = false;
 pm_suspend_target_state = 0;
 assert(notify(NULL, CPU_PM_ENTER, NULL) == NOTIFY_OK);
 assert(!armed && arms == 2);
 stop();
 return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'routing.c'
    binary = Path(directory) / 'routing'
    test.write_text(prefix + source + checks)
    subprocess.run(['cc', '-std=gnu11', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: arm, wake restore, abort restore, masked-route refusal, inactive no-op')

# zrn-sleep-init's resume-window cover (power/suspend/wdt-resume.h)
cover = Path(__file__).resolve().parents[1].joinpath('wdt-resume.h').read_text()
cover_prefix = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#define __iomem
#define BIT(n) (1U << (n))
static u32 wdt[4], pmu[0x1000 / 4];
static u32 readl_relaxed(void *p) { return *(u32 *)p; }
static void writel_relaxed(u32 v, void *p) { *(u32 *)p = v; }
"""
cover_checks = r"""
int main(void) {
 struct zrn_wdt_cover c = { .wdt = wdt, .pmu = pmu };
 /* off: nothing touched */
 pmu[0x408 / 4] = BIT(23) | BIT(7);
 pmu[0x40c / 4] = BIT(23) | BIT(9);
 zrn_wdt_arm(&c, 0);
 assert(!c.armed && wdt[0] == 0 && pmu[0x408 / 4] == (BIT(23) | BIT(7)));
 /* armed: 60 s, reset on, route open, other PMU bits kept */
 zrn_wdt_arm(&c, 60);
 assert(c.armed && c.arms == 1);
 assert(wdt[0] == 0xff39 && wdt[1] == 60 * 793 && wdt[2] == 60 * 793);
 assert(pmu[0x408 / 4] == BIT(7) && pmu[0x40c / 4] == BIT(9));
 /* no driver took it: stopped, route as before */
 pmu[0x408 / 4] |= BIT(8);
 zrn_wdt_release(&c);
 assert(!c.armed && c.stopped == 1 && wdt[0] == 0);
 assert(pmu[0x408 / 4] == (BIT(23) | BIT(7) | BIT(8)) && pmu[0x40c / 4] == (BIT(23) | BIT(9)));
 zrn_wdt_release(&c);
 assert(c.stopped == 1);
 /* over 82 s: the longest count */
 zrn_wdt_arm(&c, 600);
 assert(wdt[1] == 0xffff);
 /* the driver took it back (systemd's 30 s): left alone */
 wdt[0] = 0x5c39; wdt[1] = 0xfff5; pmu[0x408 / 4] = 0; pmu[0x40c / 4] = 0;
 zrn_wdt_release(&c);
 assert(c.handed == 1 && wdt[0] == 0x5c39 && pmu[0x408 / 4] == 0 && pmu[0x40c / 4] == 0);
 return 0;
}
"""
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'cover.c'
    binary = Path(directory) / 'cover'
    test.write_text(cover_prefix + cover + cover_checks)
    subprocess.run(['cc', '-std=gnu11', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: resume cover off, arm, stop and route restore, 82 s cap, driver hand-over')
