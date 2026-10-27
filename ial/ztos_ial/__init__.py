"""ztOSIAL: the interfaces between the ztOS system image and a phone's own hardware.

The design is design/ztOSIAL.md. The interfaces are in ial/spec/, one
introspection file each; ztos-iald serves them on the system bus as
org.ztos.ial, at /org/ztos/ial/<Area>, from the backends the phone's manifest
(/vendor/ztos/manifest.toml) names.

Licence: LGPL-2.1-or-later.
"""

BUS_NAME = 'org.ztos.ial'
OBJECT_ROOT = '/org/ztos/ial'
PREFIX = 'org.ztos.ial.'

# Every interface of version 1, and whether a GSI needs it to boot
INTERFACES = {
    'Device1': True, 'Power1': True, 'Perf1': True, 'Display1': True, 'Sleep1': True,
    'Sensors1': False, 'Haptics1': False, 'Lights1': False, 'Modem1': False,
    'CallAudio1': False, 'AudioDevice1': False, 'Accel1': False, 'Android1': False,
}


def object_path(interface):
    """'Power1' -> '/org/ztos/ial/Power'"""
    return OBJECT_ROOT + '/' + interface.rstrip('0123456789')
