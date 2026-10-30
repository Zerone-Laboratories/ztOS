#!/usr/bin/env python3
"""Bench only: zrn-sleepd's `once SECONDS`, with the timed-push inhibit not
counted. zrn-pushd holds timed-push for 60 s after every push message; a
busy chat (30 Oct: Beeper, about 8 a minute for over an hour) then holds
every sleep off, and a bench of modem wakes needs real sleeps. Messages
that come in while it sleeps wait for the wake, as Google holds them for
any sleeping phone.

	sudo systemd-run --unit NAME python3 once-no-push.py SECONDS
"""
import importlib.machinery
import importlib.util
import sys
import time

loader = importlib.machinery.SourceFileLoader('sleepd', '/usr/local/sbin/zrn-sleepd')
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)

real_inhibits = sleepd.inhibits
sleepd.inhibits = lambda: [name for name in real_inhibits() if name != 'timed-push']
sleepd.MAX_SLEEP_S = int(sys.argv[1]) if len(sys.argv) > 1 else 60
sleepd.STATE.mkdir(parents=True, exist_ok=True)
if not sleepd.setup():
	sys.exit(1)
if not sleepd.screen_off():
	sleepd.tell_shell('screen off')
	time.sleep(3)
sleepd.log('once (bench, timed-push not counted)')
for attempt in range(3):
	reason, stat = sleepd.cycle(sleepd.ONCE_IGNORES)
	info = dict(sleepd.last_sleep)
	charge = ''
	if info.get('slept') is not None:
		charge = (f' (asleep {info["slept"]:.1f}s, charge {sleepd.mah(info.get("charge_in"))}'
			  f'->{sleepd.mah(info.get("charge_out"))} mAh)')
	sleepd.log(f'once: woke: {sleepd.woke_text(reason, info)} wake_stat={stat:#010x}{charge}')
	if reason not in ('aborted', 'fw-abort', 'alarm-due', 'hub-busy'):
		break
	time.sleep(5)
