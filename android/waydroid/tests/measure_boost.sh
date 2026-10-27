#!/usr/bin/env bash
# Run as root while the Waydroid probe is focused. Workload ends after six seconds.
set -eu
lxc_args=(-P /var/lib/waydroid/lxc -n waydroid)
date -Ins
printf 'Before workload\n'
cat /run/zrnperfd.status
timeout 12 lxc-attach "${lxc_args[@]}" --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- /system/bin/sh -c \
    'set -e; end=$(( $(/system/bin/date +%s) + 6 )); while [ "$(/system/bin/date +%s)" -lt "$end" ]; do :; done' &
workload=$!
sleep 3
printf 'During workload\n'
cat /run/zrnperfd.status
cat /sys/fs/cgroup/lxc.payload.waydroid/{cpuset.cpus.effective,cpu.stat}
wait "$workload"
printf 'Workload completed\n'
