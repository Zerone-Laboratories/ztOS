#!/usr/bin/env python3
"""Exercise suspend rollback and the sleep loop's decisions without touching
hardware or system services."""
import errno
import importlib.machinery
import importlib.util
from pathlib import Path
import sys
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
    def exercise(self, failure=None, awake_after_settle=None, busy=False, capacity=None, hub_busy=False):
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
                                MARK=mark, ENABLED=enabled, BATTERY=battery), \
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
            if awake_after_settle or busy or hub_busy or capacity is not None:
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

    def test_low_battery_shortens_the_sleep(self):
        for capacity, alarm in ((50, '+1800'), (10, '+600'), (4, '+120')):
            self.exercise(capacity=capacity)
            self.assertIn(('/sys/class/rtc/rtc0/wakealarm', alarm), self.writes, capacity)


class WakeReasonTest(unittest.TestCase):
    def reason(self, slept, **values):
        with tempfile.TemporaryDirectory() as directory:
            parameters = Path(directory)
            fields = dict(entries='4', wake_stat=str(0x80000000), wake_stat2='0', wake_stat3='0',
                          last_smc='0', grp4_before='0', eint_pending='0,0,0,0,0')
            fields.update(values)
            for name, value in fields.items():
                (parameters / name).write_text(value + '\n')
            with patch.object(sleepd, 'SLEEP', parameters):
                return sleepd.wake_reason(3, slept)[0]

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
    def run_main(self, results, until, push=lambda t: {}, dozing=False, slept=None):
        clock = {'t': 0.0}
        boot = {'extra': 0.0}	# boot time runs on through the sleeps
        calls, statuses, notes, writes = [], [], [], []
        script = list(results)

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
                                 'paused' if sleepd.PAUSED.exists() else None), \
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

    def test_firmware_aborts_pause_without_clear(self):
        calls, statuses, notes, writes, paused = self.run_main(['fw-abort'] * 3 + ['rtc'], 600)
        self.assertEqual(len(calls), 3)		# paused: no fourth try
        for gap in self.gaps(calls):
            self.assertTrue(5 <= gap <= 7, calls)
        # GRP4 never forced clear: only the reset to N at the start
        self.assertEqual([value for path, value in writes if path.endswith('/clear_grp4')], [0])
        self.assertTrue(paused)
        self.assertTrue(any('Sleep paused' in n for n in notes))
        # a firmware abort is not a wake the sensor hub's side should hear of
        self.assertEqual(statuses[-1][1].get('wakes'), 3)

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


if __name__ == '__main__':
    unittest.main()
