// SPDX-License-Identifier: GPL-2.0-only
/* One stock MC_FC_INFO request on CPU0. No initialization or core swap. */
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/arm-smccc.h>

static unsigned long response, result, state, extended;
module_param(response, ulong, 0444);
module_param(result, ulong, 0444);
module_param(state, ulong, 0444);
module_param(extended, ulong, 0444);

static long query(void *unused)
{
	struct arm_smccc_res reply;
	/* Pinned vendor mcifc.h: MC_FC_INFO, MC_EXT_INFO_ID_MC_STATUS. */
	arm_smccc_smc(0xff000002, 18, 0, 0, 0, 0, 0, 0, &reply);
	response = reply.a0;
	result = reply.a1;
	state = reply.a2;
	extended = reply.a3;
	pr_info("zrn-tee-info: cpu=%u response=%lx result=%lx state=%lx extended=%lx\n",
		raw_smp_processor_id(), response, result, state, extended);
	return 0;
}

static int __init start(void)
{
	long ret;
	cpus_read_lock();
	if (!cpu_online(0)) {
		cpus_read_unlock();
		return -ENODEV;
	}
	ret = work_on_cpu(0, query, NULL);
	cpus_read_unlock();
	return ret;
}

static void __exit stop(void) {}
module_init(start);
module_exit(stop);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read stock Trustonic runtime status on CPU0");
