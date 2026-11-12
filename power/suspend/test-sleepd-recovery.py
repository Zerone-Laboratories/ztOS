#!/usr/bin/env python3
"""Exercise suspend rollback and the sleep loop's decisions without touching
hardware or system services."""
import errno
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys
import threading
import time
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2] / 'devices/samsung-starlte/overlay/usr/local/sbin/zrn-sleepd'
# its ztos_user, from the tree rather than /usr/lib/ztos/python
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'os/overlay/usr/lib/ztos/python'))
loader = importlib.machinery.SourceFileLoader('sleepd', str(SOURCE))
spec = importlib.util.spec_from_loader(loader.name, loader)
sleepd = importlib.util.module_from_spec(spec)
loader.exec_module(sleepd)


class RecoveryTest(unittest.TestCase):
    def exercise(self, failure=None, awake_after_settle=None, busy=False, capacity=None, hub_busy=False,
                 count_einval=False, watch=None):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            wifi = base / 'wifi'
            wifi.mkdir()
            bound = wifi / sleepd.WIFI_DEV
            bound.touch()
            enabled = base / 'enabled'
            enabled.touch()
            mark = base / 'sleeping'
            writes, commands = [], []
            entered = False
            refused = {'n': 4}
            parameters = base / 'parameters'
            ufs = base / 'ufs'
            battery = base / 'battery'
            if capacity is not None:
                battery.mkdir()
                (battery / 'capacity').write_text(f'{capacity}\n')

            def write(path, value):
                nonlocal entered
                path = str(path)
                writes.append((path, value))
                if path == '/sys/power/wakeup_count' and count_einval:
                    raise OSError(errno.EINVAL, 'Invalid argument')
                if path == '/sys/power/state':
                    if hub_busy:	# zrn-sleep-init refused: host_req high
                        refused['n'] += 1
                        raise OSError(errno.EBUSY, 'Device or resource busy')
                    if busy:
                        raise OSError(errno.EBUSY, 'Device or resource busy')
                    entered = True
                if failure == path and entered:
                    raise OSError('injected recovery failure')
                if path == str(wifi / 'unbind'):
                    bound.unlink()
                if path == str(wifi / 'bind'):
                    bound.touch()

            def run(*args, **kwargs):
                commands.append(args)
                failed = False
                return SimpleNamespace(returncode=int(failed))

            real_read = sleepd.read

            def read(path, default=''):
                if str(path).startswith(str(battery)):
                    return real_read(path, default)
                if path == ufs:
                    return '2'
                if str(path).endswith('/pm_ready'):
                    return 'N' if failure == 'audio' and entered else 'Y'
                if str(path).endswith('last_failed_dev'):
                    return 'alarmtimer.2.auto'
                if str(path).endswith('/hub_refused'):
                    return str(refused['n'])
                return '0'

            with patch.multiple(sleepd, SLEEP=parameters, WIFI=wifi, UFS=ufs,
                                MARK=mark, ENABLED=enabled, BATTERY=battery,
                                INHIBIT=base / 'inhibit'), \
                    patch.object(sleepd, '_watch', watch), \
                    patch.object(sleepd, 'ensure_watch', side_effect=lambda: watch), \
                    patch.object(sleepd, 'wakeup_sources', return_value='none'), \
                    patch.object(sleepd, 'read', side_effect=read), \
                    patch.object(sleepd, 'why_awake', return_value=awake_after_settle), \
                    patch.object(sleepd, 'write', side_effect=write), \
                    patch.object(sleepd, 'run', side_effect=run), \
                    patch.object(sleepd, 'plugged', return_value=False), \
                    patch.object(sleepd, 'background_apps_active', return_value=False), \
                    patch.object(sleepd, 'bt_sleep_mode'), \
                    patch.object(sleepd, 'wake_reason', return_value=('rtc', 0x80000002)), \
                    patch.object(sleepd, 'tell_shell'), patch.object(sleepd, 'log'), \
                    patch.object(sleepd.ztos_user, 'USER', 'test'), \
                    patch.object(sleepd.time, 'sleep'), patch.object(sleepd.os, 'sync'):
                reason, _ = sleepd.cycle()
            self.writes = writes

            self.assertIn((str(wifi / 'bind'), sleepd.WIFI_DEV), writes)
            if awake_after_settle or busy or hub_busy or capacity is not None or count_einval:
                return reason
            self.assertFalse(any(command[0] in ('modprobe', 'rmmod') for command in commands))
            self.assertFalse(any('pipewire.service' in command for command in commands))
            self.assertEqual(reason, 'failed' if failure else 'rtc')
            self.assertEqual(enabled.exists(), not bool(failure))
            self.assertEqual(mark.exists(), bool(failure))

    def test_success_clears_crash_marker(self):
        self.exercise()

    def test_sysfs_failure_still_recovers_wifi_and_audio(self):
        self.exercise('/sys/power/mem_sleep')

    def test_alarm_failure_still_recovers_wifi_and_audio(self):
        self.exercise('/sys/class/rtc/rtc0/wakealarm')

    def test_audio_failure_disables_further_sleep(self):
        self.exercise('audio')

    def test_awake_after_settle_cancels_before_suspend(self):
        # the screen came on during the preparations: no suspend, Wi-Fi back
        self.assertEqual(self.exercise(awake_after_settle='screen on'), 'cancelled')
        self.assertFalse(any(path == '/sys/power/state' for path, _ in self.writes))
        self.assertFalse(any(path == '/sys/power/wakeup_count' for path, _ in self.writes))

    def test_alarm_due_is_a_short_retry(self):
        self.assertEqual(self.exercise(busy=True), 'alarm-due')

    def test_hub_refusal_is_hub_busy(self):
        # EBUSY with hub_refused moved: the hub, not an alarm (last_failed_dev
        # still names an old alarmtimer failure)
        self.assertEqual(self.exercise(hub_busy=True), 'hub-busy')

    def test_wakeup_count_einval_is_cancelled(self):
        # a wakeup event between the count read and its write: cancelled
        # (a short retry), not a failure with a 30-300 s backoff
        self.assertEqual(self.exercise(count_einval=True), 'cancelled')
        self.assertFalse(any(path == '/sys/power/state' for path, _ in self.writes))

    def test_ebusy_after_late_inhibit_is_cancelled(self):
        class Watch:
            def drain(self):
                return True	# an inhibit came after the re-check
        self.assertEqual(self.exercise(busy=True, watch=Watch()), 'cancelled')

    def test_ebusy_without_inhibit_event_is_alarm_due(self):
        class Watch:
            def drain(self):
                return False
        self.assertEqual(self.exercise(busy=True, watch=Watch()), 'alarm-due')

    def test_cycle_writes_clear_grp4_zero_only(self):
        self.exercise()
        values = [value for path, value in self.writes if path.endswith('/clear_grp4')]
        self.assertEqual(values, [0])

    def test_low_battery_shortens_the_sleep(self):
        for capacity, alarm in ((50, '+1800'), (10, '+600'), (4, '+120')):
            self.exercise(capacity=capacity)
            self.assertIn(('/sys/class/rtc/rtc0/wakealarm', alarm), self.writes, capacity)


class WakeReasonTest(unittest.TestCase):
    def reason(self, slept, early_rtc=False, **values):
        with tempfile.TemporaryDirectory() as directory:
            parameters = Path(directory)
            fields = dict(entries='4', wake_stat=str(0x80000000), wake_stat2='0', wake_stat3='0',
                          last_smc='0', grp4_before='0', eint_pending='0,0,0,0,0')
            fields.update(values)
            for name, value in fields.items():
                (parameters / name).write_text(value + '\n')
            with patch.object(sleepd, 'SLEEP', parameters):
                return sleepd.wake_reason(3, slept, early_rtc)[0]

    def test_firmware_abort(self):
        self.assertEqual(self.reason(0.4), 'fw-abort')

    def test_latched_grp4_is_a_firmware_abort(self):
        self.assertEqual(self.reason(0.4, wake_stat=str(0x80000001), grp4_before='1',
                                     eint_pending='0,128,0,0,0'), 'fw-abort')

    def test_long_sleep_without_source_is_other(self):
        self.assertEqual(self.reason(300), 'other')

    def test_rtc_and_key(self):
        self.assertEqual(self.reason(300, wake_stat=str(0x80000002)), 'rtc')
        self.assertEqual(self.reason(0.5, eint_pending='0,0,16,0,0'), 'key')

    def test_not_entered(self):
        self.assertEqual(self.reason(0, entries='3'), 'aborted')

    def test_hub_wake_reason(self):
        # an EINT wake (bit 0) with gpa1-7, the hub's host_req, pending
        self.assertEqual(self.reason(30, wake_stat=str(0x80000001), eint_pending='0,128,0,0,0'), 'hub')

    def test_alarm_reason_uses_t_alarm(self):
        # zrn-sleepd's own alarm: written at 100 for +60, 6 s of preparations
        # after it, the RTC fires at 160: 'rtc' (slept is only 54 s)
        self.assertFalse(sleepd.rtc_early(100.0, 60, 160.4))
        self.assertEqual(self.reason(54, sleepd.rtc_early(100.0, 60, 160.4),
                                     wake_stat=str(0x80000002)), 'rtc')
        # an Android alarm 60 s before ours: 'alarm'
        self.assertTrue(sleepd.rtc_early(100.0, 1800, 1840.0))
        self.assertEqual(self.reason(1734, True, wake_stat=str(0x80000002)), 'alarm')
        # no alarm written (an early failure): never 'alarm'
        self.assertFalse(sleepd.rtc_early(None, 60, 10.0))
        # an early RTC flag means nothing without the RTC's wake bit
        self.assertEqual(self.reason(30, True, wake_stat=str(0x81000000)), 'modem')

    def test_rtc_with_gpa1_pending_is_rtc(self):
        self.assertEqual(self.reason(60, wake_stat=str(0x80000002), eint_pending='0,128,0,0,0'), 'rtc')

    def test_key_before_hub(self):
        self.assertEqual(self.reason(30, wake_stat=str(0x80000001), eint_pending='0,128,16,0,0'), 'key')

    def test_fw_abort_uses_grp4_at_smc_when_present(self):
        # zrn-sleep-init cleared a stale latch after reading grp4_before
        self.assertEqual(self.reason(60, wake_stat=str(0x80000002), grp4_before='1',
                                     grp4_at_smc='0'), 'rtc')
        self.assertEqual(self.reason(0.4, wake_stat=str(0x80000001), grp4_before='1',
                                     grp4_at_smc='1', eint_pending='0,128,0,0,0'), 'fw-abort')
        # an older module: grp4_before alone
        self.assertEqual(self.reason(60, wake_stat=str(0x80000002), grp4_before='1'), 'fw-abort')


class ModemRxTest(unittest.TestCase):
    """Which modem channel a wake carried, from the wakeup sources' counts."""
    def tree(self, base, sources):
        for i, (name, wakeups, events) in enumerate(sources):
            d = base / f'wakeup{i}'
            d.mkdir()
            (d / 'name').write_text(name + '\n')
            (d / 'wakeup_count').write_text(f'{wakeups}\n')
            (d / 'event_count').write_text(f'{events}\n')

    def counts(self, sources):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            self.tree(base, sources)
            with patch.object(sleepd, 'WAKEUP', base):
                return sleepd.modem_counts()

    BEFORE = [('umts_rfs0', 10, 91), ('umts_ipc0', 0, 82), ('umts_router', 0, 975),
              ('rmnet0', 0, 0), ('14170000.mcu_ipc', 0, 0), ('alarmtimer.2.auto', 3, 3)]

    def test_parse_only_modem_sources(self):
        counts = self.counts(self.BEFORE)
        self.assertEqual(counts, {'umts_rfs0': (10, 91), 'umts_ipc0': (0, 82),
                                  'umts_router': (0, 975), 'rmnet0': (0, 0)})

    def test_rfs_only(self):
        before = self.counts(self.BEFORE)
        after = dict(before, umts_rfs0=(20, 101))
        self.assertEqual(sleepd.modem_rx(before, after), 'umts_rfs0 +10')

    def test_router_only(self):
        before = self.counts(self.BEFORE)
        after = dict(before, umts_router=(0, 977))
        self.assertEqual(sleepd.modem_rx(before, after), 'umts_router +2')

    def test_no_delta(self):
        before = self.counts(self.BEFORE)
        self.assertEqual(sleepd.modem_rx(before, dict(before)), '')
        self.assertEqual(sleepd.woke_text('modem', {'modem_rx': ''}), 'modem (no channel rx)')
        self.assertEqual(sleepd.woke_text('rtc', {'modem_rx': ''}), 'rtc')

    def test_missing_source(self):
        before = self.counts(self.BEFORE)
        after = dict(before, umts_ipc0=(1, 85))
        del before['umts_ipc0']		# gone before, back after: not counted
        self.assertEqual(sleepd.modem_rx(before, after), '')
        after2 = dict(self.counts(self.BEFORE))
        del after2['umts_rfs0']
        self.assertEqual(sleepd.modem_rx(self.counts(self.BEFORE), after2), '')

    def test_woke_text(self):
        self.assertEqual(sleepd.woke_text('modem', {'modem_rx': 'umts_rfs0 +10'}), 'modem (umts_rfs0 +10)')
        self.assertEqual(sleepd.woke_text('rtc', {'modem_rx': 'umts_ipc0 +1'}), 'rtc (modem rx: umts_ipc0 +1)')
        self.assertEqual(sleepd.woke_text('aborted', {}), 'aborted')

    def test_cycle_records_the_delta(self):
        # snapshots after the wakeup_count write and a second after the return
        snaps = [{'umts_ipc0': (0, 5), 'umts_router': (0, 9)}, {'umts_ipc0': (1, 7), 'umts_router': (0, 9)}]
        with patch.object(sleepd, 'modem_counts', side_effect=snaps):
            RecoveryTest('test_success_clears_crash_marker').exercise()
        self.assertEqual(sleepd.last_sleep.get('modem_rx'), 'umts_ipc0 +2')


class Stop(Exception):
    pass


class LoopTest(unittest.TestCase):
    """main() on a fake clock: 2 s a loop, cycle() scripted."""
    def run_main(self, results, until, push=lambda t: {}, dozing=False, slept=None, causes=None):
        clock = {'t': 0.0}
        boot = {'extra': 0.0}	# boot time runs on through the sleeps
        calls, statuses, notes, writes = [], [], [], []
        script = list(results)
        cause_script = list(causes or [])

        def sleep(seconds):
            clock['t'] += seconds
            if clock['t'] > until:
                raise Stop()

        def cycle(ignore=()):
            calls.append(clock['t'])
            sleepd.last_dozing = dozing
            sleepd.last_sleep.clear()
            if not script:
                raise Stop()
            reason = script.pop(0)
            if slept:
                boot['extra'] += slept
            sleepd.last_sleep.update(entered=reason not in ('aborted', 'failed', 'cancelled', 'alarm-due', 'hub-busy'),
                                     slept=0.4 if reason == 'fw-abort' else 300.0)
            return reason, 0x80000000

        def kv(path):
            # push(seconds since the first wake), for the doze-window cases
            return push(clock['t'] - calls[0] if calls else -1e6) if path == sleepd.PUSH else {}

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            parameters = base / 'parameters'
            parameters.mkdir()
            (parameters / 'grp4_before').write_text('1\n')
            with patch.multiple(sleepd, SLEEP=parameters, STATE=base, MARK=base / 'sleeping',
                                ENABLED=base / 'enabled', INHIBIT=base / 'inhibit',
                                PAUSED=base / 'paused', STATUS=base / 'status',
                                BATTERY=base / 'battery', LOG=base / 'log'), \
                    patch.object(sleepd, 'why_awake', side_effect=lambda ignore=():
                                 'paused' if sleepd.paused() else None), \
                    patch.object(sleepd, 'fw_abort_cause', side_effect=lambda:
                                 cause_script.pop(0) if cause_script else 'other'), \
                    patch.object(sleepd, 'screen_off', return_value=True), \
                    patch.object(sleepd, 'setup', return_value=True), \
                    patch.object(sleepd, 'cycle', side_effect=cycle), \
                    patch.object(sleepd, 'kv', side_effect=kv), \
                    patch.object(sleepd, 'write', side_effect=lambda path, value: writes.append((str(path), value))), \
                    patch.object(sleepd, 'status', side_effect=lambda **f: statuses.append((clock['t'], f))), \
                    patch.object(sleepd, 'tell_shell', side_effect=notes.append), \
                    patch.object(sleepd, 'log'), \
                    patch.object(sleepd, 'boottime', side_effect=lambda: clock['t'] + boot['extra']), \
                    patch.object(sleepd.time, 'monotonic', side_effect=lambda: clock['t']), \
                    patch.object(sleepd.time, 'time', side_effect=lambda: 1e9 + clock['t']), \
                    patch.object(sleepd.time, 'sleep', side_effect=sleep):
                try:
                    sleepd.main()
                except Stop:
                    pass
                paused = (base / 'paused').exists()
        return calls, statuses, notes, writes, paused

    def gaps(self, calls):
        return [round(b - a) for a, b in zip(calls, calls[1:])]

    def test_backoff_counts_failures_in_a_row(self):
        calls, *_ = self.run_main(['failed', 'failed', 'rtc', 'failed', 'failed'], 2000)
        # fail, fail, ok, fail: 30, 60, the rtc wake's 3 (a loop of 2 s), 30
        gaps = self.gaps(calls)
        self.assertGreaterEqual(len(gaps), 4)
        for gap, want in zip(gaps, (30, 60, 3, 30)):
            self.assertTrue(want <= gap <= want + 2, (gaps, want))

    def test_short_retries_do_not_count(self):
        calls, *_ = self.run_main(['cancelled', 'alarm-due', 'failed', 'rtc'], 2000)
        gaps = self.gaps(calls)
        for gap, want in zip(gaps, (5, 3, 30)):
            self.assertTrue(want <= gap <= want + 2, (gaps, want))

    def test_three_other_aborts_pause_30_min_then_expire(self):
        calls, statuses, notes, writes, paused = self.run_main(['fw-abort'] * 3 + ['rtc'], 600)
        self.assertEqual(len(calls), 3)		# paused: no fourth try in 600 s
        for gap in self.gaps(calls):
            self.assertTrue(5 <= gap <= 7, calls)
        # GRP4 never forced clear: only the reset to N at the start
        self.assertEqual([value for path, value in writes if path.endswith('/clear_grp4')], [0])
        self.assertTrue(paused)
        self.assertEqual(sum('Sleep paused' in n for n in notes), 1)
        # a firmware abort is not a wake the sensor hub's side should hear of
        self.assertEqual(statuses[-1][1].get('wakes'), 3)
        # the pause ends by itself 30 min later, and sleeping goes on
        calls, statuses, notes, writes, paused = self.run_main(['fw-abort'] * 3 + ['rtc', 'rtc'], 2400)
        self.assertGreaterEqual(len(calls), 4, calls)
        self.assertTrue(sleepd.PAUSE_S <= calls[3] - calls[2] <= sleepd.PAUSE_S + 4, calls)
        self.assertFalse(paused)

    def test_hub_abort_backs_off_never_pauses(self):
        calls, statuses, notes, writes, paused = self.run_main(['fw-abort'] * 10, 4000,
                                                               causes=['hub'] * 10)
        gaps = self.gaps(calls)
        self.assertEqual(len(gaps), 10, calls)	# the 11th call ends the script
        for gap in gaps[:5]:
            self.assertTrue(20 <= gap <= 22, gaps)
        for gap in gaps[5:]:
            self.assertTrue(300 <= gap <= 302, gaps)
        self.assertFalse(paused)
        self.assertFalse(any('Sleep paused' in n for n in notes))

    def test_clear_grp4_never_written_one(self):
        causes = ['hub', 'other'] * 5
        *_, writes, paused = self.run_main(['fw-abort'] * 10, 6000, causes=causes)
        values = [value for path, value in writes if path.endswith('/clear_grp4')]
        self.assertNotIn(1, values)
        self.assertEqual(values, [0])
        # and `once`: three attempts, all aborted, classified, never cleared
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            writes, logs = [], []
            with patch.multiple(sleepd, STATE=base, LOG=base / 'log', SLEEP=base, MAX_SLEEP_S=1800),\
                    patch.object(sleepd, 'setup', return_value=True), \
                    patch.object(sleepd, 'ensure_watch'), \
                    patch.object(sleepd, 'screen_off', return_value=True), \
                    patch.object(sleepd, 'cycle', return_value=('fw-abort', 0x80000000)) as cycle, \
                    patch.object(sleepd, 'fw_abort_cause', return_value='hub'), \
                    patch.object(sleepd, 'write', side_effect=lambda path, value: writes.append((str(path), value))), \
                    patch.object(sleepd, 'log', side_effect=lambda *w: logs.append(' '.join(map(str, w)))), \
                    patch.object(sleepd.time, 'sleep'):
                sleepd.once(['60'])
            self.assertEqual(cycle.call_count, 3)
            self.assertNotIn(1, [value for path, value in writes if path.endswith('/clear_grp4')])
            self.assertEqual(sum('cause hub' in line for line in logs), 3)

    def test_alarm_wake_awake_20s_and_doze_window(self):
        calls, *_ = self.run_main(['alarm', 'rtc'], 200)
        gaps = self.gaps(calls)
        self.assertTrue(20 <= gaps[0] <= 22, gaps)
        calls, statuses, *_ = self.run_main(['alarm', 'rtc'], 200, dozing=True,
                                            push=lambda t: dict(identity='known', state='connecting', since='0'))
        woke = calls[0]
        self.assertTrue(any(t > woke and f.get('why') == 'doze window' for t, f in statuses))

    def test_fw_abort_never_writes_clear_grp4(self):
        *_, writes, paused = self.run_main(['fw-abort', 'rtc', 'fw-abort', 'rtc'], 600)
        self.assertNotIn(1, [value for path, value in writes if path.endswith('/clear_grp4')])
        self.assertFalse(paused)

    def test_clear_grp4_reset_at_start(self):
        calls, statuses, notes, writes, paused = self.run_main(['rtc'], 60)
        self.assertEqual(writes[0][1], 0)
        self.assertTrue(writes[0][0].endswith('/clear_grp4'))

    def test_setup_resets_clear_grp4(self):
        with tempfile.TemporaryDirectory() as directory:
            parameters = Path(directory)
            (parameters / 'enable_deep').write_text('Y\n')
            writes = []
            with patch.object(sleepd, 'SLEEP', parameters), \
                    patch.object(sleepd, 'read', side_effect=lambda path, default='':
                                 'Y' if str(path).endswith('enable_deep') else 's2idle [deep]'), \
                    patch.object(sleepd, 'write', side_effect=lambda path, value: writes.append((str(path), value))):
                self.assertTrue(sleepd.setup())
            self.assertEqual(writes, [(str(parameters / 'clear_grp4'), 0)])

    def test_hub_busy_is_a_short_retry(self):
        calls, *_ = self.run_main(['hub-busy', 'rtc', 'rtc'], 600)
        gaps = self.gaps(calls)
        self.assertTrue(5 <= gaps[0] <= 7, gaps)

    def test_hub_backoff_after_three_in_120s(self):
        calls, *_ = self.run_main(['hub', 'hub-busy', 'hub', 'rtc', 'rtc'], 2000)
        gaps = self.gaps(calls)
        self.assertGreaterEqual(len(gaps), 4, gaps)
        # 5 s after each of the first two, then left awake 120 s, then as usual
        for gap, want in zip(gaps, (5, 5, 120, 3)):
            self.assertTrue(want <= gap <= want + 2, (gaps, want))

    def test_hub_wakes_far_apart_do_not_back_off(self):
        # 300 s asleep between them (boot time): never 3 within 120 s
        calls, *_ = self.run_main(['hub', 'hub', 'hub', 'rtc'], 2000, slept=300.0)
        for gap in self.gaps(calls)[:3]:
            self.assertTrue(5 <= gap <= 7, calls)

    def window_end(self, push):
        """When a doze window after an RTC wake at t=0 of the loop ended."""
        calls, statuses, *_ = self.run_main(['rtc', 'rtc'], 200, push=push, dozing=True)
        woke = calls[0]
        for t, fields in statuses:
            if t > woke and fields.get('why') != 'doze window':
                return t - woke
        return None

    def test_doze_window_ends(self):
        woke = {}

        def wall(t):	# the wall clock t s after the first wake
            return 1e9 + woke['at'] + t
        real_window_end = self.window_end

        def window_end(push):
            # the first wake comes at SCREEN_OFF_S or so: measure it first
            calls, *_ = self.run_main(['rtc'], 60)
            woke['at'] = calls[0]
            return real_window_end(push)
        # online before the sleep, the connection lived through it: +8
        before = window_end(lambda t: dict(identity='known', state='online', since=str(int(wall(-100)))))
        # never online, no network: +15
        never = window_end(lambda t: dict(identity='known', state='offline', since='0'))
        # logs in 7 s after the wake: +7 and PUSH_GRACE_S
        later = window_end(lambda t: dict(identity='known', state='online', since=str(int(wall(7))))
                                if t >= 7 else dict(identity='known', state='connecting', since='0'))
        self.assertTrue(8 <= before <= 10, before)
        self.assertTrue(15 <= never <= 17, never)
        self.assertTrue(15 <= later <= 17, later)


class InhibitWatchTest(unittest.TestCase):
    """The inhibit watch on a real directory (inotify; EPOLLWAKEUP needs
    CAP_BLOCK_SUSPEND, so armed() is tested on fdinfo text)."""
    def test_inhibit_watch_sees_rename_and_touch(self):
        with tempfile.TemporaryDirectory() as directory:
            inhibit = Path(directory) / 'inhibit'
            inhibit.mkdir()
            watch = sleepd.InhibitWatch(inhibit)
            try:
                self.assertFalse(watch.drain())
                tmp = inhibit / '.r2.tmp'
                tmp.write_text('')
                watch.drain()		# the temporary file itself
                os.utime(tmp, (time.time() + 60, time.time() + 60))
                os.replace(tmp, inhibit / 'timed-r2')
                self.assertTrue(watch.drain())
                os.utime(inhibit / 'timed-r2', (time.time() + 90, time.time() + 90))
                self.assertTrue(watch.drain())
                (inhibit / 'timed-r2').unlink()		# a release is not an event
                self.assertFalse(watch.drain())
                self.assertFalse(watch.drain())
                self.assertFalse(watch.stale())
            finally:
                watch.close()

    def test_inhibit_watch_rearms_after_rmdir(self):
        with tempfile.TemporaryDirectory() as directory:
            inhibit = Path(directory) / 'inhibit'
            inhibit.mkdir()
            with patch.object(sleepd, 'INHIBIT', inhibit), patch.object(sleepd, '_watch', None), \
                    patch.object(sleepd, 'log') as log:
                first = sleepd.ensure_watch()
                self.assertIsNotNone(first)
                self.assertFalse((inhibit / '.watch-selftest').exists())
                inhibit.rmdir()
                inhibit.mkdir()
                first.drain()
                self.assertTrue(first.stale())
                second = sleepd.ensure_watch()
                self.assertIsNot(second, first)
                tmp = inhibit / '.x.tmp'
                tmp.write_text('')
                second.drain()
                os.replace(tmp, inhibit / 'timed-x')
                self.assertTrue(second.drain())
                self.assertIs(sleepd.ensure_watch(), second)	# not stale: kept
                second.close()
                lines = [' '.join(map(str, c.args)) for c in log.call_args_list]
                # unprivileged here: the kernel strips EPOLLWAKEUP, and it says so
                if os.geteuid() != 0:
                    self.assertTrue(any('not armed' in l for l in lines), lines)

    def test_armed_reads_fdinfo(self):
        with tempfile.TemporaryDirectory() as directory:
            watch = sleepd.InhibitWatch(directory)
            try:
                fd = watch.fd
                line = 'pos:\t0\nflags:\t02000002\nmnt_id:\t16\nino:\t1057\n'
                armed = line + f'tfd: {fd:8d} events: 20000019 data: {fd:16x}  pos:0 ino:4ad sdev:f\n'
                stripped = line + f'tfd: {fd:8d} events:       19 data: {fd:16x}  pos:0 ino:4ad sdev:f\n'
                other = line + f'tfd: {fd + 1:8d} events: 20000019 data: 0  pos:0 ino:4ad sdev:f\n'
                self.assertTrue(watch.armed(armed))
                self.assertFalse(watch.armed(stripped))
                self.assertFalse(watch.armed(other))
            finally:
                watch.close()
        with tempfile.TemporaryDirectory() as directory:
            inhibit = Path(directory)
            with patch.object(sleepd, 'INHIBIT', inhibit), patch.object(sleepd, '_watch', None), \
                    patch.object(sleepd.InhibitWatch, 'armed', return_value=True), \
                    patch.object(sleepd, 'log') as log:
                sleepd.ensure_watch().close()
            self.assertEqual(log.call_args_list[-1].args, ('inhibit watch armed',))
            with patch.object(sleepd, 'INHIBIT', inhibit), patch.object(sleepd, '_watch', None), \
                    patch.object(sleepd.InhibitWatch, 'armed', return_value=False), \
                    patch.object(sleepd, 'log') as log:
                sleepd.ensure_watch().close()
            self.assertIn('not armed', log.call_args_list[-1].args[0])


class CountReadTest(unittest.TestCase):
    def test_count_read_does_not_hang_on_own_event(self):
        # the kernel's read waits while our own watch's source is active:
        # it returns once drain() lets the source go
        released = threading.Event()

        class Watch:
            drains = 0

            def drain(self):
                Watch.drains += 1
                if Watch.drains >= 2:	# the first is before the read starts
                    released.set()
                return False

        def read(path, default=''):
            if str(path) == '/sys/power/wakeup_count':
                released.wait(10)
                return '1234' if released.is_set() else ''
            return default
        with patch.object(sleepd, '_watch', Watch()), patch.object(sleepd, 'read', side_effect=read):
            start = time.monotonic()
            self.assertEqual(sleepd.read_wakeup_count(10), '1234')
            self.assertLess(time.monotonic() - start, 3)

    def test_count_read_held_by_another_source_stalls(self):
        def read(path, default=''):
            time.sleep(5)
            return '1'
        with patch.object(sleepd, '_watch', None), patch.object(sleepd, 'read', side_effect=read):
            with self.assertRaises(sleepd.Stalled) as stalled:
                sleepd.read_wakeup_count(2)
            self.assertEqual(stalled.exception.seconds, 2)


class FwCauseTest(unittest.TestCase):
    def cause(self, eint_before='0,0,0,0,0', eint_pending='0,0,0,0,0', inhibits=(), ready='1'):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            parameters = base / 'parameters'
            parameters.mkdir()
            (parameters / 'eint_before').write_text(eint_before + '\n')
            (parameters / 'eint_pending').write_text(eint_pending + '\n')
            inhibit = base / 'inhibit'
            inhibit.mkdir()
            for name in inhibits:
                (inhibit / name).touch()
            (base / 'ready').write_text(ready + '\n')
            with patch.multiple(sleepd, SLEEP=parameters, INHIBIT=inhibit, SENSORHUB_READY=base / 'ready'):
                return sleepd.fw_abort_cause()

    def test_masked_gpa1_bit0_alone_is_other(self):
        self.assertEqual(self.cause(eint_before='0,1,0,0,0'), 'other')
        self.assertEqual(self.cause(eint_pending='0,1,0,0,0'), 'other')
        self.assertEqual(self.cause(), 'other')

    def test_host_req_inhibit_or_not_ready_is_hub(self):
        self.assertEqual(self.cause(eint_before='0,128,0,0,0'), 'hub')
        self.assertEqual(self.cause(eint_pending='0,129,0,0,0'), 'hub')
        self.assertEqual(self.cause(inhibits=('timed-gnss',)), 'hub')
        self.assertEqual(self.cause(inhibits=('timed-hub',)), 'hub')
        self.assertEqual(self.cause(inhibits=('timed-push',)), 'other')
        self.assertEqual(self.cause(ready='0'), 'hub')


class LogTest(unittest.TestCase):
    def test_repair_log_marks_nul_run_once(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'log'
            before = b'2026-10-30 01:00:00 woke: rtc\n2026-10-30 01:05:00 wo' + b'\0' * 64 + \
                b'2026-10-30 08:00:00 backend up\n'
            log.write_bytes(before)
            with patch.object(sleepd, 'LOG', log):
                sleepd.repair_log()
                after = log.read_bytes()
                self.assertNotIn(b'\0', after)
                self.assertEqual(after.count(b'-- 64 bytes lost: an unclean stop --'), 1)
                self.assertEqual(after.decode().splitlines(), [
                    '2026-10-30 01:00:00 woke: rtc', '2026-10-30 01:05:00 wo',
                    '-- 64 bytes lost: an unclean stop --', '2026-10-30 08:00:00 backend up'])
                self.assertEqual((Path(directory) / 'log.nul').read_bytes(), before)
                sleepd.repair_log()		# idempotent
                self.assertEqual(log.read_bytes(), after)
                # a later reset: a new marker, log.nul keeps the first evidence
                log.write_bytes(after + b'\0' * 8 + b'x\n')
                sleepd.repair_log()
                self.assertEqual(log.read_bytes().count(b'bytes lost'), 2)
                self.assertEqual((Path(directory) / 'log.nul').read_bytes(), before)

    def test_log_rotates_at_1_mib(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'log'
            log.write_bytes(b'x' * (sleepd.LOG_ROTATE_BYTES + 1))
            with patch.object(sleepd, 'LOG', log), patch('builtins.print'):
                sleepd.log('after the rotation')
            self.assertEqual((Path(directory) / 'log.1').stat().st_size, sleepd.LOG_ROTATE_BYTES + 1)
            self.assertTrue(log.read_text().endswith('after the rotation\n'))
            self.assertEqual(len(log.read_text().splitlines()), 1)

    def test_log_command_strips_nul(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / 'log'
            (Path(directory) / 'log.1').write_bytes(b'old 1\nold 2\n')
            log.write_bytes(b'new 1\n\0\0\0new 2\nnew 3\n')
            printed = []
            with patch.object(sleepd, 'LOG', log), \
                    patch('builtins.print', side_effect=lambda *a, **k: printed.append(' '.join(a))):
                sleepd.show_log(3)
                self.assertEqual(printed, ['new 1', 'new 2', 'new 3'])
                printed.clear()
                sleepd.show_log(None)
                self.assertEqual(printed, ['old 1', 'old 2', 'new 1', 'new 2', 'new 3'])


if __name__ == '__main__':
    unittest.main()
