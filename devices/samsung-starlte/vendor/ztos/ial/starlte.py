"""ztOSIAL backends for the Galaxy S9 (starlte), for what generic Linux does
not cover: the panel's brightness table, Exynos SoC sleep (zrn-sleepd), the
SSP sensor hub (ztoscsf-sensord), the Shannon modem (ztoscorenetworkingd),
the ABOX DSP's call routes and speaker protection, and the SCORE DSP.

Each class wraps what the S9 already had; nothing here drives hardware
directly that its own daemon did not.
"""
import os
import subprocess

from ztos_ial.backends import Backend, NotSupported, exists, read, read_int
from ztos_ial.backends import generic


def tell(pipe, line):
    """A line down one of the S9 daemons' control pipes (they read lines)."""
    try:
        fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK)
    except OSError as error:
        raise NotSupported(f'{pipe}: {error.strerror}') from None
    try:
        os.write(fd, (line.rstrip('\n') + '\n').encode())
    finally:
        os.close(fd)


def key_values(path):
    values = {}
    for line in (read(path, '') or '').splitlines():
        key, sep, value = line.partition('=')
        if sep:
            values[key.strip()] = value.strip()
    return values


# ---- Display1: the S6E3HA8's brightness -------------------------------------

class Display(generic.Display):
    """With its full table loaded (ztos-panel-bl, /run/ztos-panel-bl) the panel
    takes every level of it; without, only the AOR register, and the shell
    dims the picture for the rest (ztmetro.c, BL_TABLE and BL_S9)."""

    def props(self):
        values = super().props()
        values['Mode'] = 'table' if exists('/run/ztos-panel-bl') else 'aor'
        values['Curve'] = 'table:' + self.config.get('nits_path', '') if values['Mode'] == 'table' else 'linear'
        return values


# ---- Sleep1: zrn-sleepd -----------------------------------------------------

SLEEP_STATE = '/var/lib/zrn-sleep/enabled'
SLEEP_STATUS = '/run/zrn-sleepd/status'
SLEEP_INHIBIT = '/run/zrn-sleepd/inhibit'


class Sleep(Backend):
    interface = 'Sleep1'
    poll_s = 10

    def props(self):
        status = key_values(SLEEP_STATUS)
        held = sorted(os.listdir(SLEEP_INHIBIT)) if os.path.isdir(SLEEP_INHIBIT) else []
        return {'Method': 'soc', 'Enabled': exists(SLEEP_STATE), 'State': status.get('state', 'unknown'),
                'Inhibitors': held}

    def m_SetEnabled(self, enabled):
        subprocess.run(['/usr/local/sbin/zrn-sleepd', 'enable' if enabled else 'disable'],
                       check=True, timeout=20)

    def m_Inhibit(self, who, why):
        os.makedirs(SLEEP_INHIBIT, exist_ok=True)
        cookie = 1
        while exists(os.path.join(SLEEP_INHIBIT, f'ial-{cookie}')):
            cookie += 1
        with open(os.path.join(SLEEP_INHIBIT, f'ial-{cookie}'), 'w') as stream:
            stream.write(f'{who}: {why}\n')
        return cookie

    def m_Uninhibit(self, cookie):
        try:
            os.unlink(os.path.join(SLEEP_INHIBIT, f'ial-{int(cookie)}'))
        except FileNotFoundError:
            pass


# ---- Sensors1: the sensor hub, through ztoscsf-sensord ----------------------

SENSORS = '/run/zrn-sensors'


class Sensors(Backend):
    interface = 'Sensors1'

    def available(self):
        return exists(os.path.join(SENSORS, 'light'))

    def props(self):
        return {'Available': ['light'] if self.available() else []}

    def m_Read(self, sensor):
        if sensor != 'light' or not self.available():
            raise NotSupported(sensor)
        text = read(os.path.join(SENSORS, 'light'), '') or ''
        return [float(text.split()[0])] if text.split() else [0.0]

    def m_SetAutoBrightness(self, enabled):
        tell('/run/ztoscsf-sensord.ctl', 'auto on' if enabled else 'auto off')


# ---- Modem1: ztoscorenetworkingd --------------------------------------------

MODEM_STATUS = '/run/ztoscorenetworking/status'
MODEM_CTL = '/run/ztoscorenetworking.ctl'


class Modem(Backend):
    interface = 'Modem1'
    poll_s = 5

    def available(self):
        return exists(MODEM_STATUS)

    def props(self):
        s = key_values(MODEM_STATUS)
        bars = int(s.get('bars', '0') or 0)
        return {
            'Present': self.available(), 'Powered': s.get('airplane', '1') != '1',
            'SimState': s.get('sim', 'absent') or 'absent', 'Operator': s.get('operator', ''),
            'Signal': max(0, min(100, bars * 25)), 'Technology': s.get('tech', ''),
            'Registration': s.get('registered', 'none') or 'none',
            'DataEnabled': s.get('data_on', '0') == '1', 'DataConnected': s.get('data', '0') == '1',
            'CallState': s.get('call', 'idle') or 'idle', 'CallNumber': s.get('call_number', ''),
        }

    def m_SetPowered(self, powered):
        tell(MODEM_CTL, 'airplane off' if powered else 'airplane on')

    def m_SetDataEnabled(self, enabled):
        tell(MODEM_CTL, 'data on' if enabled else 'data off')

    def m_Dial(self, number):
        tell(MODEM_CTL, 'call ' + number.split()[0])

    def m_Answer(self):
        tell(MODEM_CTL, 'answer')

    def m_Hangup(self):
        tell(MODEM_CTL, 'hangup')

    def m_SendSms(self, number, text):
        escaped = text.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')
        tell(MODEM_CTL, f'smse {number.split()[0]} {escaped}')


# ---- CallAudio1: the voice path in the ABOX, through the modem daemon -------

class CallAudio(Backend):
    interface = 'CallAudio1'

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.route = 'earpiece'

    def available(self):
        return exists(MODEM_STATUS)

    def props(self):
        return {'Route': self.route, 'Routes': ['earpiece', 'speaker'], 'Muted': False}

    def m_SetRoute(self, route):
        if route not in ('earpiece', 'speaker'):
            raise NotSupported(route)
        tell(MODEM_CTL, 'speaker on' if route == 'speaker' else 'speaker off')
        self.route = route

    def m_SetMuted(self, muted):
        raise NotSupported('mute is not wired on this phone yet')


# ---- AudioDevice1: snd-zrn-abox's own state ---------------------------------

ABOX = '/sys/module/snd_zrn_abox/parameters'


class AudioDevice(Backend):
    interface = 'AudioDevice1'
    poll_s = 30

    def available(self):
        return os.path.isdir(ABOX)

    def props(self):
        details = {}
        if os.path.isdir(ABOX):
            for name in ('fast_rebuilds', 'mixp_flushes', 'dsp_restarts', 'watchdog_recoveries',
                         'ipc_retries', 'dsm_temp', 'aud_cycles'):
                value = read(os.path.join(ABOX, name))
                if value is not None:
                    details[name] = value
        return {
            'SpeakerProtection': read_int(os.path.join(ABOX, 'dsm_temp')) > 0,
            # ztos-speaker-cal hands the module EFS's factory values at boot
            'Calibrated': (read(os.path.join(ABOX, 'dsm_rdc'), '0,0') or '0,0').replace('0', '').strip(',') != '',
            'AmpHealthy': read_int(os.path.join(ABOX, 'watchdog_recoveries')) == 0,
            'Details': details,
        }


# ---- Accel1: the SCORE vision DSP (zrn-score) -------------------------------

class Accel(Backend):
    interface = 'Accel1'

    def available(self):
        return exists('/dev/score')

    def props(self):
        return {'Engine': 'score' if self.available() else 'cpu', 'Available': self.available(),
                'DevicePath': '/dev/score' if self.available() else ''}
