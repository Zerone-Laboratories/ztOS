#!/usr/bin/env python3
"""Timed device/s2idle validation. Run as root with the display already off.

Logs before/after firmware counters and wake evidence. Wi-Fi is detached
because BCM4361's current firmware does not acknowledge its D3 handshake.
The original storage PM level and Wi-Fi binding are restored after wake.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--sicd', action='store_true')
p.add_argument('--cpd', action='store_true')
p.add_argument('--allow-mif-sleep', action='store_true')
p.add_argument('--allow-unready-sensorhub', action='store_true',
               help='allow running when sensorhub was not warm-booted from TWRP')
p.add_argument('--allow-ap-only', action='store_true',
               help='allow deep sleep verification to succeed on sleep_ap even if sleep_mif does not advance')
p.add_argument('--acpm-fvp-tool', type=Path,
               help='opt in to Samsung MIF/INT FVP suspend handshake using the corrected s9p-acpm tool')
p.add_argument('--mif-suspend-khz', type=int, choices=[421000, 1014000],
               help='MIF frequency before sleep; stock logs Suspend_frequency 421000; needs verified restore frequency')
p.add_argument('--mif-restore-khz', type=int, choices=[1794000],
               help='explicit pre-test MIF frequency verified by hardware clock audit')
p.add_argument('--int-suspend-khz', type=int, choices=[107000],
               help='explicit audited Samsung INT suspend frequency')
p.add_argument('--int-restore-khz', type=int, choices=[534000],
               help='explicit pre-test INT frequency verified by hardware clock audit')
p.add_argument('--pcie-recovery-module', type=Path,
               help='temporary recovery of detached WLAN with the old PCIe host lacking resume callbacks')
p.add_argument('--wait-unplug', type=int, default=0, metavar='SECONDS')
p.add_argument('--sicd-cpu', type=int, choices=range(4))
p.add_argument('--battery-only', action='store_true')
p.add_argument('--connected-diagnostic', action='store_true',
               help='explicitly allow a bounded wake-mask trial with USB attached')
p.add_argument('--ignore-host-req-wake', action='store_true',
               help='temporarily mask BCM4773 IRQ 105 wake for this bounded trial')
p.add_argument('--seconds', type=int, default=20, choices=range(10, 121))
p.add_argument('--pm-test', choices=['none', 'devices', 'processors', 'core'], default='none')
p.add_argument('--mem-sleep', choices=['s2idle', 'deep'], default='s2idle')
p.add_argument('--wake-source', choices=['mct', 'rtc', 'pmic'], default='mct',
               help='pmic: S2MPS18 RTC alarm 0 through EINT gpa0-1, as stock wakes; '
                    'the SoC RTC is left disarmed (stock masks it in WAKEUP_MASK)')
p.add_argument('--pmic-rtc-tool', type=Path,
               help='pmic-rtc-alarm.py, required by --wake-source pmic')
p.add_argument('--stock-devfreq', action='store_true',
               help="stock suspend_late order: fsys0, cam, disp, intcam to their suspend "
                    "frequencies, then INT release+frequency, then MIF release+frequency")
p.add_argument('--validate-only', action='store_true',
               help='validate arguments and exit without accessing hardware')
a = p.parse_args()
if a.allow_ap_only and a.allow_mif_sleep:
    p.error('--allow-ap-only and --allow-mif-sleep select different verification targets')
if a.acpm_fvp_tool and a.mem_sleep != 'deep':
    p.error('--acpm-fvp-tool requires the guarded platform deep-sleep backend')
if bool(a.mif_suspend_khz) != bool(a.mif_restore_khz):
    p.error('MIF frequency trial requires both suspend and verified restore frequencies')
if a.mif_suspend_khz and not a.acpm_fvp_tool:
    p.error('MIF frequency trial requires the corrected ACPM transport')
if bool(a.int_suspend_khz) != bool(a.int_restore_khz):
    p.error('INT frequency trial requires both suspend and verified restore frequencies')
if a.int_suspend_khz and not a.mif_suspend_khz:
    p.error('INT frequency trial requires the audited MIF frequency trial')
if a.allow_ap_only and a.mem_sleep != 'deep':
    p.error('--allow-ap-only requires platform deep sleep')
if a.mem_sleep == 'deep' and a.pm_test == 'none' and not (a.allow_ap_only or a.allow_mif_sleep):
    p.error('select --allow-ap-only diagnostic or --allow-mif-sleep verification explicitly')
if a.allow_mif_sleep and a.mem_sleep != 'deep':
    p.error('MIF sleep requires the gated platform deep-sleep backend')
if a.allow_mif_sleep and not (
        (a.mem_sleep == 'deep' or (a.sicd and a.cpd)) and a.pm_test == 'none'):
    p.error('--allow-mif-sleep requires deep or SICD/CPD and an actual suspend trial')
if a.mem_sleep == 'deep' and a.sicd:
    p.error('deep uses the platform backend; --sicd applies to s2idle')
if a.mem_sleep == 'deep' and a.pm_test == 'none' and a.wake_source not in ('rtc', 'pmic'):
    p.error('a deep-sleep trial requires the independent RTC wake source')
if a.wake_source == 'pmic' and not (a.pmic_rtc_tool and a.mem_sleep == 'deep'):
    p.error('--wake-source pmic needs --pmic-rtc-tool and the platform deep backend')
if a.stock_devfreq and not (a.mif_suspend_khz and a.int_suspend_khz):
    p.error('--stock-devfreq needs the MIF and INT suspend frequencies')
if a.connected_diagnostic and a.battery_only:
    p.error('--connected-diagnostic cannot claim battery-only validation')
if a.ignore_host_req_wake and not (
        ((a.battery_only and a.wait_unplug) or a.connected_diagnostic) and
        a.pm_test == 'none' and a.seconds <= 30):
    p.error('--ignore-host-req-wake requires an explicit connected diagnostic '
            'or battery-only unplugged trial of 30 seconds or less')
if a.validate_only:
    print(json.dumps({'phase': 'arguments_valid', 'options': vars(a)}, default=str))
    raise SystemExit(0)
if a.wait_unplug:
    deadline = time.monotonic() + a.wait_unplug
    print(json.dumps({'phase': 'waiting_for_unplug', 'timeout': a.wait_unplug}), flush=True)
    while any(x.read_text().strip() == '1' for x in Path('/sys/class/power_supply').glob('*/online')):
        if time.monotonic() >= deadline:
            raise TimeoutError('Cable still connected; no suspend attempted')
        time.sleep(1)
    # Unplug itself raises the charger/GPIO wake line. Let that transition
    # and its workqueues settle before beginning a suspend that must be
    # woken by the independent MCT comparator.
    time.sleep(15)
if a.battery_only and any(x.read_text().strip() == '1' for x in Path('/sys/class/power_supply').glob('*/online')):
    raise RuntimeError('Battery-only test requires the cable disconnected')
P = Path('/sys/module/zrn_cpuidle/parameters')
U = Path('/sys/devices/platform/soc@0/11120000.ufs/spm_lvl')
W = Path('/sys/bus/pci/drivers/brcmfmac')
DEV = '0000:01:00.0'

def write(path, value):
    Path(path).write_text(str(value))

def snapshot(label, wakeup_baseline=None):
    info = {'phase': label, 'wall': time.time(), 'monotonic': time.monotonic(), 'mif_requested': a.allow_mif_sleep}
    info['acpm_fvp_domains'] = list(fvp_domains)
    info['suspend_frequencies_khz'] = {'mif': a.mif_suspend_khz, 'int': a.int_suspend_khz}
    ready = Path('/sys/class/sec/sensorhub/ready')
    info['sensorhub_ready'] = ready.read_text().strip() if ready.exists() else 'missing'
    info['host_req_wake_masked'] = host_req_wake_masked
    info['online_cpus'] = Path('/sys/devices/system/cpu/online').read_text().strip()
    mod_name = os.environ.get('ZRN_SYSTEM_SLEEP_MODULE')
    if not mod_name:
        mod_name = 'zrn_sleep_latch' if Path('/sys/module/zrn_sleep_latch/parameters').exists() else 'zrn_system_sleep'
    system_sleep = Path('/sys/module') / mod_name / 'parameters'
    if system_sleep.exists():
        info['system_sleep'] = {name: (system_sleep / name).read_text().strip()
            for name in ['attempts', 'entries', 'last_error', 'last_smc',
                         'last_restore_error', 'dry_run', 'wake_stat', 'wake_stat2', 'wake_stat3', 'bid_pending', 'eint_pending', 'mif_sleep', 'mif_gate_before', 'mif_requests_before']}
    for name in ('pmic_pend_after', 'pmic_refused', 'eint_effective'):
        if (system_sleep / name).exists():
            info.setdefault('system_sleep', {})[name] = (system_sleep / name).read_text().strip()
    rtc_fires = Path('/sys/module/zrn_exynos_rtc/parameters/alarm_fires')
    if rtc_fires.exists():
        info['rtc_alarm_fires'] = rtc_fires.read_text().strip()
    info['external_power'] = any(x.read_text().strip() == '1'
        for x in Path('/sys/class/power_supply').glob('*/online'))
    info['battery'] = {}
    for supply in Path('/sys/class/power_supply').glob('*'):
        if (supply / 'type').read_text().strip() == 'Battery':
            for name in ['current_now', 'current_avg', 'capacity', 'temp', 'voltage_now']:
                if (supply / name).exists():
                    info['battery'][name] = (supply / name).read_text().strip()
    for name in ['attempts', 'entries', 'suspend_entries', 'last_error',
                 'last_sleep_us', 'last_block', 'alarm_fires', 'mct_late_events']:
        info[name] = (P / name).read_text().strip()
    for name in ['cpd_attempts', 'cpd_entries', 'cpd_last_error', 'last_smc_result',
                 'last_trial_us', 'max_trial_us', 'last_core']:
        if (P / name).exists():
            info[name] = (P / name).read_text().strip()
    info['suspend_stats'] = {x.name: x.read_text().strip()
        for x in Path('/sys/power/suspend_stats').glob('*') if x.is_file()}
    wake_irq = Path('/sys/power/pm_wakeup_irq')
    if wake_irq.exists():
        try:
            info['pm_wakeup_irq'] = wake_irq.read_text().strip()
        except OSError as exc:
            # Some kernels expose this node but return ENODATA when no IRQ
            # wake source was recorded. Preserve the
            # rest of the after-snapshot instead of aborting the trial.
            info['pm_wakeup_irq'] = 'unavailable'
            info['pm_wakeup_irq_error'] = str(exc)
    irq_path = Path('/proc/interrupts')
    if irq_path.exists():
        irq_lines = irq_path.read_text().splitlines()
        cpu_count = len(irq_lines[0].split()) if irq_lines else 0
        irq_counts = {}
        for line in irq_lines[1:]:
            fields = line.split()
            if fields and fields[0] in ('105:', '108:'):
                irq_counts[fields[0][:-1]] = [int(x) for x in fields[1:1 + cpu_count]]
        info['irq_counts'] = irq_counts
        if wakeup_baseline is not None:
            info['irq_delta'] = {
                irq: [after - before for after, before in zip(
                    counts, wakeup_baseline.get(f"irq-{irq}", counts))]
                for irq, counts in irq_counts.items()}
    sources = Path('/sys/kernel/debug/wakeup_sources')
    if sources.exists():
        lines = sources.read_text().splitlines()
        info['wakeup_sources'] = {}
        for line in lines[1:]:
            columns = line.split()
            if len(columns) > 3:
                info['wakeup_sources'][columns[0]] = {
                    'events': int(columns[2]), 'wakes': int(columns[3])}
        if wakeup_baseline is not None:
            info['wakeup_delta'] = {
                name: {'events': counts['events'] - wakeup_baseline.get(name, {}).get('events', 0),
                       'wakes': counts['wakes'] - wakeup_baseline.get(name, {}).get('wakes', 0)}
                for name, counts in info['wakeup_sources'].items()
                if counts['events'] != wakeup_baseline.get(name, {}).get('events', 0)
                or counts['wakes'] != wakeup_baseline.get(name, {}).get('wakes', 0)}
    print(json.dumps(info), flush=True)
    firmware = subprocess.check_output(
        ['python3', str(Path(__file__).with_name('firmware-state.py'))], text=True)
    print(firmware.strip(), flush=True)
    info['firmware'] = json.loads(firmware)
    return info

assert all('disabled' in x.read_text() for x in Path('/sys/class/drm').glob('card*-*/enabled')), 'Display must be off'
# Screen-off hub reconfiguration can briefly drop ready. Require a stable
# ready interval without restarting the hub or beginning hardware changes.
ready_path = Path('/sys/class/sec/sensorhub/ready')
ready_deadline = time.monotonic() + 15
ready_samples = 0
if not a.allow_unready_sensorhub:
    while ready_samples < 4 and time.monotonic() < ready_deadline:
        ready_samples = ready_samples + 1 if ready_path.read_text().strip() == '1' else 0
        time.sleep(0.25)
    assert ready_samples == 4, 'Sensor hub did not become ready; preserve the TWRP warm-boot requirement'
else:
    ready_samples = 4 if ready_path.read_text().strip() == '1' else 0
old_level = U.read_text().strip()
mem_sleep_path = Path('/sys/power/mem_sleep')
old_mem_sleep = next(x[1:-1] for x in mem_sleep_path.read_text().split()
                     if x.startswith('['))
hotplug_suspend = Path('/sys/module/zrn_hotplug/parameters/suspend_cluster_off')
old_hotplug_suspend = hotplug_suspend.read_text().strip() if hotplug_suspend.exists() else None
old_sicd = (P / 's2idle_sicd').read_text().strip()
old_cpd = (P / 's2idle_cpd').read_text().strip() if (P / 's2idle_cpd').exists() else None
old_cpu = (P / 'sicd_cpu').read_text().strip() if (P / 'sicd_cpu').exists() else None
bound = (W / DEV).exists()
host_req_wake_masked = False
fvp_domains = []
# Stock DT (stock/twrp-live.dts:9159-9345): only mif and int are use_acpm;
# the rest go through cal_dfs_set_rate -> ACPM FREQ_REQ (acpm_dvfs.c:21).
# ACPM DVFS ids (acpm_dvfs_exynos9810.h): intcam 5, fsys0 6, cam 7, disp 8.
# Suspend kHz = stock log twrp-stock-sleep.log:108-112 (= each DT minimum);
# restore = DT freq_info[0] unless FREQ_GET answers.
EXTRA_DEVFREQ = [('fsys0', 6, 73000, 336000), ('cam', 7, 590000, 690000),
                 ('disp', 8, 200000, 640000), ('intcam', 5, 630000, 690000)]
extra_changed = []
pmic_armed = False
mif_frequency_changed = False
int_frequency_changed = False
try:
    if a.mem_sleep == 'deep':
        if old_hotplug_suspend is None:
            raise RuntimeError('deep needs the suspend-aware CPU hotplug driver')
        write(hotplug_suspend, 1)
    write(mem_sleep_path, a.mem_sleep)
    if bound:
        subprocess.run(['ip', 'link', 'set', 'wlp1s0', 'down'], check=True)
        write(W / 'unbind', DEV)
    write(U, 5)
    write('/sys/power/pm_test', a.pm_test)
    write(P / 's2idle_sicd', int(a.sicd))
    if old_cpd is not None:
        write(P / 's2idle_cpd', int(a.cpd))
    if a.sicd_cpu is not None:
        write(P / 'sicd_cpu', a.sicd_cpu)
    # The backend holds MIF_en=0 until its saved-context SMC handoff.
    if a.mem_sleep == 'deep':
        gate_param = Path('/sys/module') / os.environ.get('ZRN_SYSTEM_SLEEP_MODULE', 'zrn_system_sleep') / 'parameters/mif_sleep'
        if not gate_param.exists():
            raise RuntimeError('Deep sleep requires the guarded MIF backend')
        write(gate_param, int(a.allow_mif_sleep))
    if a.ignore_host_req_wake:
        mask_module = Path(__file__).with_name('zrn_hostreq_wake_test.ko')
        if not mask_module.exists():
            raise FileNotFoundError(f'Missing bounded wake-mask module: {mask_module}')
        subprocess.run(['insmod', str(mask_module)], check=True)
        host_req_wake_masked = True
    if a.acpm_fvp_tool:
        if not a.acpm_fvp_tool.is_file():
            raise FileNotFoundError(a.acpm_fvp_tool)
        # Stock devfreq suspend_late: DATA_INIT=5, RELEASE=2. Keep the
        # firmware MIF gate disabled during preparation; only the saved CPU
        # finisher may enable it. Include a possibly timed-out message in
        # rollback because firmware may have applied it without an ACK.
        def acpm(*args):
            subprocess.run(['python3', str(a.acpm_fvp_tool), *map(str, args)],
                           check=True, timeout=5)
        if a.stock_devfreq:
            for name, dom, khz, restore in EXTRA_DEVFREQ:
                got = subprocess.run(['python3', str(a.acpm_fvp_tool), 'dvfs', str(dom)],
                                     capture_output=True, text=True, timeout=5)
                words = got.stdout.split()
                if got.returncode == 0 and 'kHz' in words:
                    restore = int(words[words.index('kHz') - 1])
                extra_changed.append((name, dom, restore))
                acpm('dvfs', dom, khz)
            # exynos-devfreq.c:1947-1976 per domain, INT's suspend_late first
            fvp_domains.append(1)
            acpm('ipc', 1, 1, 0, 5, 2)
            int_frequency_changed = True
            acpm('dvfs', 'int', a.int_suspend_khz)
            fvp_domains.append(0)
            acpm('ipc', 1, 0, 0, 5, 2)
            mif_frequency_changed = True
            acpm('dvfs', 'mif', a.mif_suspend_khz)
        else:
            for domain in (0, 1):  # DEVFREQ_MIF, DEVFREQ_INT
                fvp_domains.append(domain)
                acpm('ipc', 1, domain, 0, 5, 2)
            if a.mif_suspend_khz:
                mif_frequency_changed = True
                acpm('dvfs', 'mif', a.mif_suspend_khz)
            if a.int_suspend_khz:
                int_frequency_changed = True
                acpm('dvfs', 'int', a.int_suspend_khz)
    before = snapshot('before')
    if a.wake_source == 'rtc':
        rtc_alarm = Path('/sys/class/rtc/rtc0/wakealarm')
        if not rtc_alarm.exists() or 'rtc_alarm_fires' not in before:
            raise RuntimeError('RTC wake source is not ready')
        write(rtc_alarm, 0)
        write(rtc_alarm, f'+{a.seconds}')
        # Independent fallback for s2idle diagnostics, not a deep-sleep wake.
        if os.environ.get('NO_MCT_BACKSTOP') != '1': write(P / 'alarm_seconds', a.seconds + 10)
    elif a.wake_source == 'pmic':
        # stock arms no SoC RTC alarm and masks its wake bit; no MCT fallback
        write('/sys/class/rtc/rtc0/wakealarm', 0)
        write(P / 'alarm_seconds', 0)
        pmic_armed = True
        subprocess.run(['python3', str(a.pmic_rtc_tool), 'arm', str(a.seconds)],
                       check=True, timeout=120)  # ~10 s awake, far slower at suspend clocks
    else:
        write(P / 'alarm_seconds', a.seconds)
    try:
        write('/sys/power/state', 'mem')
    finally:
        after = snapshot('after', {**before.get('wakeup_sources', {}),
                                   **{f"irq-{irq}": counts for irq, counts in
                                      before.get('irq_counts', {}).items()}})
    if a.wake_source == 'pmic':
        ss = after.get('system_sleep', {})
        if a.pm_test == 'none' and not (int(ss.get('wake_stat', '0'), 0) & 1 and
                                        int(ss.get('pmic_pend_after', '0'), 0) & 2):
            raise RuntimeError('Suspend returned without the PMIC EINT wake; '
                               f"wake_stat={ss.get('wake_stat')} pmic_pend={ss.get('pmic_pend_after')}")
    alarm_field = 'rtc_alarm_fires' if a.wake_source == 'rtc' else 'alarm_fires'
    if a.wake_source != 'pmic' and a.pm_test == 'none' and int(after[alarm_field]) <= int(before[alarm_field]):
        raise RuntimeError(f'Suspend returned without the {a.wake_source} wake alarm; '
                           f"last wake IRQ={after.get('pm_wakeup_irq', 'unknown')}")
    if ready_samples == 4 and after['sensorhub_ready'] != '1':
        raise RuntimeError('Sensor hub did not return ready after suspend; '
                           'the required TWRP hot reboot may be needed')
    if a.sicd and a.pm_test == 'none':
        if int(after['suspend_entries']) <= int(before['suspend_entries']):
            raise RuntimeError(
                'SICD did not enter: '
                f"attempts_delta={int(after['attempts']) - int(before['attempts'])}, "
                f"cpu_suspend={after['last_error']}, "
                f"raw_smc={after.get('last_smc_result', 'unavailable')}, "
                f"last_gate={after['last_block']}; timed wake alone is not deep sleep")
        if after['firmware']['sicd_ap'][1] <= before['firmware']['sicd_ap'][1]:
            raise RuntimeError('SICD software entry was not confirmed by the '
                               'firmware AP sequence-down counter')
    if a.mem_sleep == 'deep' and after['system_sleep']['last_restore_error'] != '0':
        raise RuntimeError('Platform register restore reported an error')
    if a.mem_sleep == 'deep' and a.pm_test == 'none':
        if (int(after['system_sleep']['entries']) <= int(before['system_sleep']['entries'])
                or after['system_sleep']['last_restore_error'] != '0'):
            raise RuntimeError('Platform sleep did not complete a clean entry and restore')
        required_counters = ['sleep_ap'] if a.allow_ap_only else ['sleep_ap', 'sleep_mif']
        for counter in required_counters:
            if after['firmware'][counter][1] <= before['firmware'][counter][1]:
                raise RuntimeError(f'Platform sleep unconfirmed: {counter} did not advance')
    if after['online_cpus'] != before['online_cpus']:
        raise RuntimeError('Online CPU set was not restored after suspend')
finally:
    if a.mem_sleep == 'deep':
        module = os.environ.get('ZRN_SYSTEM_SLEEP_MODULE', 'zrn_system_sleep')
        mif_param = Path('/sys/module') / module / 'parameters/mif_sleep'
        if mif_param.exists():
            write(mif_param, 0)
    if pmic_armed:
        # also clears INT1-7, so the PMIC line is high for the next sleep
        subprocess.run(['python3', str(a.pmic_rtc_tool), 'disarm'], timeout=120)
    fvp_errors = []
    for domain in reversed(fvp_domains):
        try:
            subprocess.run(['python3', str(a.acpm_fvp_tool), 'ipc', '1',
                            str(domain), '1', '5', '2'], check=True, timeout=5)
        except (subprocess.SubprocessError, OSError) as error:
            fvp_errors.append(str(error))
    if fvp_errors:
        print(json.dumps({'phase': 'fvp_restore_failed', 'errors': fvp_errors}), flush=True)
    # Raise interconnect capacity before raising memory bandwidth again.
    if int_frequency_changed:
        try:
            subprocess.run(['python3', str(a.acpm_fvp_tool), 'dvfs', 'int',
                            str(a.int_restore_khz)], check=True, timeout=5)
        except (subprocess.SubprocessError, OSError) as error:
            fvp_errors.append(str(error))
            print(json.dumps({'phase': 'int_frequency_restore_failed', 'error': str(error)}), flush=True)
    if mif_frequency_changed:
        try:
            subprocess.run(['python3', str(a.acpm_fvp_tool), 'dvfs', 'mif',
                            str(a.mif_restore_khz)], check=True, timeout=5)
        except (subprocess.SubprocessError, OSError) as error:
            fvp_errors.append(str(error))
            print(json.dumps({'phase': 'mif_frequency_restore_failed', 'error': str(error)}), flush=True)
    for name, dom, khz in reversed(extra_changed):
        try:
            subprocess.run(['python3', str(a.acpm_fvp_tool), 'dvfs', str(dom), str(khz)],
                           check=True, timeout=5)
        except (subprocess.SubprocessError, OSError) as error:
            fvp_errors.append(str(error))
            print(json.dumps({'phase': f'{name}_frequency_restore_failed', 'error': str(error)}), flush=True)
    if host_req_wake_masked:
        subprocess.run(['rmmod', 'zrn_hostreq_wake_test'], check=True)
    write('/sys/power/pm_test', 'none')
    write(mem_sleep_path, old_mem_sleep)
    if old_hotplug_suspend is not None:
        write(hotplug_suspend, old_hotplug_suspend)
    write(P / 'alarm_seconds', 0)
    if a.wake_source == 'rtc':
        write('/sys/class/rtc/rtc0/wakealarm', 0)
    write(P / 's2idle_sicd', old_sicd)
    if old_cpd is not None:
        write(P / 's2idle_cpd', old_cpd)
    if old_cpu is not None:
        write(P / 'sicd_cpu', old_cpu)
    write(U, old_level)
    if bound and not (W / DEV).exists():
        if a.pcie_recovery_module:
            subprocess.run(['insmod', str(a.pcie_recovery_module), 'recover=1'], check=True)
            subprocess.run(['rmmod', 'zrn_pcie_recovery'], check=True)
        write(W / 'bind', DEV)
    subprocess.run(['dmesg', '--level=err,warn'])
    if fvp_errors:
        raise RuntimeError('ACPM handshake or frequency restoration failed on resume')
