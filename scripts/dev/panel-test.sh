#!/bin/sh
# panel-test.sh: one screen off/on cycle using Samsung's own panel sequences (dsim.py), with the
# driver's panel_sleep off so it only sends display off/on itself. Run as root on the phone.
# A panel that stays dark is brought back by `screen on` after `dsim.py up`, or by a reboot.
. /usr/lib/ztos/user.sh
C=$ZTOS_RUNTIME_DIR/ztmetro.ctl
echo N > /sys/module/s9p_decon/parameters/panel_sleep
printf 'brightness 100\n' > $C
printf 'screen off\n' > $C; sleep 2
python3 /tmp/zrn/dsim.py down; echo "down: $?"
sleep 4
python3 /tmp/zrn/dsim.py ${1:-up}; echo "up ${1:-up}: $?"
printf 'screen on\n' > $C
echo done
