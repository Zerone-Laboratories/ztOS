/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Between zrn-idle.o and zrn-sicd.o, both linked into zrn-cpuidle.ko. */
#ifndef ZRN_IDLE_H
#define ZRN_IDLE_H

#include <linux/cpumask.h>
#include <linux/spinlock.h>
#include <linux/types.h>

/* which cores are down in C2 right now, and the lock that keeps it true */
extern raw_spinlock_t zrn_c2_lock;
extern unsigned long zrn_c2_mask[BITS_TO_LONGS(NR_CPUS)];

void zrn_c2_mark(unsigned int core, bool down);
void __iomem *zrn_pmu_base(void);
void __iomem *zrn_bid_base(void);
int zrn_cpu_suspend_call(unsigned long state, int (*finisher)(unsigned long));
unsigned long zrn_cpu_resume_pa(void);
u64 zrn_mct_count_now(void);
unsigned long zrn_mct_rate_now(void);

struct cpuidle_device;
bool zrn_sicd_try(struct cpuidle_device *dev, unsigned int core, bool suspended, int *ret);
bool zrn_s2idle_any_committer(void);

#endif
