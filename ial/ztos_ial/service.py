"""ztos-iald: serves the manifest's backends on the system bus.

Each interface is an object, /org/ztos/ial/<Area>, implementing
org.ztos.ial.<Area><Major> as described by ial/spec/. Properties are read
from the backend on each Get; backends with poll_s are compared every poll_s
seconds and changes go out as PropertiesChanged. A backend that raises is
logged and answers org.ztos.ial.Error.*; it never takes the daemon down.
"""
import importlib.util
import os
import pathlib
import sys
import traceback

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

from . import BUS_NAME, INTERFACES, PREFIX, object_path  # noqa: E402
from . import manifest as manifests  # noqa: E402
from .backends import Backend, NotSupported  # noqa: E402
from .backends import generic  # noqa: E402

SPEC_DIRS = [os.environ.get('ZTOS_IAL_SPEC', ''), '/usr/share/ztos-ial/spec',
             str(pathlib.Path(__file__).resolve().parent.parent / 'spec')]


def log(text):
    print('ztos-iald: ' + text, file=sys.stderr, flush=True)


def spec(interface):
    for directory in SPEC_DIRS:
        path = pathlib.Path(directory) / f'{PREFIX}{interface}.xml' if directory else None
        if path and path.exists():
            return Gio.DBusNodeInfo.new_for_xml(path.read_text()).interfaces[0]
    raise FileNotFoundError(f'no spec for {interface}')


def load_vendor(interface, entry, data):
    """The vendor module's class for this interface (Power for Power1)."""
    module_path = entry['module']
    name = 'ztos_ial_vendor_' + pathlib.Path(module_path).stem
    module = sys.modules.get(name)
    if module is None:
        loaded = importlib.util.spec_from_file_location(name, module_path)
        module = importlib.util.module_from_spec(loaded)
        sys.modules[name] = module
        loaded.loader.exec_module(module)
    cls = getattr(module, entry.get('class') or interface.rstrip('0123456789'))
    return cls(entry, data)


class Device(Backend):
    interface = 'Device1'

    def __init__(self, config, manifest, served, missing, backends):
        super().__init__(config, manifest)
        self.served, self.missing, self.backends = served, missing, backends

    def props(self):
        device = self.manifest.get('device', {})
        found = set()
        for name, backend in self.backends.items():
            try:
                if backend.available():
                    found.add(name.rstrip('0123456789').lower())
            except Exception:
                pass
        capabilities = sorted(set(device.get('capabilities', [])) | found)
        return {
            'Codename': device.get('codename', 'generic'), 'Name': device.get('name', 'Linux phone'),
            'FormFactor': device.get('form_factor', 'phone'), 'Scale': float(device.get('scale', 1.0)),
            'GsiVersion': manifests.gsi().get('version', 'unknown'), 'Capabilities': capabilities,
            'Interfaces': {name: int(self.manifest['ial'][name].get('version', 1)) for name in self.served},
            'Backends': {name: self.manifest['ial'][name].get('backend', 'generic') for name in self.served},
            'Missing': sorted(self.missing), 'ManifestPath': self.manifest['_path'],
        }


class Service:
    def __init__(self):
        self.manifest = manifests.load()
        self.backends = {}
        self.info = {}
        self.last = {}
        missing = []
        for name in INTERFACES:
            if name == 'Device1':
                continue
            entry = self.manifest['ial'][name]
            kind = entry.get('backend', 'generic')
            if kind in ('none', 'service'):
                if kind == 'none':
                    missing.append(name)
                continue        # a service backend owns org.ztos.ial.<Area> itself
            try:
                backend = load_vendor(name, entry, self.manifest) if kind == 'vendor' \
                    else generic.CLASSES[name](entry, self.manifest)
                self.backends[name] = backend
            except Exception:
                log(f'{name}: backend failed to start:\n' + traceback.format_exc())
                missing.append(name)
        served = ['Device1'] + list(self.backends)
        self.backends['Device1'] = Device({}, self.manifest, served, missing, dict(self.backends))

    # ---- D-Bus ----

    def start(self, connection):
        for name, backend in self.backends.items():
            info = spec(name)
            self.info[name] = info
            connection.register_object(object_path(name), info, self._method(name),
                                       self._get(name), None)
            if backend.poll_s:
                GLib.timeout_add_seconds(backend.poll_s, self._poll, connection, name)
        log(f'serving {", ".join(sorted(self.backends))} for '
            f'{self.manifest.get("device", {}).get("codename", "generic")}')

    def _values(self, name):
        backend = self.backends[name]
        values = dict(backend.props())
        values['Version'] = backend.version
        return values

    def _variant(self, name, prop, value):
        signature = self.info[name].lookup_property(prop).signature
        return GLib.Variant(signature, value)

    def _get(self, name):
        def get(connection, sender, path, interface, prop):
            try:
                return self._variant(name, prop, self._values(name)[prop])
            except Exception:
                log(f'{name}.{prop}:\n' + traceback.format_exc())
                return None
        return get

    def _method(self, name):
        def call(connection, sender, path, interface, method, params, invocation):
            backend = self.backends[name]
            try:
                result = backend.call(method, params.unpack())
                out = self.info[name].lookup_method(method).out_args
                if out:
                    signature = '(' + ''.join(a.signature for a in out) + ')'
                    invocation.return_value(GLib.Variant(signature, (result,)))
                else:
                    invocation.return_value(None)
                if backend.poll_s:
                    self._poll(connection, name)
            except NotSupported as error:
                invocation.return_dbus_error('org.ztos.ial.Error.NotSupported', str(error) or method)
            except PermissionError as error:
                invocation.return_dbus_error('org.ztos.ial.Error.PermissionDenied', str(error))
            except Exception as error:
                log(f'{name}.{method}:\n' + traceback.format_exc())
                invocation.return_dbus_error('org.ztos.ial.Error.Failed', str(error))
        return call

    def _poll(self, connection, name):
        try:
            values = self._values(name)
        except Exception:
            log(f'{name} poll:\n' + traceback.format_exc())
            return True
        before = self.last.get(name)
        self.last[name] = values
        if before is not None:
            changed = {k: self._variant(name, k, v) for k, v in values.items() if before.get(k) != v}
            if changed:
                connection.emit_signal(None, object_path(name), 'org.freedesktop.DBus.Properties',
                                       'PropertiesChanged',
                                       GLib.Variant('(sa{sv}as)', (PREFIX + name, changed, [])))
        return True


def main():
    service = Service()
    loop = GLib.MainLoop()

    def acquired(connection, _name):
        log('on the bus as ' + BUS_NAME)

    def lost(connection, _name):
        log('lost the bus name ' + BUS_NAME)
        loop.quit()

    def bus(connection, _name):
        service.start(connection)

    bus_type = Gio.BusType.SESSION if os.environ.get('ZTOS_IAL_SESSION') else Gio.BusType.SYSTEM
    Gio.bus_own_name(bus_type, BUS_NAME, Gio.BusNameOwnerFlags.NONE, bus, acquired, lost)
    loop.run()
