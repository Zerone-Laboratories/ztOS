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
