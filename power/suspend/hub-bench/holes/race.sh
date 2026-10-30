#!/bin/sh
# R1 run N: the helper races `zrn-sleepd once 60`; timed-push is not counted
# (a busy chat refreshes it all the time); timed-r2 is removed afterwards
n=$1
d=/home/zerone/team-r2-holes
rm -f /run/zrn-sleepd/inhibit/timed-r2
python3 $d/race-helper.py "$n" ${2:-0.8} > $d/r1-$n.helper 2>&1 &
helper=$!
/usr/local/sbin/zrn-sleepd once 60 --ignore-inhibit timed-push > $d/r1-$n.once 2>&1
wait $helper
rm -f /run/zrn-sleepd/inhibit/timed-r2
echo done >> $d/r1-$n.once
