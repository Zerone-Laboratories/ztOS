#!/usr/bin/env bash
# Run as root. Queries thaw Android; measure FROZEN externally instead.
set -euo pipefail
[[ $EUID == 0 ]] || exit 1
printf 'UTC: '; date -u +%FT%TZ
waydroid status
state=$(lxc-info -P /var/lib/waydroid/lxc -n waydroid -sH 2>/dev/null || true)
for name in memory.current memory.max memory.events memory.stat; do
    path=/sys/fs/cgroup/lxc.payload.waydroid/$name
    if [[ -f $path ]]; then echo "$path"; cat "$path"; fi
done
free -m
stat -c '%n %s bytes' /usr/share/waydroid-extra/images/{system.img,system.img.stock,vendor.img}
if [[ $state != RUNNING ]]; then exit 0; fi
android() { timeout 10 lxc-attach -P /var/lib/waydroid/lxc -n waydroid --clear-env -v PATH=/system/bin:/system/xbin:/vendor/bin -- "/system/bin/$@"; }
for prop in sys.boot_completed ro.zygote ro.product.cpu.abilist ro.product.cpu.abilist32 init.svc.zygote init.svc.zygote_secondary ro.lmk.use_psi; do
    printf '%s=' "$prop"; android getprop "$prop"
done
android ps -A | grep -E 'zygote|system_server|systemui|whatsapp' || true
android settings get global restricted_networking_mode
android ip route show table all
android dumpsys connectivity | grep 'Active default network' || true
android curl -sS -o /dev/null -w 'HTTP %{http_code}\n' --max-time 8 http://connectivitycheck.gstatic.com/generate_204
android logcat -d -b crash | grep -E 'Cmdline|Fatal|Abort message' || true
