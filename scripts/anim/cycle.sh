#!/bin/bash
# cycle.sh NAME [on|off]: build ztmetro on the phone, install, restart, record a launch and return, fetch
cd "$(dirname "$0")/../.."
S=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PHONE=172.16.42.1 HOST=172.16.42.2 timeout 400 scripts/ztmetro-push.sh 2>&1 | grep -E " error" | head -3
timeout 300 scripts/ssh-pw.py alarm "${ZT_PW:-alarm}" 172.16.42.1 'cd ~/ztmetro && touch ztmetro.c core/*.c && make 2>&1 | grep -E "error|(ztmetro|zrnmetrocore).c.*warning" | head -3' 2>&1 | tail -3
timeout 120 scripts/ssh-pw.py alarm "${ZT_PW:-alarm},root" 172.16.42.1 "su -c 'make -s -C /home/alarm/ztmetro install 2>&1 | grep -v -i skew | tail -1; zboot-session ztmetro | tail -1'" 2>&1 | tail -1
timeout 60 bash -c 'until timeout 10 scripts/ssh-pw.py alarm "${ZT_PW:-alarm}" 172.16.42.1 "test -p \$XDG_RUNTIME_DIR/ztmetro.ctl && pgrep -x ztmetro >/dev/null && echo up" 2>/dev/null | grep -q up; do sleep 2; done'
sleep 4
PHONE=172.16.42.1 HOST=172.16.42.2 audio/push.sh $S/rec.sh >/dev/null 2>&1
timeout 90 scripts/ssh-pw.py alarm "${ZT_PW:-alarm}" 172.16.42.1 "sh /tmp/zrn/rec.sh ${2:-on} 9" 2>&1 | tail -4
$S/fetch.sh $1
