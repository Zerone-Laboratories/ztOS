"""Calling ztOSIAL from Python (the SDK's ztos.ial uses this).

    from ztos_ial import client
    client.get('Power1', 'Capacity')          # 87
    client.call('Haptics1', 'Play', 'tap')
    client.has('Modem1')                      # False on a phone without one
"""
import os

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

from . import BUS_NAME, PREFIX, object_path  # noqa: E402

_bus = None


class Error(Exception):
    """A ztOSIAL error; .name is the D-Bus error name (org.ztos.ial.Error.NotSupported...)."""

    def __init__(self, name, message):
        super().__init__(message)
        self.name = name


def bus():
    global _bus
    if _bus is None:
        kind = Gio.BusType.SESSION if os.environ.get('ZTOS_IAL_SESSION') else Gio.BusType.SYSTEM
        _bus = Gio.bus_get_sync(kind, None)
    return _bus


def _run(path, interface, method, params, reply_type=None, timeout=10000):
    try:
        return bus().call_sync(BUS_NAME, path, interface, method, params,
                               GLib.VariantType(reply_type) if reply_type else None,
                               Gio.DBusCallFlags.NONE, timeout, None)
    except GLib.Error as error:
        name = Gio.DBusError.get_remote_error(error) or 'org.ztos.ial.Error.Failed'
        Gio.DBusError.strip_remote_error(error)
        raise Error(name, error.message) from None


def get(interface, prop):
    reply = _run(object_path(interface), 'org.freedesktop.DBus.Properties', 'Get',
                 GLib.Variant('(ss)', (PREFIX + interface, prop)), '(v)')
    return reply.unpack()[0]


def get_all(interface):
    reply = _run(object_path(interface), 'org.freedesktop.DBus.Properties', 'GetAll',
                 GLib.Variant('(s)', (PREFIX + interface,)), '(a{sv})')
    return reply.unpack()[0]


def signature(interface, method):
    """The method's in-arguments, from the running daemon's introspection."""
    reply = _run(object_path(interface), 'org.freedesktop.DBus.Introspectable', 'Introspect', None, '(s)')
    info = Gio.DBusNodeInfo.new_for_xml(reply.unpack()[0]).lookup_interface(PREFIX + interface)
    found = info.lookup_method(method) if info else None
    if found is None:
        raise Error('org.freedesktop.DBus.Error.UnknownMethod', f'{interface}.{method}')
    return ''.join(a.signature for a in found.in_args)


def call(interface, method, *args, timeout=30000):
    sig = signature(interface, method)
    params = GLib.Variant('(' + sig + ')', args) if sig else None
    reply = _run(object_path(interface), PREFIX + interface, method, params, timeout=timeout)
    values = reply.unpack() if reply is not None else ()
    return values[0] if len(values) == 1 else (values or None)


def has(interface):
    try:
        return interface in get('Device1', 'Interfaces')
    except Error:
        return False
