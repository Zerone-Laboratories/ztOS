"""Generic backends: ztOSIAL over standard Linux (power_supply, backlight,
cpufreq, thermal, devfreq, iio, input force feedback, leds, ModemManager).

A phone with full mainline support needs nothing else; its manifest only
names nodes where guessing would be wrong (which backlight, which haptic
device). Each class reads its [ial.X] table for those.
"""
import fcntl
import json
import os
import pathlib
import struct
import time

from . import Backend, NotSupported, exists, first, read, read_int, write

SUPPLY = '/sys/class/power_supply'


# ---- Power1 -----------------------------------------------------------------

class Power(Backend):
    interface = 'Power1'
    poll_s = 10

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.battery = config.get('battery') or self._find('Battery')
        self.charger = config.get('charger') or self._find_charger()
        self.sign = int(config.get('current_sign', 1))    # make + mean charging

    @staticmethod
    def _find(kind):
        for path in sorted(pathlib.Path(SUPPLY).glob('*')):
            if read(path / 'type') == kind:
                return str(path)
        return None

    def _find_charger(self):
        for path in sorted(pathlib.Path(SUPPLY).glob('*')):
            if read(path / 'type') != 'Battery' and (path / 'online').exists():
                return str(path)
        return None

    def available(self):
        return bool(self.battery)

    def node(self, name, where=None):
        base = where or self.battery
        return os.path.join(base, name) if base else ''

    def props(self):
        b = self.battery
        status = (read(self.node('status'), 'Unknown') or 'Unknown').lower().replace(' ', '-')
        online = read_int(self.node('online', self.charger)) == 1 if self.charger else False
        limit = self.node('input_current_limit', self.charger) if self.charger else ''
        end = self.node('charge_control_end_threshold')
        return {
            'Capacity': read_int(self.node('capacity'), -1) if b else -1,
            'Status': status,
            'CurrentNow': self.sign * read_int(self.node('current_now')) if b else 0,
            'VoltageNow': read_int(self.node('voltage_now')) if b else 0,
            'Temperature': read_int(self.node('temp')) / 10.0 if b else 0.0,
            'ChargerOnline': online,
            'ChargerType': (read(self.node('usb_type', self.charger)) or read(self.node('type', self.charger)) or '')
            if self.charger else '',
            'InputCurrentLimit': read_int(limit, -1) if exists(limit) else -1,
            'ChargeLimit': read_int(end, 100) if exists(end) else 100,
            'CapacityNode': self.node('capacity') if b else '',
            'CurrentNode': self.node('current_now') if b else '',
            'StatusNode': self.node('status') if b else '',
        }

    def m_SetInputCurrentLimit(self, microamps):
        path = self.node('input_current_limit', self.charger) if self.charger else ''
        if not exists(path):
            raise NotSupported('no input_current_limit')
        write(path, int(microamps))

    def m_SetChargeLimit(self, percent):
        path = self.node('charge_control_end_threshold')
        if not exists(path):
            raise NotSupported('no charge_control_end_threshold')
        write(path, max(50, min(100, int(percent))))


# ---- Perf1 ------------------------------------------------------------------

class Perf(Backend):
    """The topology a policy daemon (zrnperfd) needs, from cpufreq, the
    manifest's caps and thermal sensors, written once to TopologyPath."""
    interface = 'Perf1'
    poll_s = 5
    PATH = '/run/ztos-ial/Perf1.json'

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.topology = self._topology()
        try:
            os.makedirs(os.path.dirname(self.PATH), exist_ok=True)
            tmp = self.PATH + '.new'
            with open(tmp, 'w') as stream:
                json.dump(self.topology, stream, indent=1)
            os.replace(tmp, self.PATH)
            # the same, flat, for C daemons (zrnperfd): key=value lines
            flat = self.PATH[:-len('.json')] + '.env'
            with open(flat + '.new', 'w') as stream:
                stream.write(self.flat())
            os.replace(flat + '.new', flat)
        except OSError:
            pass

    def flat(self):
        t = self.topology
        lines = []
        for cluster in t['clusters']:
            lines.append(f"cluster.{cluster['policy']}={','.join(str(c) for c in cluster['cpus'])}")
            for online, khz in sorted(cluster['caps_by_online'].items()):
                lines.append(f"cap.{cluster['policy']}.{online}={khz}")
        for name, path in t['thermal'].items():
            lines.append(f'thermal.{name}={path}')
        lines += [f"thermal_unit={t['thermal_unit']}",
                  'thermal_marks=' + ','.join(str(m) for m in t['thermal_marks']),
                  f"gpu_devfreq={t['gpu_devfreq']}",
                  f"battery_current={t['battery_current_node']}",
                  'keep_online=' + ','.join(str(c) for c in t['keep_online']),
                  f"idle_mask={t['idle_mask'] or ''}", f"hotplug={int(bool(t['hotplug']))}"]
        return '\n'.join(lines) + '\n'

    def _topology(self):
        clusters = []
        caps = self.config.get('caps', {})        # policyN = { "1" = khz, "2" = khz, ... }
        for policy in sorted(pathlib.Path('/sys/devices/system/cpu/cpufreq').glob('policy*'),
                             key=lambda p: int(p.name[6:])):
            cpus = [int(c) for c in (read(policy / 'related_cpus') or '').split()]
            freqs = sorted(int(f) for f in (read(policy / 'scaling_available_frequencies') or '').split())
            if not freqs:
                freqs = [read_int(policy / 'cpuinfo_min_freq'), read_int(policy / 'cpuinfo_max_freq')]
            clusters.append({
                'policy': policy.name, 'cpus': cpus, 'freqs_khz': freqs,
                'caps_by_online': {str(k): int(v) for k, v in caps.get(policy.name, {}).items()},
            })
        thermal = dict(self.config.get('thermal', {}))
        if not thermal:
            for zone in sorted(pathlib.Path('/sys/class/thermal').glob('thermal_zone*')):
                thermal[read(zone / 'type') or zone.name] = str(zone / 'temp')
        gpu = self.config.get('gpu_devfreq') or first('/sys/class/devfreq/*gpu*') or ''
        return {
            'clusters': clusters,
            'keep_online': self.config.get('keep_online', [0]),
            'idle_mask': self.config.get('idle_mask'),
            'hotplug': self.config.get('hotplug', True),
            'thermal': thermal,
            'thermal_unit': self.config.get('thermal_unit', 'millicelsius'),
            'thermal_marks': self.config.get('thermal_marks', [70, 76, 82]),
            'gpu_devfreq': gpu,
            'battery_current_node': self.manifest.get('ial', {}).get('Power1', {}).get('current_node', ''),
        }

    def props(self):
        temps = {}
        unit = self.topology['thermal_unit']
        for name, path in self.topology['thermal'].items():
            value = read_int(path, -1000000)
            temps[name] = value // 1000 if unit == 'millicelsius' and value > -1000000 else value
        return {
            'Topology': json.dumps(self.topology),
            'TopologyPath': self.PATH,
            'Thermal': temps,
            'GpuDevfreq': self.topology['gpu_devfreq'],
        }


# ---- Display1 ---------------------------------------------------------------

class Display(Backend):
    interface = 'Display1'
    poll_s = 5

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.node = config.get('node') or (first('/sys/class/backlight/*') or '')
        self.curve = config.get('curve', 'linear')
        self.mode = config.get('mode', 'linear')

    def available(self):
        return bool(self.node)

    def _max(self):
        return read_int(os.path.join(self.node, 'max_brightness'), 0) if self.node else 0

    def _to_level(self, value):
        value = max(0.0, min(1.0, float(value)))
        if self.curve.startswith('gamma:'):
            value = value ** float(self.curve[6:])
        return round(value * self._max())

    def _from_level(self, level):
        top = self._max()
        if not top:
            return 0.0
        value = level / top
        if self.curve.startswith('gamma:'):
            value = value ** (1 / float(self.curve[6:]))
        return value

    def props(self):
        level = read_int(os.path.join(self.node, 'brightness')) if self.node else 0
        return {
            'Brightness': self._from_level(level), 'Node': self.node, 'Mode': self.mode,
            'MaxLevel': self._max(), 'Curve': self.curve, 'NitsPath': self.config.get('nits_path', ''),
        }

    def m_SetBrightness(self, value):
        if not self.node:
            raise NotSupported('no backlight')
        write(os.path.join(self.node, 'brightness'), self._to_level(value))


# ---- Sleep1 -----------------------------------------------------------------

class Sleep(Backend):
    """The kernel's own suspend (s2idle or deep) through logind; inhibitors
    are kept here and held with a logind sleep inhibitor."""
    interface = 'Sleep1'
    poll_s = 10

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.held = {}
        self.cookie = 0

    def props(self):
        mem = read('/sys/power/mem_sleep', '') or ''
        method = mem[mem.find('[') + 1:mem.find(']')] if '[' in mem else 's2idle'
        return {'Method': method, 'Enabled': True, 'State': 'awake',
                'Inhibitors': sorted(f'{who}: {why}' for who, why, _ in self.held.values())}

    def m_SetEnabled(self, enabled):
        raise NotSupported('the generic backend follows logind')

    def m_Inhibit(self, who, why):
        self.cookie += 1
        fd = None
        try:
            from gi.repository import Gio, GLib
            bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
            reply, fds = bus.call_with_unix_fd_list_sync(
                'org.freedesktop.login1', '/org/freedesktop/login1', 'org.freedesktop.login1.Manager',
                'Inhibit', GLib.Variant('(ssss)', ('sleep', who, why, 'block')), GLib.VariantType('(h)'),
                Gio.DBusCallFlags.NONE, -1, None, None)
            fd = fds.get(reply.unpack()[0])
        except Exception:
            pass
        self.held[self.cookie] = (who, why, fd)
        return self.cookie

    def m_Uninhibit(self, cookie):
        held = self.held.pop(int(cookie), None)
        if held and held[2] is not None:
            os.close(held[2])


# ---- Sensors1 ---------------------------------------------------------------

IIO = '/sys/bus/iio/devices'


class Sensors(Backend):
    interface = 'Sensors1'

    def _devices(self):
        found = {}
        for device in sorted(pathlib.Path(IIO).glob('iio:device*')):
            names = {p.name for p in device.iterdir()}
            if any(n.startswith('in_illuminance') for n in names):
                found.setdefault('light', device)
            if {'in_accel_x_raw', 'in_accel_y_raw', 'in_accel_z_raw'} <= names:
                found.setdefault('accelerometer', device)
            if 'in_proximity_raw' in names:
                found.setdefault('proximity', device)
            if {'in_magn_x_raw', 'in_magn_y_raw', 'in_magn_z_raw'} <= names:
                found.setdefault('magnetometer', device)
        return found

    def available(self):
        return bool(self._devices())

    def props(self):
        return {'Available': sorted(self._devices())}

    def m_Read(self, sensor):
        device = self._devices().get(sensor)
        if device is None:
            raise NotSupported(sensor)
        if sensor == 'light':
            for name in ('in_illuminance_input', 'in_illuminance_raw'):
                if (device / name).exists():
                    scale = float(read(device / 'in_illuminance_scale', '1') or 1)
                    return [float(read(device / name)) * (1 if name.endswith('input') else scale)]
        axis = {'accelerometer': 'accel', 'magnetometer': 'magn'}.get(sensor)
        if axis:
            scale = float(read(device / f'in_{axis}_scale', '1') or 1)
            return [read_int(device / f'in_{axis}_{a}_raw') * scale for a in 'xyz']
        if sensor == 'proximity':
            return [float(read_int(device / 'in_proximity_raw'))]
        raise NotSupported(sensor)

    def m_SetAutoBrightness(self, enabled):
        raise NotSupported('no automatic brightness service on the generic backend')


# ---- Haptics1 ---------------------------------------------------------------

EV_FF = 0x15
FF_RUMBLE = 0x50
EVIOCSFF = 0x40304580          # _IOW('E', 0x80, struct ff_effect), 48 bytes on 64-bit
EVIOCRMFF = 0x40044581         # _IOW('E', 0x81, int)
PATTERNS = {'tap': (12, 0.6), 'click': (20, 0.8), 'notify': (180, 1.0), 'long': (500, 1.0)}


class Haptics(Backend):
    interface = 'Haptics1'

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.device = self._find(config.get('device'))

    @staticmethod
    def _find(name):
        for event in sorted(pathlib.Path('/sys/class/input').glob('event*')):
            label = read(event / 'device/name', '')
            caps = read(event / 'device/capabilities/ff', '0') or '0'
            words = [int(w, 16) for w in caps.split()][::-1]     # lowest word last in sysfs
            bit = FF_RUMBLE
            rumble = len(words) > bit // 64 and words[bit // 64] >> (bit % 64) & 1
            if (name and label == name) or (not name and rumble):
                return '/dev/input/' + event.name
        return ''

    def available(self):
        return bool(self.device)

    def props(self):
        return {'Patterns': sorted(PATTERNS), 'Device': self.device}

    def m_Play(self, pattern):
        if pattern not in PATTERNS:
            raise NotSupported(pattern)
        self.m_Vibrate(*PATTERNS[pattern])

    def m_Vibrate(self, milliseconds, strength):
        if not self.device:
            raise NotSupported('no force-feedback device')
        ms = max(1, min(5000, int(milliseconds)))
        magnitude = int(max(0.0, min(1.0, float(strength))) * 0xffff)
        # struct ff_effect: type, id, direction, trigger(button, interval),
        # replay(length, delay), 2 bytes padding, then the union (rumble:
        # strong, weak; the rest of its 32 bytes zero)
        effect = bytearray(struct.pack('=HhHHHHH2xHH28x', FF_RUMBLE, -1, 0, 0, 0, ms, 0, magnitude, magnitude))
        fd = os.open(self.device, os.O_RDWR)
        try:
            fcntl.ioctl(fd, EVIOCSFF, effect, True)
            effect_id = struct.unpack_from('=h', effect, 2)[0]
            now = time.time()
            event = struct.pack('=qqHHi', int(now), int((now % 1) * 1e6), EV_FF, effect_id, 1)
            os.write(fd, event)
        except OSError:
            os.close(fd)
            raise

        # the effect ends by itself; it is removed after, without holding up the bus
        def done():
            try:
                fcntl.ioctl(fd, EVIOCRMFF, effect_id)
            except OSError:
                pass
            os.close(fd)
            return False
        from gi.repository import GLib
        GLib.timeout_add(ms + 50, done)


# ---- Lights1 ----------------------------------------------------------------

LEDS = '/sys/class/leds'


class Lights(Backend):
    """A notification light made of red, green and blue LEDs (names in the
    manifest: rgb = ["red:status", "green:status", "blue:status"]), or a
    multicolour LED, and a torch."""
    interface = 'Lights1'

    def __init__(self, config, manifest):
        super().__init__(config, manifest)
        self.rgb = config.get('rgb') or self._guess_rgb()
        self.torch = config.get('torch') or self._guess_torch()

    @staticmethod
    def _guess_rgb():
        names = [p.name for p in pathlib.Path(LEDS).glob('*')]
        pick = []
        for colour in ('red', 'green', 'blue'):
            match = [n for n in names if n.startswith(colour + ':')]
            if match:
                pick.append(sorted(match)[0])
        return pick if len(pick) == 3 else []

    @staticmethod
    def _guess_torch():
        for pattern in ('*:torch', '*:flash', '*torch*', '*flash*'):
            found = first(os.path.join(LEDS, pattern))
            if found:
                return os.path.basename(found)
        return ''

    def available(self):
        return bool(self.rgb or self.torch)

    def props(self):
        on = read_int(os.path.join(LEDS, self.torch, 'brightness')) > 0 if self.torch else False
        return {'Lights': ['notification'] if self.rgb else [], 'Torch': on}

    def m_Set(self, light, colour, on_ms, off_ms):
        if light != 'notification' or not self.rgb:
            raise NotSupported(light)
        for shift, name in zip((16, 8, 0), self.rgb):
            base = os.path.join(LEDS, name)
            level = (int(colour) >> shift) & 0xff
            top = read_int(os.path.join(base, 'max_brightness'), 255)
            if on_ms and off_ms and level:
                write(os.path.join(base, 'trigger'), 'timer')
                write(os.path.join(base, 'delay_on'), int(on_ms))
                write(os.path.join(base, 'delay_off'), int(off_ms))
            else:
                write(os.path.join(base, 'trigger'), 'none')
            write(os.path.join(base, 'brightness'), level * top // 255)

    def m_SetTorch(self, on):
        if not self.torch:
            raise NotSupported('no torch')
        base = os.path.join(LEDS, self.torch)
        write(os.path.join(base, 'brightness'), read_int(os.path.join(base, 'max_brightness'), 1) if on else 0)


# ---- Modem1 -----------------------------------------------------------------

class Modem(Backend):
    """ModemManager's first modem. Calls and SMS go through its Voice and
    Messaging interfaces."""
    interface = 'Modem1'
    poll_s = 10
    MM = 'org.freedesktop.ModemManager1'

    def _modem(self):
        from gi.repository import Gio
        try:
            manager = Gio.DBusObjectManagerClient.new_for_bus_sync(
                Gio.BusType.SYSTEM, Gio.DBusObjectManagerClientFlags.NONE, self.MM,
                '/org/freedesktop/ModemManager1', None, None, None)
        except Exception:
            return None, None
        objects = manager.get_objects()
        return (manager, objects[0]) if objects else (manager, None)

    def _prop(self, obj, interface, name, default):
        proxy = obj.get_interface(interface) if obj else None
        value = proxy.get_cached_property(name) if proxy else None
        return value.unpack() if value is not None else default

    def available(self):
        return self._modem()[1] is not None

    def props(self):
        _, obj = self._modem()
        if obj is None:
            return {'Present': False, 'Powered': False, 'SimState': 'absent', 'Operator': '', 'Signal': 0,
                    'Technology': '', 'Registration': 'none', 'DataEnabled': False, 'DataConnected': False,
                    'CallState': 'idle', 'CallNumber': ''}
        modem = self.MM + '.Modem'
        state = self._prop(obj, modem, 'State', 0)
        quality = self._prop(obj, modem, 'SignalQuality', (0, False))[0]
        return {
            'Present': True, 'Powered': state >= 6,             # MM_MODEM_STATE_ENABLED
            'SimState': 'ready' if self._prop(obj, modem, 'Sim', '/') != '/' else 'absent',
            'Operator': self._prop(obj, self.MM + '.Modem.Modem3gpp', 'OperatorName', ''),
            'Signal': int(quality), 'Technology': str(self._prop(obj, modem, 'AccessTechnologies', 0)),
            'Registration': {11: 'connected', 8: 'home'}.get(state, 'searching' if state >= 6 else 'none'),
            'DataEnabled': state >= 8, 'DataConnected': state == 11,
            'CallState': 'idle', 'CallNumber': '',
        }

    def _call(self, interface, method, signature, args):
        from gi.repository import Gio, GLib
        _, obj = self._modem()
        if obj is None:
            raise NotSupported('no modem')
        bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        return bus.call_sync(self.MM, obj.get_object_path(), interface, method,
                             GLib.Variant(signature, args) if signature else None,
                             None, Gio.DBusCallFlags.NONE, 30000, None)

    def m_SetPowered(self, powered):
        self._call(self.MM + '.Modem', 'Enable', '(b)', (bool(powered),))

    def m_SetDataEnabled(self, enabled):
        raise NotSupported('data connections belong to NetworkManager on the generic backend')

    def m_Dial(self, number):
        from gi.repository import GLib
        reply = self._call(self.MM + '.Modem.Voice', 'CreateCall', '(a{sv})',
                           ({'number': GLib.Variant('s', number)},))
        path = reply.unpack()[0]
        from gi.repository import Gio
        Gio.bus_get_sync(Gio.BusType.SYSTEM, None).call_sync(
            self.MM, path, self.MM + '.Call', 'Start', None, None, Gio.DBusCallFlags.NONE, 30000, None)

    def m_Answer(self):
        raise NotSupported('answering goes through the call object on the generic backend')

    def m_Hangup(self):
        self._call(self.MM + '.Modem.Voice', 'HangupAll', None, None)

    def m_SendSms(self, number, text):
        from gi.repository import Gio, GLib
        reply = self._call(self.MM + '.Modem.Messaging', 'Create', '(a{sv})',
                           ({'number': GLib.Variant('s', number), 'text': GLib.Variant('s', text)},))
        Gio.bus_get_sync(Gio.BusType.SYSTEM, None).call_sync(
            self.MM, reply.unpack()[0], self.MM + '.Sms', 'Send', None, None, Gio.DBusCallFlags.NONE, 60000, None)


# ---- the rest ---------------------------------------------------------------

class CallAudio(Backend):
    """ALSA UCM's Voice Call verb would go here; nothing generic drives it yet."""
    interface = 'CallAudio1'

    def available(self):
        return False

    def props(self):
        return {'Route': 'earpiece', 'Routes': [], 'Muted': False}

    def m_SetRoute(self, route):
        raise NotSupported('no call audio')

    def m_SetMuted(self, muted):
        raise NotSupported('no call audio')


class AudioDevice(Backend):
    interface = 'AudioDevice1'

    def available(self):
        return False

    def props(self):
        return {'SpeakerProtection': False, 'Calibrated': False, 'AmpHealthy': True, 'Details': {}}


class Accel(Backend):
    interface = 'Accel1'

    def available(self):
        return False      # the CPU is no accelerator

    def props(self):
        return {'Engine': 'cpu', 'Available': True, 'DevicePath': ''}


class Android(Backend):
    interface = 'Android1'

    def available(self):
        return self.props()['Enabled']

    def props(self):
        android = self.manifest.get('android', {})
        binder = android.get('binder')
        if not binder:
            binder = 'binderfs' if 'binder' in (read('/proc/filesystems', '') or '') else 'none'
        return {'Binder': binder, 'Gralloc': android.get('gralloc', 'gbm'),
                'MemoryBudgetMb': int(android.get('memory_budget_mb', 1024)),
                'Enabled': binder != 'none'}


CLASSES = {cls.interface: cls for cls in
           (Power, Perf, Display, Sleep, Sensors, Haptics, Lights, Modem, CallAudio, AudioDevice, Accel, Android)}
