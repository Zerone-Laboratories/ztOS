#!/bin/sh
cd "$(dirname "$0")/../.."
export ZT_PW=${ZT_PW:?set ZT_PW to the phone user's password}
PHONE=172.16.42.1 HOST=172.16.42.2 audio/push.sh build/rec3.sh
timeout 60 python3 scripts/ssh-pw.py alarm "$ZT_PW" 172.16.42.1 "pgrep -a ztmetro | head -1; sh /tmp/zrn/rec3.sh" < /dev/null
timeout 30 python3 scripts/ssh-pw.py alarm "$ZT_PW" 172.16.42.1 "rm -rf /tmp/ztrec-kb; mv /tmp/ztrec /tmp/ztrec-kb; mv /tmp/ztrec-open /tmp/ztrec" < /dev/null
timeout 100 scripts/anim/fetch.sh notes-open3
timeout 30 python3 scripts/ssh-pw.py alarm "$ZT_PW" 172.16.42.1 "rm -rf /tmp/ztrec; mv /tmp/ztrec-kb /tmp/ztrec" < /dev/null
timeout 100 scripts/anim/fetch.sh notes-kb
echo RUN-DONE
