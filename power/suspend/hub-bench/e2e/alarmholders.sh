#!/bin/bash
# list timerfds with clockid 8/9 (*_ALARM): pid, comm, it_value
for f in /proc/[0-9]*/fdinfo/*; do
	grep -q '^clockid:[[:space:]]*[89]$' "$f" 2>/dev/null || continue
	p=${f#/proc/}; p=${p%%/*}
	echo "pid=$p comm=$(cat /proc/$p/comm 2>/dev/null) $(grep -E 'clockid|it_value|it_interval' $f | tr -s ' \t' ' ' | tr '\n' ' ')"
done
cat /proc/timer_list 2>/dev/null | grep -A3 -i 'alarm' | head -20
