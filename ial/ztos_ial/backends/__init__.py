"""What a backend is: one class per interface, its properties as a dict and its
methods as m_<Name> functions. ztos-iald turns them into D-Bus.

A vendor backend is a Python file in the device layer (manifest:
module = "/vendor/ztos/ial/<file>.py") with a class named after the interface
without its version (Power, Display...), usually a subclass of the generic one.
"""
import glob
import os


class NotSupported(Exception):
    """The call is valid but this phone cannot do it (org.ztos.ial.Error.NotSupported)."""


class Backend:
    interface = ''      # 'Power1'
    version = 1         # the minor version served (the Version property)
    poll_s = 0          # how often properties are compared for change signals (0: never)

    def __init__(self, config, manifest):
        self.config = config        # this interface's [ial.X] table
        self.manifest = manifest    # the whole manifest

    def props(self):
        return {}

    def available(self):
        """Whether the hardware behind this interface is there (Device1's
        Capabilities): a generic Modem1 is served on every phone, but a phone
        has the "modem" capability only when ModemManager finds one."""
        return True

    def call(self, method, args):
        handler = getattr(self, 'm_' + method, None)
        if handler is None:
            raise NotSupported(method)
        return handler(*args)


def read(path, default=None):
    try:
        with open(path) as stream:
            return stream.read().strip()
    except OSError:
        return default


def read_int(path, default=0):
    text = read(path)
    try:
        return int(text)
    except (TypeError, ValueError):
        return default


def write(path, value):
    with open(path, 'w') as stream:
        stream.write(str(value))


def first(pattern):
    found = sorted(glob.glob(pattern))
    return found[0] if found else None


def exists(path):
    return bool(path) and os.path.exists(path)
