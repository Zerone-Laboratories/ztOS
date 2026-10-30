#!/bin/bash
# r2seq.sh "MODE S TAG [FOLLOW]"...: bench runs one after another (root, one systemd-run)
H=/home/zerone/team-r2
for run in "$@"; do
	set -- $run
	$H/r2bench2.sh "$@"
	sleep 20
done
touch $H/seq-done-$(date +%s)
