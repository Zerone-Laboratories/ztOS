#!/bin/sh
# load checks after the round-2 reboot (root: 0600 parameters)
P=/sys/module/zrn_sleep_init/parameters
md5sum /lib/modules/7.1.0-zboot/extra/zrn-bbd.ko /usr/local/libexec/ztos-power/mif-trial/zrn-sleep-init.ko
dmesg | grep -E "follows IRQ|cannot follow|zrn-bbd: transport up|pdn gpg0" | tail -5
for n in eint_follow eint_effective hub_refused hub_pend_cleared grp4_stale_clear grp4_stale_clears grp4_at_smc clear_grp4 eint_wake; do printf '%s=%s\n' $n "$(cat $P/$n 2>/dev/null)"; done
echo "ap_sleep_pulse=$(cat /sys/module/zrn_bbd/parameters/ap_sleep_pulse) us=$(cat /sys/module/zrn_bbd/parameters/ap_sleep_pulse_us) resume_seq=$(cat /sys/class/sec/sensorhub/resume_seq) ap_suspends=$(cat /sys/class/sec/sensorhub/ap_suspends) ready=$(cat /sys/class/sec/sensorhub/ready)"
grep -E "wakeup|chip|hwirq" /sys/kernel/irq/107/* 2>/dev/null | head -4
