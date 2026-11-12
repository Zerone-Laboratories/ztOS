#!/bin/sh
# A1 (root): the container's init and system_server capabilities, clockids
pid_init=$(lxc-info -n waydroid -P /var/lib/waydroid/lxc -p -H 2>/dev/null)
echo "container init pid: $pid_init"
grep -E 'CapEff|CapBnd' /proc/$pid_init/status
sha256sum /proc/$pid_init/root/system/bin/init 2>/dev/null || sha256sum /proc/$pid_init/exe
for p in $(ls /proc | grep -E '^[0-9]+$'); do
    [ "$(cat /proc/$p/comm 2>/dev/null)" = system_server ] && ss=$p
done
echo "system_server pid: ${ss:-none}"
[ -n "${ss:-}" ] && grep -E 'CapEff|CapPrm|CapBnd' /proc/$ss/status
[ -n "${ss:-}" ] && for f in /proc/$ss/fdinfo/*; do grep -h '^clockid' $f 2>/dev/null; done | sort | uniq -c
